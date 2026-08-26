#if !defined(_WIN32)

#include "bake/os.h"
#include "bake/ps.h"
#include <flecs.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#if defined(__APPLE__) && defined(__MAC_OS_X_VERSION_MIN_REQUIRED) && \
    __MAC_OS_X_VERSION_MIN_REQUIRED >= 260000
#define BAKE_SPAWN_HAS_CHDIR
#define bake_spawn_addchdir posix_spawn_file_actions_addchdir
#elif defined(__APPLE__) || \
    (defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 29)))
#define BAKE_SPAWN_HAS_CHDIR
#define bake_spawn_addchdir posix_spawn_file_actions_addchdir_np
#endif

/* posix_spawn instead of fork/exec: bake spawns compilers from worker
 * threads, and code between fork and exec in a multithreaded process is
 * limited to async-signal-safe calls. */
int bake_proc_run(
    const char *const *argv,
    const bake_process_stdio_t *stdio_cfg,
    bake_process_result_t *result)
{
    if (!argv || !argv[0]) {
        return -1;
    }

    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    if (posix_spawn_file_actions_init(&fa) != 0) {
        return -1;
    }
    if (posix_spawnattr_init(&attr) != 0) {
        posix_spawn_file_actions_destroy(&fa);
        return -1;
    }

    int err = 0;
    if (stdio_cfg) {
#ifdef BAKE_SPAWN_HAS_CHDIR
        if (stdio_cfg->cwd && stdio_cfg->cwd[0]) {
            err = bake_spawn_addchdir(&fa, stdio_cfg->cwd);
        }
#endif

        if (!err && stdio_cfg->stdin_path && stdio_cfg->stdin_path[0]) {
            err = posix_spawn_file_actions_addopen(
                &fa, STDIN_FILENO, stdio_cfg->stdin_path, O_RDONLY, 0);
        }

        if (!err && stdio_cfg->stdout_path && stdio_cfg->stdout_path[0]) {
            int flags = O_WRONLY | O_CREAT |
                (stdio_cfg->stdout_append ? O_APPEND : O_TRUNC);
            err = posix_spawn_file_actions_addopen(
                &fa, STDOUT_FILENO, stdio_cfg->stdout_path, flags, 0644);
        }

        if (!err && stdio_cfg->stderr_path && stdio_cfg->stderr_path[0]) {
            int flags = O_WRONLY | O_CREAT |
                (stdio_cfg->stderr_append ? O_APPEND : O_TRUNC);
            err = posix_spawn_file_actions_addopen(
                &fa, STDERR_FILENO, stdio_cfg->stderr_path, flags, 0644);
        }

        if (!err && stdio_cfg->stderr_to_stdout) {
            err = posix_spawn_file_actions_adddup2(
                &fa, STDOUT_FILENO, STDERR_FILENO);
        }
    }

    sigset_t sig_default;
    sigemptyset(&sig_default);
    sigaddset(&sig_default, SIGINT);
    if (!err) {
        err = posix_spawnattr_setsigdefault(&attr, &sig_default);
    }
    if (!err) {
        err = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF);
    }

    char *prev_cwd = NULL;
#ifndef BAKE_SPAWN_HAS_CHDIR
    if (!err && stdio_cfg && stdio_cfg->cwd && stdio_cfg->cwd[0]) {
        prev_cwd = bake_os_getcwd();
        if (!prev_cwd || chdir(stdio_cfg->cwd) != 0) {
            bake_log_errno_last("change directory", stdio_cfg->cwd);
            ecs_os_free(prev_cwd);
            prev_cwd = NULL;
            err = EINVAL;
        }
    }
#endif

    pid_t pid = 0;
    if (!err) {
        err = posix_spawnp(&pid, argv[0], &fa, &attr, (char *const*)argv, environ);
    }

    if (prev_cwd) {
        if (chdir(prev_cwd) != 0) {
            bake_log_errno_last("change directory", prev_cwd);
        }
        ecs_os_free(prev_cwd);
    }

    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);

    if (err) {
        bake_log_errno("start command", argv[0], err);
        return -1;
    }

    bool registered = false;
    if (stdio_cfg && stdio_cfg->ps) {
        registered = bake_ps_register(
            (int64_t)pid, argv, (const bake_ps_info_t*)stdio_cfg->ps) == 0;
    }

    int status = 0;
    for (;;) {
        pid_t rc = waitpid(pid, &status, 0);
        if (rc == pid) {
            break;
        }

        if (rc < 0 && errno == EINTR) {
            continue;
        }

        if (registered) {
            bake_ps_unregister((int64_t)pid);
        }

        bake_log_errno_last("wait for command", argv[0]);
        return -1;
    }

    if (registered) {
        bake_ps_unregister((int64_t)pid);
    }

    if (result) {
        result->exit_code = 0;
        result->term_signal = 0;
        result->interrupted = false;

        if (WIFEXITED(status)) {
            result->exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            result->term_signal = WTERMSIG(status);
            result->exit_code = 128 + result->term_signal;
        }

        if (result->term_signal == SIGINT) {
            result->interrupted = true;
        }
    }

    return 0;
}

int bake_proc_run_argv(const char *const *argv, bake_process_result_t *result) {
    return bake_proc_run(argv, NULL, result);
}

#define BAKE_PS_SNAPSHOT_CMD \
    "ps -axww -o pid=,ppid=,uid=,state=,etime=,command= 2>/dev/null"

static const char* bake_proc_scan_field(const char *cursor, char *buf, size_t size) {
    while (*cursor == ' ' || *cursor == '\t') {
        cursor ++;
    }

    size_t len = 0;
    while (*cursor && *cursor != ' ' && *cursor != '\t') {
        if (len < (size - 1)) {
            buf[len] = *cursor;
        }
        len ++;
        cursor ++;
    }

    buf[len < (size - 1) ? len : (size - 1)] = '\0';
    return cursor;
}

int bake_proc_snapshot(bake_proc_info_t **procs_out, int32_t *count_out) {
    if (!procs_out || !count_out) {
        return -1;
    }

    *procs_out = NULL;
    *count_out = 0;

    FILE *stream = popen(BAKE_PS_SNAPSHOT_CMD, "r");
    if (!stream) {
        return -1;
    }

    ecs_vec_t vec = {0};
    char line[8192];
    while (fgets(line, sizeof(line), stream)) {
        char pid[32], ppid[32], uid[32], state[32], etime[64];
        const char *cursor = line;
        cursor = bake_proc_scan_field(cursor, pid, sizeof(pid));
        cursor = bake_proc_scan_field(cursor, ppid, sizeof(ppid));
        cursor = bake_proc_scan_field(cursor, uid, sizeof(uid));
        cursor = bake_proc_scan_field(cursor, state, sizeof(state));
        cursor = bake_proc_scan_field(cursor, etime, sizeof(etime));

        while (*cursor == ' ' || *cursor == '\t') {
            cursor ++;
        }

        size_t cmd_len = strlen(cursor);
        while (cmd_len && (cursor[cmd_len - 1] == '\n' || cursor[cmd_len - 1] == '\r')) {
            cmd_len --;
        }

        if (!pid[0] || !cmd_len) {
            continue;
        }

        bake_proc_info_t *info = ecs_vec_append_t(NULL, &vec, bake_proc_info_t);
        info->pid = (int64_t)strtoll(pid, NULL, 10);
        info->parent_pid = (int64_t)strtoll(ppid, NULL, 10);
        info->uid = (int64_t)strtoll(uid, NULL, 10);
        info->elapsed_sec = bake_ps_parse_etime(etime);
        info->state = state[0];
        info->cmd = ecs_os_malloc((ecs_size_t)cmd_len + 1);
        ecs_os_memcpy(info->cmd, cursor, (ecs_size_t)cmd_len);
        info->cmd[cmd_len] = '\0';
    }

    if (pclose(stream) == -1 && !ecs_vec_count(&vec)) {
        ecs_vec_fini_t(NULL, &vec, bake_proc_info_t);
        return -1;
    }

    int32_t count = ecs_vec_count(&vec);
    if (count) {
        bake_proc_info_t *items = ecs_os_malloc_n(bake_proc_info_t, count);
        ecs_os_memcpy_n(items, ecs_vec_first_t(&vec, bake_proc_info_t),
            bake_proc_info_t, count);
        *procs_out = items;
    }

    ecs_vec_fini_t(NULL, &vec, bake_proc_info_t);
    *count_out = count;
    return 0;
}

void bake_proc_snapshot_free(bake_proc_info_t *procs, int32_t count) {
    for (int32_t i = 0; i < count; i ++) {
        ecs_os_free(procs[i].cmd);
    }
    ecs_os_free(procs);
}

#endif

#if defined(_WIN32)
typedef int bake_posix_proc_dummy_t;
#endif
