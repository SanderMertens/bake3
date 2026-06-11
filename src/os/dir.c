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

static int bake_os_mkdir_component(const char *full_path, const char *component) {
#if defined(_WIN32)
    /* Drive roots like "D:" cannot be created and do not stat reliably. */
    if (component[0] && component[1] == ':' && !component[2]) {
        return 0;
    }
#endif

    if (bake_os_mkdir(component) == 0) {
        return 0;
    }

    /* mkdir of an existing component fails with EEXIST on POSIX, but Windows
     * reports EACCES for drive roots like "D:"; accept any existing
     * directory regardless of the error. */
    if (bake_path_is_dir(component)) {
        return 0;
    }

    if (errno == EEXIST) {
        ecs_err(
            "failed to create directory '%s': path component '%s' is not a directory",
            full_path,
            component);
        return -1;
    }

    bake_log_errno_last("create directory", component);
    return -1;
}

int bake_os_mkdirs(const char *path) {
    if (!path || !path[0]) {
        ecs_err("failed to create directory: invalid path");
        return -1;
    }

    if (bake_path_is_dir(path)) {
        return 0;
    }

    char *tmp = ecs_os_strdup(path);
    size_t len = strlen(tmp);
    int rc = 0;
    for (size_t i = 1; rc == 0 && i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char prev = tmp[i];
            tmp[i] = '\0';
            if (tmp[0]) {
                rc = bake_os_mkdir_component(path, tmp);
            }
            tmp[i] = prev;
        }
    }

    if (rc == 0) {
        rc = bake_os_mkdir_component(path, tmp);
    }

    ecs_os_free(tmp);
    return rc;
}

int bake_os_rmtree(const char *path) {
    /* Symlinks (including symlinks to directories) must be unlinked rather
     * than traversed. Treating a directory symlink as a directory and
     * recursing into it would delete the contents of the symlink target,
     * which is almost never what bake wants and has caused source trees
     * (e.g. legacy bake2 symlinks under ~/bake/include/<id>) to be wiped.
     * This check must come before the exists check: stat follows links, so
     * a dangling symlink would otherwise be reported as nonexistent and
     * left in place. */
    if (bake_path_is_symlink(path)) {
#if defined(_WIN32)
        /* Directory junctions and symlinks cannot be deleted with remove();
         * RemoveDirectory deletes the link without traversing into it. */
        if (bake_path_is_dir(path)) {
            if (bake_os_rmdir(path) != 0) {
                bake_log_errno_last("remove directory link", path);
                return -1;
            }
            return 0;
        }
#endif
        return bake_remove_file(path);
    }

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
    if (!len) {
        /* Parent of a root-level path is the root itself. */
        char root[2] = { slash[0], '\0' };
        return ecs_os_strdup(root);
    }

    char *out = ecs_os_malloc(len + 1);
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
