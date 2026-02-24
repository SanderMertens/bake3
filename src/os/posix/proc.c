#if !defined(_WIN32)

#include "bake2/os.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t bake_proc_interrupted = 0;
static bool bake_proc_handler_installed = false;

static void bake_proc_sigint_handler(int sig) {
    BAKE_UNUSED(sig);
    bake_proc_interrupted = 1;
}

static void bake_proc_install_signal_handler(void) {
    if (bake_proc_handler_installed) {
        return;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = bake_proc_sigint_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    bake_proc_handler_installed = true;
}

static int bake_proc_redirect_file(const char *path, int fd, int flags) {
    int file = open(path, flags, 0644);
    if (file < 0) {
        return -1;
    }

    if (dup2(file, fd) < 0) {
        close(file);
        return -1;
    }

    close(file);
    return 0;
}

int bake_proc_run(
    const char *const *argv,
    const bake_process_stdio_t *stdio_cfg,
    bake_process_result_t *result)
{
    if (!argv || !argv[0]) {
        return -1;
    }

    bake_proc_install_signal_handler();

    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }

    if (pid == 0) {
        signal(SIGINT, SIG_DFL);

        if (stdio_cfg) {
            if (stdio_cfg->stdin_path && stdio_cfg->stdin_path[0]) {
                if (bake_proc_redirect_file(stdio_cfg->stdin_path, STDIN_FILENO, O_RDONLY) != 0) {
                    _exit(127);
                }
            }

            if (stdio_cfg->stdout_path && stdio_cfg->stdout_path[0]) {
                int flags = O_WRONLY | O_CREAT |
                    (stdio_cfg->stdout_append ? O_APPEND : O_TRUNC);
                if (bake_proc_redirect_file(stdio_cfg->stdout_path, STDOUT_FILENO, flags) != 0) {
                    _exit(127);
                }
            }

            if (stdio_cfg->stderr_path && stdio_cfg->stderr_path[0]) {
                int flags = O_WRONLY | O_CREAT |
                    (stdio_cfg->stderr_append ? O_APPEND : O_TRUNC);
                if (bake_proc_redirect_file(stdio_cfg->stderr_path, STDERR_FILENO, flags) != 0) {
                    _exit(127);
                }
            }
        }

        execvp(argv[0], (char *const*)argv);
        _exit(127);
    }

    int status = 0;
    for (;;) {
        if (bake_proc_interrupted) {
            kill(pid, SIGINT);
        }

        pid_t rc = waitpid(pid, &status, WNOHANG);
        if (rc == pid) {
            break;
        }

        if (rc == 0) {
            usleep(10000);
            continue;
        }

        if (errno == EINTR) {
            continue;
        }

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

        if (bake_proc_interrupted || result->term_signal == SIGINT) {
            result->interrupted = true;
            bake_proc_interrupted = 1;
        }
    }

    return 0;
}

int bake_proc_run_argv(const char *const *argv, bake_process_result_t *result) {
    return bake_proc_run(argv, NULL, result);
}

bool bake_proc_was_interrupted(void) {
    return bake_proc_interrupted != 0;
}

void bake_proc_clear_interrupt(void) {
    bake_proc_interrupted = 0;
}

#endif

#if defined(_WIN32)
typedef int bake_posix_proc_dummy_t;
#endif
