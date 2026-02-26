#if defined(_WIN32)

#include "bake/os.h"

#include <windows.h>

static volatile LONG bake_proc_interrupted = 0;
static bool bake_proc_handler_installed = false;

static BOOL WINAPI bake_proc_console_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        InterlockedExchange(&bake_proc_interrupted, 1);
        return TRUE;
    }
    return FALSE;
}

static void bake_proc_install_signal_handler(void) {
    if (bake_proc_handler_installed) {
        return;
    }

    SetConsoleCtrlHandler(bake_proc_console_handler, TRUE);
    bake_proc_handler_installed = true;
}

static char* bake_proc_quote_arg(const char *arg) {
    if (!arg) {
        return ecs_os_strdup("\"\"");
    }

    bool needs_quote = false;
    for (const char *p = arg; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '"') {
            needs_quote = true;
            break;
        }
    }

    if (!needs_quote) {
        return ecs_os_strdup(arg);
    }

    size_t len = strlen(arg);
    char *out = ecs_os_malloc((len * 2) + 3);
    if (!out) {
        return NULL;
    }

    size_t w = 0;
    out[w++] = '"';
    for (size_t i = 0; i < len; i++) {
        if (arg[i] == '"') {
            out[w++] = '\\';
        }
        out[w++] = arg[i];
    }
    out[w++] = '"';
    out[w] = '\0';
    return out;
}

static char* bake_proc_join_command_line(const char *const *argv) {
    ecs_strbuf_t buf = ECS_STRBUF_INIT;
    for (int i = 0; argv[i]; i++) {
        char *q = bake_proc_quote_arg(argv[i]);
        if (!q) {
            char *tmp = ecs_strbuf_get(&buf);
            ecs_os_free(tmp);
            return NULL;
        }
        ecs_strbuf_append(&buf, "%s%s", i ? " " : "", q);
        ecs_os_free(q);
    }
    return ecs_strbuf_get(&buf);
}

static HANDLE bake_proc_dup_inheritable(HANDLE src) {
    if (!src || src == INVALID_HANDLE_VALUE) {
        return NULL;
    }

    HANDLE out = NULL;
    if (!DuplicateHandle(
        GetCurrentProcess(),
        src,
        GetCurrentProcess(),
        &out,
        0,
        TRUE,
        DUPLICATE_SAME_ACCESS))
    {
        return NULL;
    }

    return out;
}

static HANDLE bake_proc_open_redirect(
    const char *path,
    bool write_mode,
    bool append_mode)
{
    if (!path || !path[0]) {
        return NULL;
    }

    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    DWORD access = write_mode ? GENERIC_WRITE : GENERIC_READ;
    DWORD creation = write_mode
        ? (append_mode ? OPEN_ALWAYS : CREATE_ALWAYS)
        : OPEN_EXISTING;

    HANDLE h = CreateFileA(
        path,
        access,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa,
        creation,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    if (h == INVALID_HANDLE_VALUE) {
        return NULL;
    }

    if (write_mode && append_mode) {
        SetFilePointer(h, 0, NULL, FILE_END);
    }

    return h;
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

    char *cmd_line = bake_proc_join_command_line(argv);
    if (!cmd_line) {
        return -1;
    }

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);

    HANDLE std_in = NULL;
    HANDLE std_out = NULL;
    HANDLE std_err = NULL;
    BOOL inherit_handles = FALSE;

    if (stdio_cfg) {
        std_in = bake_proc_open_redirect(stdio_cfg->stdin_path, false, false);
        std_out = bake_proc_open_redirect(
            stdio_cfg->stdout_path,
            true,
            stdio_cfg->stdout_append);
        std_err = bake_proc_open_redirect(
            stdio_cfg->stderr_path,
            true,
            stdio_cfg->stderr_append);

        if (!std_in) {
            std_in = bake_proc_dup_inheritable(GetStdHandle(STD_INPUT_HANDLE));
        }
        if (!std_out) {
            std_out = bake_proc_dup_inheritable(GetStdHandle(STD_OUTPUT_HANDLE));
        }
        if (!std_err) {
            std_err = bake_proc_dup_inheritable(GetStdHandle(STD_ERROR_HANDLE));
        }

        if (!std_in || !std_out || !std_err) {
            ecs_os_free(cmd_line);
            if (std_in) CloseHandle(std_in);
            if (std_out) CloseHandle(std_out);
            if (std_err) CloseHandle(std_err);
            return -1;
        }

        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdInput = std_in;
        si.hStdOutput = std_out;
        si.hStdError = std_err;
        inherit_handles = TRUE;
    }

    BOOL ok = CreateProcessA(
        NULL,
        cmd_line,
        NULL,
        NULL,
        inherit_handles,
        0,
        NULL,
        NULL,
        &si,
        &pi);
    ecs_os_free(cmd_line);

    if (std_in) CloseHandle(std_in);
    if (std_out) CloseHandle(std_out);
    if (std_err) CloseHandle(std_err);

    if (!ok) {
        return -1;
    }

    DWORD wait_rc = WAIT_TIMEOUT;
    for (;;) {
        wait_rc = WaitForSingleObject(pi.hProcess, 20);
        if (wait_rc == WAIT_OBJECT_0) {
            break;
        }
        if (wait_rc != WAIT_TIMEOUT) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return -1;
        }

        if (InterlockedCompareExchange(&bake_proc_interrupted, 0, 0)) {
            TerminateProcess(pi.hProcess, 130);
        }
    }

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(pi.hProcess, &exit_code)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return -1;
    }

    if (result) {
        result->exit_code = (int)exit_code;
        result->term_signal = 0;
        result->interrupted = InterlockedCompareExchange(&bake_proc_interrupted, 0, 0) != 0 ||
            exit_code == 130;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

int bake_proc_run_argv(const char *const *argv, bake_process_result_t *result) {
    return bake_proc_run(argv, NULL, result);
}

bool bake_proc_was_interrupted(void) {
    return InterlockedCompareExchange(&bake_proc_interrupted, 0, 0) != 0;
}

void bake_proc_clear_interrupt(void) {
    InterlockedExchange(&bake_proc_interrupted, 0);
}

#endif

#if !defined(_WIN32)
typedef int bake_win_proc_dummy_t;
#endif
