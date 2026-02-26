#if !defined(_WIN32)

#include "bake/os.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

char bake_path_sep(void) {
    return '/';
}

const char* bake_path_last_sep(const char *path) {
    if (!path) {
        return NULL;
    }
    return strrchr(path, '/');
}

bool bake_path_is_abs(const char *path) {
    return path && path[0] == '/';
}

int bake_path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int64_t bake_os_file_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
#if defined(__APPLE__)
    return (int64_t)st.st_mtimespec.tv_sec * 1000000000LL +
        (int64_t)st.st_mtimespec.tv_nsec;
#else
    return (int64_t)st.st_mtim.tv_sec * 1000000000LL + (int64_t)st.st_mtim.tv_nsec;
#endif
}

int bake_path_is_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return S_ISDIR(st.st_mode);
}

int bake_os_mkdir(const char *path) {
    return mkdir(path, 0755);
}

char* bake_os_getcwd(void) {
    char buf[PATH_MAX];
    if (!getcwd(buf, sizeof(buf))) {
        return NULL;
    }
    return ecs_os_strdup(buf);
}

int bake_os_rmdir(const char *path) {
    return rmdir(path);
}

#endif

#if defined(_WIN32)
typedef int bake_os_posix_fs_dummy_t;
#endif
