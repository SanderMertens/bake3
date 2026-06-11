#include "bake/os.h"
#include <flecs.h>

#if defined(_WIN32)
#include <windows.h>
#include <direct.h>
#else
#include <unistd.h>
#include <sys/types.h>
#endif

static const char* bake_errno_message(int err, char *buf, size_t size) {
#if defined(_WIN32)
    if (strerror_s(buf, size, err) != 0) {
        snprintf(buf, size, "errno %d", err);
    }
#else
    if (strerror_r(err, buf, size) != 0) {
        snprintf(buf, size, "errno %d", err);
    }
#endif
    return buf;
}

void bake_log_errno(const char *action, const char *path, int err) {
    char msgbuf[256];
    const char *msg = bake_errno_message(err, msgbuf, sizeof(msgbuf));
    const char *verb = (action && action[0]) ? action : "access path";

    if (path && path[0]) {
        ecs_err("failed to %s '%s': %s (%d)", verb, path, msg, err);
    } else {
        ecs_err("failed to %s: %s (%d)", verb, msg, err);
    }
}

void bake_log_errno_last(const char *action, const char *path) {
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

void bake_log_win_error_last(const char *action, const char *path) {
    bake_log_win_error(action, path, GetLastError());
}
#endif

int32_t bake_host_threads(void) {
    int32_t cpu = bake_os_cpu_count();
    if (cpu < 1) {
        return 1;
    }
    if (cpu > 128) {
        return 128;
    }
    return cpu;
}

char* bake_os_getcwd(void) {
    size_t size = 256;

    for (;;) {
        char *buf = ecs_os_malloc(size);

#if defined(_WIN32)
        if (_getcwd(buf, (int)size)) {
            return buf;
        }
#else
        if (getcwd(buf, size)) {
            return buf;
        }
#endif

        int err = errno;
        ecs_os_free(buf);
        if (err != ERANGE) {
            bake_log_errno("get current directory", NULL, err);
            return NULL;
        }

        if (size > (SIZE_MAX / 2)) {
            ecs_err("failed to get current directory: path is too long");
            return NULL;
        }

        size *= 2;
    }
}

int bake_remove_file(const char *path) {
    if (!path || !path[0]) {
        ecs_err("failed to remove file: invalid path");
        return -1;
    }

    if (remove(path) != 0) {
        bake_log_errno_last("remove file", path);
        return -1;
    }

    return 0;
}

int bake_remove_file_if_exists(const char *path) {
    if (!path || !path[0]) {
        return 0;
    }

    if (!bake_path_is_symlink(path) && !bake_path_exists(path)) {
        return 0;
    }

    return bake_remove_file(path);
}

const char* bake_host_os(void) {
#if defined(__APPLE__)
    return "Darwin";
#elif defined(__linux__)
    return "Linux";
#elif defined(_WIN32)
    return "Windows";
#else
    return "unknown";
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

static const char *bake_active_target = NULL;
static const char *bake_target_arch_override = NULL;
static const char *bake_target_os_override = NULL;
static const char *bake_target_exe_ext_str = "";

bool bake_target_name_is_em(const char *name) {
    return name && (!strcmp(name, "em") || !strcmp(name, "emscripten") ||
        !strcmp(name, "wasm"));
}

void bake_set_build_target(const char *target) {
    if (bake_target_name_is_em(target)) {
        bake_active_target = "em";
        bake_target_arch_override = "wasm32";
        bake_target_os_override = "Emscripten";
        bake_target_exe_ext_str = ".js";
    } else {
        bake_active_target = (target && target[0]) ? target : NULL;
        bake_target_arch_override = NULL;
        bake_target_os_override = NULL;
        bake_target_exe_ext_str = "";
    }
}

bool bake_target_is_emscripten(void) {
    return bake_active_target && !strcmp(bake_active_target, "em");
}

const char* bake_target_arch(void) {
    return bake_target_arch_override ? bake_target_arch_override : bake_host_arch();
}

const char* bake_target_os(void) {
    return bake_target_os_override ? bake_target_os_override : bake_host_os();
}

const char* bake_target_exe_ext(void) {
    return bake_target_exe_ext_str;
}

char* bake_host_triplet(const char *mode) {
    const char *cfg = mode && mode[0] ? mode : "debug";
    return flecs_asprintf("%s-%s-%s", bake_target_arch(), bake_target_os(), cfg);
}

char* bake_host_platform(void) {
    return flecs_asprintf("%s-%s", bake_target_arch(), bake_target_os());
}

#if !defined(_WIN32)
static bool bake_exe_in_path(const char *exe) {
    const char *path = getenv("PATH");
    if (!path || !path[0]) {
        return false;
    }

    const char *start = path;
    while (*start) {
        const char *sep = strchr(start, ':');
        size_t len = sep ? (size_t)(sep - start) : strlen(start);
        if (len) {
            char *dir = ecs_os_malloc(len + 1);
            memcpy(dir, start, len);
            dir[len] = '\0';
            char *full = bake_path_join(dir, exe);
            ecs_os_free(dir);
            if (!access(full, X_OK)) {
                ecs_os_free(full);
                return true;
            }
            ecs_os_free(full);
        }
        if (!sep) {
            break;
        }
        start = sep + 1;
    }

    return false;
}

static char* bake_find_emsdk_root(void) {
    const char *candidates[3];
    int32_t count = 0;
    const char *env_emsdk = getenv("EMSDK");
    const char *env_emsdk_dir = getenv("EMSDK_DIR");
    if (env_emsdk && env_emsdk[0]) candidates[count++] = env_emsdk;
    if (env_emsdk_dir && env_emsdk_dir[0]) candidates[count++] = env_emsdk_dir;

    char *home_default = NULL;
    char *home = bake_os_home_path();
    if (home) {
        home_default = bake_path_join(home, "GitHub/emsdk");
        ecs_os_free(home);
        candidates[count++] = home_default;
    }

    char *result = NULL;
    for (int32_t i = 0; i < count; i++) {
        char *script = bake_path_join(candidates[i], "emsdk_env.sh");
        if (bake_path_exists(script)) {
            result = ecs_os_strdup(candidates[i]);
            ecs_os_free(script);
            break;
        }
        ecs_os_free(script);
    }

    ecs_os_free(home_default);
    return result;
}

int bake_emsdk_ensure_env(void) {
    if (bake_exe_in_path("emcc")) {
        return 0;
    }

    char *root = bake_find_emsdk_root();
    if (!root) {
        ecs_err("emcc not found and no emsdk installation located "
            "(set EMSDK or EMSDK_DIR, or install at ~/GitHub/emsdk)");
        return -1;
    }

    ecs_trace("#[green][#[normal] emsdk#[green]]#[normal] activating %s", root);

    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || !tmpdir[0]) {
        tmpdir = "/tmp";
    }
    char *out_path = flecs_asprintf("%s/bake_emsdk_env.%d",
        tmpdir, (int)getpid());
    char *script = flecs_asprintf(
        ". \"%s/emsdk_env.sh\" > /dev/null 2>&1; env", root);

    bake_process_stdio_t stdio_cfg = { .stdout_path = out_path };
    const char *argv[] = { "sh", "-c", script, NULL };
    bake_process_result_t res = {0};
    int rc = bake_proc_run(argv, &stdio_cfg, &res);
    ecs_os_free(script);
    ecs_os_free(root);

    if (rc != 0 || res.exit_code != 0) {
        ecs_err("failed to source emsdk environment");
        bake_remove_file_if_exists(out_path);
        ecs_os_free(out_path);
        return -1;
    }

    size_t len = 0;
    char *env_dump = bake_file_read(out_path, &len);
    bake_remove_file_if_exists(out_path);
    ecs_os_free(out_path);
    if (!env_dump) {
        return -1;
    }

    char *line = env_dump;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        char *eq = strchr(line, '=');
        if (eq) {
            *eq = '\0';
            const char *name = line;
            const char *value = eq + 1;
            if (!strcmp(name, "PATH") || !strncmp(name, "EM", 2)) {
                bake_os_setenv(name, value);
            }
        }

        if (!nl) break;
        line = nl + 1;
    }

    ecs_os_free(env_dump);

    if (!bake_exe_in_path("emcc")) {
        ecs_err("emsdk activated but emcc still not found on PATH");
        return -1;
    }

    return 0;
}
#else
int bake_emsdk_ensure_env(void) {
    ecs_err("emscripten target is not supported on Windows");
    return -1;
}
#endif
