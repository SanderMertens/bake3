#include "bake/common.h"
#include "bake/os.h"
#include <flecs.h>

#include <ctype.h>

char* bake_project_id_as_macro(const char *id) {
    if (!id) {
        return NULL;
    }

    size_t len = strlen(id);
    char *out = ecs_os_malloc(len + 1);

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)id[i];
        if (isalnum(ch)) {
            out[i] = (char)tolower(ch);
        } else {
            out[i] = '_';
        }
    }
    out[len] = '\0';
    return out;
}

bool bake_char_is_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

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
    char *w = out;

    cur = input;
    while (true) {
        const char *hit = strstr(cur, needle);
        if (!hit) {
            size_t tail_len = strlen(cur);
            memcpy(w, cur, tail_len);
            w += tail_len;
            break;
        }
        size_t prefix_len = (size_t)(hit - cur);
        memcpy(w, cur, prefix_len);
        w += prefix_len;
        memcpy(w, replacement, repl_len);
        w += repl_len;
        cur = hit + needle_len;
    }
    *w = '\0';

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

    for (char *ptr = out; *ptr; ptr++) {
        *ptr = (char)toupper((unsigned char)*ptr);
    }

    return out;
}

static char* bake_strdup_n(const char *str, size_t len) {
    char *out = ecs_os_malloc((ecs_size_t)len + 1);
    memcpy(out, str, len);
    out[len] = '\0';
    return out;
}

bool bake_expr_split(
    const char *expr,
    size_t len,
    char **kind_out,
    char **arg_out)
{
    *kind_out = NULL;
    *arg_out = NULL;

    const char *start = expr;
    const char *end = expr + len;
    while (start < end && isspace((unsigned char)*start)) {
        start++;
    }
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }

    const char *space = start;
    while (space < end && !isspace((unsigned char)*space)) {
        space++;
    }

    if (space == start) {
        return false;
    }

    *kind_out = bake_strdup_n(start, (size_t)(space - start));

    const char *arg = space;
    while (arg < end && isspace((unsigned char)*arg)) {
        arg++;
    }
    if (arg < end) {
        *arg_out = bake_strdup_n(arg, (size_t)(end - arg));
    }

    return true;
}

char* bake_expr_expand(
    const char *str,
    bake_expr_resolve_cb resolve,
    void *ctx)
{
    if (!str) {
        return NULL;
    }

    ecs_strbuf_t buf = ECS_STRBUF_INIT;
    const char *cur = str;
    while (true) {
        const char *open = strstr(cur, "${");
        if (!open) {
            ecs_strbuf_appendstr(&buf, cur);
            break;
        }

        ecs_strbuf_appendstrn(&buf, cur, (int32_t)(open - cur));

        const char *close = strchr(open + 2, '}');
        if (!close) {
            ecs_err("unterminated expression in '%s'", str);
            ecs_strbuf_reset(&buf);
            return NULL;
        }

        char *kind = NULL;
        char *arg = NULL;
        char *value = NULL;
        if (bake_expr_split(open + 2, (size_t)(close - open - 2), &kind, &arg)) {
            value = resolve(kind, arg, ctx);
        } else {
            ecs_err("empty expression in '%s'", str);
        }
        ecs_os_free(kind);
        ecs_os_free(arg);

        if (!value) {
            ecs_strbuf_reset(&buf);
            return NULL;
        }

        ecs_strbuf_appendstr(&buf, value);
        ecs_os_free(value);
        cur = close + 1;
    }

    return ecs_strbuf_get(&buf);
}
