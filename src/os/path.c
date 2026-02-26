#include "bake/os.h"

static size_t bake_path_trim_len(const char *path) {
    size_t len = strlen(path);
    while (len > 0 && bake_path_is_sep(path[len - 1])) {
        len--;
    }
    return len;
}

bool bake_path_is_sep(char ch) {
    return ch == '/' || ch == '\\';
}

char* bake_path_join(const char *lhs, const char *rhs) {
    if (!lhs || !lhs[0]) {
        return ecs_os_strdup(rhs);
    }
    if (!rhs || !rhs[0]) {
        return ecs_os_strdup(lhs);
    }

    size_t lhs_len = strlen(lhs);
    size_t rhs_len = strlen(rhs);
    char path_sep = bake_path_sep();
    bool has_sep = lhs[lhs_len - 1] == path_sep;

    char *out = ecs_os_malloc(lhs_len + rhs_len + (has_sep ? 1 : 2));
    if (!out) {
        return NULL;
    }

    memcpy(out, lhs, lhs_len);
    out[lhs_len] = '\0';
    if (!has_sep) {
        out[lhs_len] = path_sep;
        out[lhs_len + 1] = '\0';
    }
    strcat(out, rhs);
    return out;
}

char* bake_path_join3(const char *a, const char *b, const char *c) {
    char *ab = bake_path_join(a, b);
    if (!ab) {
        return NULL;
    }
    char *abc = bake_path_join(ab, c);
    ecs_os_free(ab);
    return abc;
}

bool bake_path_equal_normalized(const char *lhs, const char *rhs) {
    if (!lhs || !rhs) {
        return false;
    }

    size_t lhs_len = bake_path_trim_len(lhs);
    size_t rhs_len = bake_path_trim_len(rhs);
    if (lhs_len != rhs_len) {
        return false;
    }

    return strncmp(lhs, rhs, lhs_len) == 0;
}

bool bake_path_has_prefix_normalized(const char *path, const char *prefix, size_t *prefix_len_out) {
    if (!path || !prefix) {
        return false;
    }

    size_t prefix_len = bake_path_trim_len(prefix);
    if (!prefix_len) {
        return false;
    }

    if (strncmp(path, prefix, prefix_len)) {
        return false;
    }

    if (path[prefix_len] && !bake_path_is_sep(path[prefix_len])) {
        return false;
    }

    if (prefix_len_out) {
        *prefix_len_out = prefix_len;
    }
    return true;
}
