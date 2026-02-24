#include "bake2/os.h"

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
