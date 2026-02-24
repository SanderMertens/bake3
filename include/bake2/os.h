#ifndef BAKE2_OS_H
#define BAKE2_OS_H

#include "bake2/common.h"

typedef struct b2_dir_entry_t {
    char *name;
    char *path;
    bool is_dir;
} b2_dir_entry_t;

typedef int (*b2_dir_walk_cb)(const b2_dir_entry_t *entry, void *ctx);

int b2_dir_list(const char *path, b2_dir_entry_t **entries_out, int32_t *count_out);
void b2_dir_entries_free(b2_dir_entry_t *entries, int32_t count);
int b2_dir_walk_recursive(const char *root, b2_dir_walk_cb cb, void *ctx);

#endif
