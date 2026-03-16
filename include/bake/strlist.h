#ifndef BAKE3_STRLIST_H
#define BAKE3_STRLIST_H

#include "bake/common.h"

typedef struct bake_strlist_t {
    char **items;
    int32_t count;
    int32_t capacity;
} bake_strlist_t;

void bake_strlist_init(bake_strlist_t *list);
void bake_strlist_fini(bake_strlist_t *list);
int bake_strlist_append(bake_strlist_t *list, const char *value);
int bake_strlist_append_owned(bake_strlist_t *list, char *value);
int bake_strlist_contains(const bake_strlist_t *list, const char *value);
char* bake_strlist_join(const bake_strlist_t *list, const char *separator);
int bake_strlist_merge_unique(bake_strlist_t *dst, const bake_strlist_t *src);
int bake_strlist_copy(bake_strlist_t *dst, const bake_strlist_t *src);
int bake_strlist_append_unique(bake_strlist_t *list, const char *value);

#endif
