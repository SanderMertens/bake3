#if defined(_WIN32)

#include "bake/os.h"
#include <flecs.h>

#include <ctype.h>
#include <direct.h>
#include <errno.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>

char bake_path_sep(void) {
    return '\\';
}

const char* bake_path_last_sep(const char *path) {
    if (!path) {
        return NULL;
    }

    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    if (!slash || (backslash && backslash > slash)) {
        slash = backslash;
    }
    return slash;
}

bool bake_path_is_abs(const char *path) {
    if (!path || !path[0]) {
        return false;
    }
    if (path[0] == '\\' || path[0] == '/') {
        return true;
    }
    return isalpha((unsigned char)path[0]) && path[1] == ':';
}

int bake_path_exists(const char *path) {
    struct _stat st;
    return _stat(path, &st) == 0;
}

int64_t bake_os_file_mtime(const char *path) {
    if (!path || !path[0]) {
        return -1;
    }

    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data)) {
        return -1;
    }

    ULARGE_INTEGER ft;
    ft.LowPart = data.ftLastWriteTime.dwLowDateTime;
    ft.HighPart = data.ftLastWriteTime.dwHighDateTime;

    int64_t epoch_100ns = 116444736000000000LL;
    int64_t since_epoch_100ns = (int64_t)ft.QuadPart - epoch_100ns;
    if (since_epoch_100ns < 0) {
        since_epoch_100ns = 0;
    }
    return since_epoch_100ns * 100LL;
}

int bake_file_sync_mode(const char *src, const char *dst) {
    if (!src || !dst) {
        return -1;
    }

    struct _stat st;
    if (_stat(src, &st) != 0) {
        bake_log_errno_last("stat file", src);
        return -1;
    }

    int mode = st.st_mode & (_S_IREAD | _S_IWRITE);
#ifdef _S_IEXEC
    mode |= st.st_mode & _S_IEXEC;
#endif

    if (_chmod(dst, mode) != 0) {
        bake_log_errno_last("set file mode", dst);
        return -1;
    }

    return 0;
}

int bake_path_is_dir(const char *path) {
    struct _stat st;
    if (_stat(path, &st) != 0) {
        return 0;
    }
    return (st.st_mode & _S_IFDIR) != 0;
}

int bake_os_mkdir(const char *path) {
    return _mkdir(path);
}

char* bake_os_getcwd(void) {
    size_t size = 256;

    for (;;) {
        char *buf = ecs_os_malloc((int32_t)size);
        if (!buf) {
            return NULL;
        }

        if (_getcwd(buf, (int)size)) {
            return buf;
        }

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

int bake_os_rmdir(const char *path) {
    return _rmdir(path);
}

#endif

#if !defined(_WIN32)
typedef int bake_os_win_fs_dummy_t;
#endif
