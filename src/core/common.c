#include "bake2/common.h"
#include "bake2/os.h"

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <windows.h>
#define B2_STAT _stat
#define B2_MKDIR(path) _mkdir(path)
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define B2_STAT stat
#define B2_MKDIR(path) mkdir(path, 0755)
#endif

char* b2_strdup(const char *str) {
    if (!str) {
        return NULL;
    }
    size_t len = strlen(str);
    char *out = ecs_os_malloc(len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, str, len + 1);
    return out;
}

char* b2_join_path(const char *lhs, const char *rhs) {
    if (!lhs || !lhs[0]) {
        return b2_strdup(rhs);
    }
    if (!rhs || !rhs[0]) {
        return b2_strdup(lhs);
    }

    size_t lhs_len = strlen(lhs);
    size_t rhs_len = strlen(rhs);
    bool has_sep = lhs[lhs_len - 1] == B2_PATH_SEP;

    char *out = ecs_os_malloc(lhs_len + rhs_len + (has_sep ? 1 : 2));
    if (!out) {
        return NULL;
    }

    memcpy(out, lhs, lhs_len);
    out[lhs_len] = '\0';
    if (!has_sep) {
        out[lhs_len] = B2_PATH_SEP;
        out[lhs_len + 1] = '\0';
    }
    strcat(out, rhs);
    return out;
}

char* b2_join3_path(const char *a, const char *b, const char *c) {
    char *ab = b2_join_path(a, b);
    if (!ab) {
        return NULL;
    }
    char *abc = b2_join_path(ab, c);
    ecs_os_free(ab);
    return abc;
}

char* b2_asprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    if (needed < 0) {
        return NULL;
    }

    char *out = ecs_os_malloc((size_t)needed + 1);
    if (!out) {
        return NULL;
    }

    va_start(args, fmt);
    vsnprintf(out, (size_t)needed + 1, fmt, args);
    va_end(args);

    return out;
}

char* b2_read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }

    long len = ftell(f);
    if (len < 0) {
        fclose(f);
        return NULL;
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    char *buf = ecs_os_malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t read_len = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (read_len != (size_t)len) {
        ecs_os_free(buf);
        return NULL;
    }

    buf[len] = '\0';
    if (len_out) {
        *len_out = (size_t)len;
    }
    return buf;
}

int b2_write_file(const char *path, const char *content) {
    char *dir = b2_dirname(path);
    if (!dir) {
        return -1;
    }

    if (b2_mkdirs(dir) != 0) {
        ecs_os_free(dir);
        return -1;
    }
    ecs_os_free(dir);

    FILE *f = fopen(path, "wb");
    if (!f) {
        return -1;
    }

    size_t len = strlen(content);
    size_t written = fwrite(content, 1, len, f);
    fclose(f);
    return written == len ? 0 : -1;
}

int b2_path_exists(const char *path) {
    struct B2_STAT st;
    return B2_STAT(path, &st) == 0;
}

int b2_is_dir(const char *path) {
    struct B2_STAT st;
    if (B2_STAT(path, &st) != 0) {
        return 0;
    }
#if defined(_WIN32)
    return (st.st_mode & _S_IFDIR) != 0;
#else
    return S_ISDIR(st.st_mode);
#endif
}

int b2_mkdirs(const char *path) {
    if (!path || !path[0]) {
        return -1;
    }

    if (b2_path_exists(path)) {
        return 0;
    }

    char *tmp = b2_strdup(path);
    if (!tmp) {
        return -1;
    }

    size_t len = strlen(tmp);
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char prev = tmp[i];
            tmp[i] = '\0';
            if (tmp[0] && !b2_path_exists(tmp) && B2_MKDIR(tmp) != 0 && errno != EEXIST) {
                ecs_os_free(tmp);
                return -1;
            }
            tmp[i] = prev;
        }
    }

    if (!b2_path_exists(tmp) && B2_MKDIR(tmp) != 0 && errno != EEXIST) {
        ecs_os_free(tmp);
        return -1;
    }

    ecs_os_free(tmp);
    return 0;
}

char* b2_dirname(const char *path) {
    if (!path) {
        return NULL;
    }

    const char *slash = strrchr(path, '/');
#if defined(_WIN32)
    const char *backslash = strrchr(path, '\\');
    if (!slash || (backslash && backslash > slash)) {
        slash = backslash;
    }
#endif

    if (!slash) {
        return b2_strdup(".");
    }

    size_t len = (size_t)(slash - path);
    char *out = ecs_os_malloc(len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, path, len);
    out[len] = '\0';
    return out;
}

char* b2_basename(const char *path) {
    if (!path) {
        return NULL;
    }

    const char *slash = strrchr(path, '/');
#if defined(_WIN32)
    const char *backslash = strrchr(path, '\\');
    if (!slash || (backslash && backslash > slash)) {
        slash = backslash;
    }
#endif

    if (!slash) {
        return b2_strdup(path);
    }

    return b2_strdup(slash + 1);
}

char* b2_stem(const char *path) {
    char *base = b2_basename(path);
    if (!base) {
        return NULL;
    }

    char *dot = strrchr(base, '.');
    if (dot) {
        dot[0] = '\0';
    }

    return base;
}

char* b2_getcwd(void) {
#if defined(_WIN32)
    char buf[MAX_PATH];
    if (!_getcwd(buf, (int)sizeof(buf))) {
        return NULL;
    }
#else
    char buf[PATH_MAX];
    if (!getcwd(buf, sizeof(buf))) {
        return NULL;
    }
#endif
    return b2_strdup(buf);
}

int b2_remove_tree(const char *path) {
    if (!b2_path_exists(path)) {
        return 0;
    }

    if (!b2_is_dir(path)) {
        return remove(path);
    }

    b2_dir_entry_t *entries = NULL;
    int32_t count = 0;
    if (b2_dir_list(path, &entries, &count) != 0) {
        return -1;
    }

    for (int32_t i = 0; i < count; i++) {
        if (!strcmp(entries[i].name, ".") || !strcmp(entries[i].name, "..")) {
            continue;
        }
        if (b2_remove_tree(entries[i].path) != 0) {
            b2_dir_entries_free(entries, count);
            return -1;
        }
    }
    b2_dir_entries_free(entries, count);

#if defined(_WIN32)
    return _rmdir(path);
#else
    return rmdir(path);
#endif
}

int b2_copy_file(const char *src, const char *dst) {
    size_t len = 0;
    char *content = b2_read_file(src, &len);
    if (!content) {
        return -1;
    }

    char *dir = b2_dirname(dst);
    if (!dir || b2_mkdirs(dir) != 0) {
        ecs_os_free(content);
        ecs_os_free(dir);
        return -1;
    }
    ecs_os_free(dir);

    FILE *f = fopen(dst, "wb");
    if (!f) {
        ecs_os_free(content);
        return -1;
    }

    size_t written = fwrite(content, 1, len, f);
    fclose(f);
    ecs_os_free(content);
    return written == len ? 0 : -1;
}

int b2_run_command(const char *cmd) {
    if (!cmd) {
        return -1;
    }
    B2_LOG("$ %s", cmd);
    int rc = system(cmd);
    if (rc != 0) {
        B2_ERR("command failed (%d): %s", rc, cmd);
        return -1;
    }
    return 0;
}
