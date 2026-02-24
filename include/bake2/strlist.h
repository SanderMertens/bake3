#ifndef BAKE2_STRLIST_H
#define BAKE2_STRLIST_H

#include "bake2/common.h"

typedef struct b2_strlist_t {
    char **items;
    int32_t count;
    int32_t capacity;
} b2_strlist_t;

void b2_strlist_init(b2_strlist_t *list);
void b2_strlist_fini(b2_strlist_t *list);
int b2_strlist_append(b2_strlist_t *list, const char *value);
int b2_strlist_append_owned(b2_strlist_t *list, char *value);
int b2_strlist_contains(const b2_strlist_t *list, const char *value);
char* b2_strlist_join(const b2_strlist_t *list, const char *separator);

#endif
