#if defined(_WIN32)

#include "bake2/os.h"

#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>

char bake_os_path_sep(void) {
    return '\\';
}

const char* bake_os_path_last_sep(const char *path) {
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

bool bake_os_path_is_abs(const char *path) {
    if (!path || !path[0]) {
        return false;
    }
    if (path[0] == '\\' || path[0] == '/') {
        return true;
    }
    return path[0] && path[1] == ':';
}

int bake_os_path_exists(const char *path) {
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

int bake_os_path_is_dir(const char *path) {
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
        return NULL;
    }
    return bake_strdup(buf);
}

int bake_os_rmdir(const char *path) {
    return _rmdir(path);
}

#endif

#if !defined(_WIN32)
typedef int bake_os_win_fs_dummy_t;
#endif
