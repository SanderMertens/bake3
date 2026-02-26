#include "bake/os.h"

void bake_dir_entries_free(bake_dir_entry_t *entries, int32_t count) {
    if (!entries) {
        return;
    }

    for (int32_t i = 0; i < count; i++) {
        ecs_os_free(entries[i].name);
        ecs_os_free(entries[i].path);
    }
    ecs_os_free(entries);
}

typedef struct bake_walk_ctx_t {
    bake_dir_walk_cb cb;
    void *ctx;
} bake_walk_ctx_t;

static int bake_dir_walk_recurse(const char *root, bake_walk_ctx_t *walk) {
    bake_dir_entry_t *entries = NULL;
    int32_t count = 0;
    if (bake_dir_list(root, &entries, &count) != 0) {
        return -1;
    }

    for (int32_t i = 0; i < count; i++) {
        const bake_dir_entry_t *entry = &entries[i];

        if (!strcmp(entry->name, ".") || !strcmp(entry->name, "..")) {
            continue;
        }

        int cb_rc = walk->cb(entry, walk->ctx);
        if (cb_rc < 0) {
            bake_dir_entries_free(entries, count);
            return -1;
        }

        if (entry->is_dir && cb_rc == 0) {
            if (bake_dir_walk_recurse(entry->path, walk) != 0) {
                bake_dir_entries_free(entries, count);
                return -1;
            }
        }
    }

    bake_dir_entries_free(entries, count);
    return 0;
}

int bake_dir_walk_recursive(const char *root, bake_dir_walk_cb cb, void *ctx) {
    bake_walk_ctx_t walk = {
        .cb = cb,
        .ctx = ctx
    };
    return bake_dir_walk_recurse(root, &walk);
}

int bake_is_dir(const char *path) {
    return bake_path_is_dir(path);
}

int bake_mkdirs(const char *path) {
    if (!path || !path[0]) {
        return -1;
    }

    if (bake_path_exists(path)) {
        return 0;
    }

    char *tmp = ecs_os_strdup(path);
    if (!tmp) {
        return -1;
    }

    size_t len = strlen(tmp);
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char prev = tmp[i];
            tmp[i] = '\0';
            if (tmp[0] && !bake_path_exists(tmp) && bake_os_mkdir(tmp) != 0 && errno != EEXIST) {
                ecs_os_free(tmp);
                return -1;
            }
            tmp[i] = prev;
        }
    }

    if (!bake_path_exists(tmp) && bake_os_mkdir(tmp) != 0 && errno != EEXIST) {
        ecs_os_free(tmp);
        return -1;
    }

    ecs_os_free(tmp);
    return 0;
}

int bake_remove_tree(const char *path) {
    if (!bake_path_exists(path)) {
        return 0;
    }

    if (!bake_is_dir(path)) {
        return remove(path);
    }

    bake_dir_entry_t *entries = NULL;
    int32_t count = 0;
    if (bake_dir_list(path, &entries, &count) != 0) {
        return -1;
    }

    for (int32_t i = 0; i < count; i++) {
        if (!strcmp(entries[i].name, ".") || !strcmp(entries[i].name, "..")) {
            continue;
        }
        if (bake_remove_tree(entries[i].path) != 0) {
            bake_dir_entries_free(entries, count);
            return -1;
        }
    }
    bake_dir_entries_free(entries, count);

    return bake_os_rmdir(path);
}

char* bake_dirname(const char *path) {
    if (!path) {
        return NULL;
    }

    const char *slash = bake_path_last_sep(path);

    if (!slash) {
        return ecs_os_strdup(".");
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

char* bake_basename(const char *path) {
    if (!path) {
        return NULL;
    }

    const char *slash = bake_path_last_sep(path);

    if (!slash) {
        return ecs_os_strdup(path);
    }

    return ecs_os_strdup(slash + 1);
}

char* bake_stem(const char *path) {
    char *base = bake_basename(path);
    if (!base) {
        return NULL;
    }

    char *dot = strrchr(base, '.');
    if (dot) {
        dot[0] = '\0';
    }

    return base;
}

char* bake_getcwd(void) {
    return bake_os_getcwd();
}
