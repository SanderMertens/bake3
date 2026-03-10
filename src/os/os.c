#include "bake/os.h"

#if defined(_WIN32)
#include <windows.h>
#endif

static const char* bake_errno_message(int err) {
    const char *msg = strerror(err);
    return (msg && msg[0]) ? msg : "unknown error";
}

void bake_log_errno(const char *action, const char *path, int err) {
    const char *msg = bake_errno_message(err);
    const char *verb = (action && action[0]) ? action : "access path";

    if (path && path[0]) {
        ecs_err("failed to %s '%s': %s (%d)", verb, path, msg, err);
    } else {
        ecs_err("failed to %s: %s (%d)", verb, msg, err);
    }
}

void bake_log_last_errno(const char *action, const char *path) {
    bake_log_errno(action, path, errno);
}

#if defined(_WIN32)
void bake_log_win_error(const char *action, const char *path, unsigned long err) {
    char *msg_buf = NULL;
    DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        err,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&msg_buf,
        0,
        NULL);

    if (size > 0) {
        while (size > 0 && (msg_buf[size - 1] == '\r' || msg_buf[size - 1] == '\n')) {
            msg_buf[--size] = '\0';
        }
    }

    const char *msg = (size > 0 && msg_buf) ? msg_buf : "unknown error";
    const char *verb = (action && action[0]) ? action : "access path";

    if (path && path[0]) {
        ecs_err("failed to %s '%s': %s (%lu)", verb, path, msg, err);
    } else {
        ecs_err("failed to %s: %s (%lu)", verb, msg, err);
    }

    if (msg_buf) {
        LocalFree(msg_buf);
    }
}

void bake_log_last_win_error(const char *action, const char *path) {
    bake_log_win_error(action, path, GetLastError());
}
#endif

int bake_remove_file(const char *path) {
    if (!path || !path[0]) {
        ecs_err("failed to remove file: invalid path");
        return -1;
    }

    if (remove(path) != 0) {
        bake_log_last_errno("remove file", path);
        return -1;
    }

    return 0;
}

int bake_remove_file_if_exists(const char *path) {
    if (!path || !path[0] || !bake_path_exists(path)) {
        return 0;
    }

    return bake_remove_file(path);
}

int bake_rename_file(const char *src, const char *dst) {
    if (!src || !src[0] || !dst || !dst[0]) {
        ecs_err("failed to rename file: invalid path");
        return -1;
    }

    if (rename(src, dst) != 0) {
        int err = errno;
        ecs_err(
            "failed to rename '%s' to '%s': %s (%d)",
            src,
            dst,
            bake_errno_message(err),
            err);
        return -1;
    }

    return 0;
}

const char* bake_host_os(void) {
#if defined(__APPLE__)
    return "Darwin";
#elif defined(__linux__)
    return "Linux";
#elif defined (_WIN32)
#else
    return "Windows";
#endif
}

const char* bake_host_arch(void) {
#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(__amd64__) || defined(_M_X64)
    return "x64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#else
    return "unknown";
#endif
}

char* bake_host_triplet(const char *mode) {
    const char *cfg = mode && mode[0] ? mode : "debug";
    return flecs_asprintf("%s-%s-%s", bake_host_arch(), bake_host_os(), cfg);
}

char* bake_host_platform(void) {
    return flecs_asprintf("%s-%s", bake_host_arch(), bake_host_os());
}
