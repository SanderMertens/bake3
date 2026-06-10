#if !defined(_WIN32)

#include "bake/os.h"
#include <flecs.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

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
        if (stdio_cfg->stdin_path && stdio_cfg->stdin_path[0]) {
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

    pid_t pid = 0;
    if (!err) {
        err = posix_spawnp(&pid, argv[0], &fa, &attr, (char *const*)argv, environ);
    }

    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);

    if (err) {
        bake_log_errno("start command", argv[0], err);
        return -1;
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

        bake_log_errno_last("wait for command", argv[0]);
        return -1;
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

#endif

#if defined(_WIN32)
typedef int bake_posix_proc_dummy_t;
#endif
