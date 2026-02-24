#include "bake2/strlist.h"

static int b2_strlist_ensure(b2_strlist_t *list, int32_t n) {
    if (list->count + n <= list->capacity) {
        return 0;
    }

    int32_t next = list->capacity ? list->capacity * 2 : 8;
    while (next < list->count + n) {
        next *= 2;
    }

    char **items = ecs_os_realloc_n(list->items, char*, next);
    if (!items) {
        return -1;
    }

    list->items = items;
    list->capacity = next;
    return 0;
}

void b2_strlist_init(b2_strlist_t *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void b2_strlist_fini(b2_strlist_t *list) {
    for (int32_t i = 0; i < list->count; i++) {
        ecs_os_free(list->items[i]);
    }
    ecs_os_free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

int b2_strlist_append(b2_strlist_t *list, const char *value) {
    char *dup = b2_strdup(value);
    if (!dup) {
        return -1;
    }
    return b2_strlist_append_owned(list, dup);
}

int b2_strlist_append_owned(b2_strlist_t *list, char *value) {
    if (b2_strlist_ensure(list, 1) != 0) {
        ecs_os_free(value);
        return -1;
    }
    list->items[list->count++] = value;
    return 0;
}

int b2_strlist_contains(const b2_strlist_t *list, const char *value) {
    for (int32_t i = 0; i < list->count; i++) {
        if (!strcmp(list->items[i], value)) {
            return 1;
        }
    }
    return 0;
}

char* b2_strlist_join(const b2_strlist_t *list, const char *separator) {
    if (list->count == 0) {
        return b2_strdup("");
    }

    size_t sep_len = strlen(separator);
    size_t total = 1;
    for (int32_t i = 0; i < list->count; i++) {
        total += strlen(list->items[i]);
        if (i) {
            total += sep_len;
        }
    }

    char *out = ecs_os_malloc(total);
    if (!out) {
        return NULL;
    }

    out[0] = '\0';
    for (int32_t i = 0; i < list->count; i++) {
        if (i) {
            strcat(out, separator);
        }
        strcat(out, list->items[i]);
    }

    return out;
}
