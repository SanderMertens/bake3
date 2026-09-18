#include "bake/environment.h"
#include "bake/os.h"
#include "env_internal.h"

static int bake_env_remove_stale_tree_entries(const char *src, const char *dst) {
    bake_dir_entry_t *entries = NULL;
    int32_t count = 0;
    if (bake_dir_list(dst, &entries, &count) != 0) {
        return -1;
    }

    int rc = 0;
    for (int32_t i = 0; i < count; i++) {
        bake_dir_entry_t *entry = &entries[i];
        if (bake_is_dot_dir(entry->name)) {
            continue;
        }

        char *src_path = bake_path_join(src, entry->name);
        bool keep = bake_path_exists(src_path) &&
            ((bake_path_is_dir(src_path) != 0) == entry->is_dir);
        ecs_os_free(src_path);

        if (keep) {
            continue;
        }

        if (bake_os_rmtree(entry->path) != 0) {
            rc = -1;
            break;
        }
    }

    bake_dir_entries_free(entries, count);
    return rc;
}

int bake_env_sync_tree_mirror(const char *src, const char *dst) {
    if (!dst || !dst[0]) {
        return -1;
    }

    if (bake_env_paths_overlap(src, dst)) {
        ecs_err(
            "refusing to sync tree: src and dst overlap "
            "(src='%s', dst='%s'); aborting to avoid deleting source files",
            src ? src : "(null)", dst);
        return -1;
    }

    if (!src || !bake_path_exists(src) || !bake_path_is_dir(src)) {
        return bake_os_rmtree(dst);
    }

    if (bake_os_mkdirs(dst) != 0) {
        return -1;
    }

    if (bake_env_remove_stale_tree_entries(src, dst) != 0) {
        return -1;
    }

    bake_dir_entry_t *entries = NULL;
    int32_t count = 0;
    if (bake_dir_list(src, &entries, &count) != 0) {
        return -1;
    }

    int rc = 0;
    for (int32_t i = 0; i < count; i++) {
        bake_dir_entry_t *entry = &entries[i];
        if (bake_is_dot_dir(entry->name)) {
            continue;
        }

        char *dst_path = bake_path_join(dst, entry->name);
        if (entry->is_dir) {
            rc = bake_env_sync_tree_mirror(entry->path, dst_path);
        } else {
            rc = bake_os_file_copy(entry->path, dst_path);
        }
        ecs_os_free(dst_path);

        if (rc != 0) {
            break;
        }
    }

    bake_dir_entries_free(entries, count);
    return rc;
}

char* bake_env_etc_dir(const bake_context_t *ctx, const char *id) {
    if (!ctx || !ctx->bake_home || !id || !id[0]) {
        return NULL;
    }

    return bake_path_join3(ctx->bake_home, "etc", id);
}

static char* bake_env_project_etc_dir(const bake_project_cfg_t *cfg) {
    if (!cfg || !cfg->path || !cfg->path[0]) {
        return NULL;
    }

    char *path = bake_path_join(cfg->path, "etc");
    if (bake_path_exists(path) && bake_path_is_dir(path)) {
        return path;
    }

    ecs_os_free(path);
    return NULL;
}

char* bake_env_etc_install_path(
    const bake_context_t *ctx,
    const bake_project_cfg_t *cfg)
{
    if (!cfg || !cfg->public_project) {
        return NULL;
    }

    char *src = bake_env_project_etc_dir(cfg);
    if (!src) {
        return NULL;
    }
    ecs_os_free(src);

    return bake_env_etc_dir(ctx, cfg->id);
}

int bake_env_sync_etc(const bake_context_t *ctx, const bake_project_cfg_t *cfg) {
    if (!cfg || !cfg->public_project || !cfg->id || !cfg->id[0]) {
        return 0;
    }

    char *dst = bake_env_etc_dir(ctx, cfg->id);
    if (!dst) {
        return 0;
    }

    char *src = bake_env_project_etc_dir(cfg);
    int rc = 0;
    if (src) {
        rc = bake_env_sync_tree_mirror(src, dst);
    } else if (bake_path_exists(dst)) {
        rc = bake_os_rmtree(dst);
    }

    ecs_os_free(src);
    ecs_os_free(dst);
    return rc;
}
