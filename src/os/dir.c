#include "bake/os.h"
#include <flecs.h>

bool bake_is_dot_dir(const char *name) {
    return !strcmp(name, ".") || !strcmp(name, "..");
}

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

#define BAKE_DIR_WALK_MAX_DEPTH 32

typedef struct bake_walk_ctx_t {
    bake_dir_walk_cb cb;
    void *ctx;
} bake_walk_ctx_t;

static int bake_dir_walk_recurse(const char *root, bake_walk_ctx_t *walk, int32_t depth) {
    if (depth > BAKE_DIR_WALK_MAX_DEPTH) {
        ecs_err("directory walk exceeded max depth (%d) at '%s'",
            BAKE_DIR_WALK_MAX_DEPTH, root);
        return -1;
    }

    bake_dir_entry_t *entries = NULL;
    int32_t count = 0;
    if (bake_dir_list(root, &entries, &count) != 0) {
        return -1;
    }

    for (int32_t i = 0; i < count; i++) {
        const bake_dir_entry_t *entry = &entries[i];

        if (bake_is_dot_dir(entry->name)) {
            continue;
        }

        int cb_rc = walk->cb(entry, walk->ctx);
        if (cb_rc < 0) {
            bake_dir_entries_free(entries, count);
            return -1;
        }

        if (entry->is_dir && cb_rc == 0) {
            if (bake_dir_walk_recurse(entry->path, walk, depth + 1) != 0) {
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
    return bake_dir_walk_recurse(root, &walk, 0);
}

int bake_os_mkdirs(const char *path) {
    if (!path || !path[0]) {
        ecs_err("failed to create directory: invalid path");
        return -1;
    }

    if (bake_path_exists(path)) {
        if (!bake_path_is_dir(path)) {
            ecs_err(
                "failed to create directory '%s': path exists and is not a directory",
                path);
            return -1;
        }
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
            if (tmp[0]) {
                if (bake_path_exists(tmp)) {
                    if (!bake_path_is_dir(tmp)) {
                        ecs_err(
                            "failed to create directory '%s': path component '%s' is not a directory",
                            path,
                            tmp);
                        ecs_os_free(tmp);
                        return -1;
                    }
                } else if (bake_os_mkdir(tmp) != 0 && errno != EEXIST) {
                    bake_log_errno_last("create directory", tmp);
                    ecs_os_free(tmp);
                    return -1;
                }
            }
            tmp[i] = prev;
        }
    }

    if (bake_path_exists(tmp)) {
        if (!bake_path_is_dir(tmp)) {
            ecs_err(
                "failed to create directory '%s': path exists and is not a directory",
                path);
            ecs_os_free(tmp);
            return -1;
        }
    } else if (bake_os_mkdir(tmp) != 0 && errno != EEXIST) {
        bake_log_errno_last("create directory", tmp);
        ecs_os_free(tmp);
        return -1;
    }

    ecs_os_free(tmp);
    return 0;
}

int bake_os_rmtree(const char *path) {
    if (!bake_path_exists(path)) {
        return 0;
    }

    if (!bake_path_is_dir(path)) {
        return bake_remove_file(path);
    }

    bake_dir_entry_t *entries = NULL;
    int32_t count = 0;
    if (bake_dir_list(path, &entries, &count) != 0) {
        return -1;
    }

    for (int32_t i = 0; i < count; i++) {
        if (bake_is_dot_dir(entries[i].name)) {
            continue;
        }
        if (bake_os_rmtree(entries[i].path) != 0) {
            bake_dir_entries_free(entries, count);
            return -1;
        }
    }
    bake_dir_entries_free(entries, count);

    if (bake_os_rmdir(path) != 0) {
        bake_log_errno_last("remove directory", path);
        return -1;
    }

    return 0;
}

char* bake_path_dirname(const char *path) {
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

char* bake_path_basename(const char *path) {
    if (!path) {
        return NULL;
    }

    const char *slash = bake_path_last_sep(path);

    if (!slash) {
        return ecs_os_strdup(path);
    }

    return ecs_os_strdup(slash + 1);
}

char* bake_path_stem(const char *path) {
    char *base = bake_path_basename(path);
    if (!base) {
        return NULL;
    }

    char *dot = strrchr(base, '.');
    if (dot) {
        dot[0] = '\0';
    }

    return base;
}
