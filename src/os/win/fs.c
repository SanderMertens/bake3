#if defined(_WIN32)

#include "bake/os.h"
#include <flecs.h>

#include <ctype.h>
#include <direct.h>
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
    struct _stat st;
    if (_stat(path, &st) != 0) {
        return -1;
    }
    return (int64_t)st.st_mtime;
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
    char buf[MAX_PATH];
    if (!_getcwd(buf, (int)sizeof(buf))) {
        bake_log_errno_last("get current directory", NULL);
        return NULL;
    }
    return ecs_os_strdup(buf);
}

int bake_os_rmdir(const char *path) {
    return _rmdir(path);
}

#endif

#if !defined(_WIN32)
typedef int bake_os_win_fs_dummy_t;
#endif
