#include "bake/common.h"
#include "bake/os.h"
#include <flecs.h>

#include <ctype.h>

char* bake_text_replace(const char *input, const char *needle, const char *replacement) {
    size_t needle_len = strlen(needle);
    size_t repl_len = strlen(replacement);
    size_t total = 1;

    const char *cur = input;
    while (true) {
        const char *hit = strstr(cur, needle);
        if (!hit) {
            total += strlen(cur);
            break;
        }
        total += (size_t)(hit - cur) + repl_len;
        cur = hit + needle_len;
    }

    char *out = ecs_os_malloc(total);
    if (!out) {
        return NULL;
    }
    out[0] = '\0';

    cur = input;
    while (true) {
        const char *hit = strstr(cur, needle);
        if (!hit) {
            strcat(out, cur);
            break;
        }
        strncat(out, cur, (size_t)(hit - cur));
        strcat(out, replacement);
        cur = hit + needle_len;
    }

    return out;
}

bool bake_has_suffix(const char *value, const char *suffix) {
    size_t value_len = strlen(value);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > value_len) {
        return 0;
    }
    return strcmp(value + value_len - suffix_len, suffix) == 0;
}

char* bake_project_id_base(const char *id) {
    if (!id) {
        return NULL;
    }

    const char *dot = strrchr(id, '.');
    if (!dot || !dot[1]) {
        return ecs_os_strdup(id);
    }

    return ecs_os_strdup(dot + 1);
}

char* bake_project_id_as_dash(const char *id) {
    if (!id) {
        return NULL;
    }

    size_t len = strlen(id);
    char *out = ecs_os_malloc(len + 1);
    if (!out) {
        return NULL;
    }

    for (size_t i = 0; i < len; i++) {
        out[i] = id[i] == '.' ? '-' : id[i];
    }
    out[len] = '\0';
    return out;
}

char* bake_macro_upper(const char *value) {
    if (!value) {
        return NULL;
    }

    char *out = ecs_os_strdup(value);
    if (!out) {
        return NULL;
    }

    for (char *ptr = out; *ptr; ptr++) {
        *ptr = (char)toupper((unsigned char)*ptr);
    }

    return out;
}
