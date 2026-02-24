#include "bake2/os.h"

void b2_dir_entries_free(b2_dir_entry_t *entries, int32_t count) {
    if (!entries) {
        return;
    }

    for (int32_t i = 0; i < count; i++) {
        ecs_os_free(entries[i].name);
        ecs_os_free(entries[i].path);
    }
    ecs_os_free(entries);
}

typedef struct b2_walk_ctx_t {
    b2_dir_walk_cb cb;
    void *ctx;
} b2_walk_ctx_t;

static int b2_dir_walk_recurse(const char *root, b2_walk_ctx_t *walk) {
    b2_dir_entry_t *entries = NULL;
    int32_t count = 0;
    if (b2_dir_list(root, &entries, &count) != 0) {
        return -1;
    }

    for (int32_t i = 0; i < count; i++) {
        const b2_dir_entry_t *entry = &entries[i];

        if (!strcmp(entry->name, ".") || !strcmp(entry->name, "..")) {
            continue;
        }

        int cb_rc = walk->cb(entry, walk->ctx);
        if (cb_rc < 0) {
            b2_dir_entries_free(entries, count);
            return -1;
        }

        if (entry->is_dir && cb_rc == 0) {
            if (b2_dir_walk_recurse(entry->path, walk) != 0) {
                b2_dir_entries_free(entries, count);
                return -1;
            }
        }
    }

    b2_dir_entries_free(entries, count);
    return 0;
}

int b2_dir_walk_recursive(const char *root, b2_dir_walk_cb cb, void *ctx) {
    b2_walk_ctx_t walk = {
        .cb = cb,
        .ctx = ctx
    };
    return b2_dir_walk_recurse(root, &walk);
}
