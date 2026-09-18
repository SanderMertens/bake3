#include "common/harness_util.h"
#include "bake/os.h"

bool bake_harness_char_is_ident(char ch) {
    return (ch >= 'a' && ch <= 'z') ||
        (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') ||
        (ch == '_');
}

bool bake_harness_symbol_chars_valid(const char *name) {
    if (!name || !name[0]) {
        return false;
    }
    for (const char *p = name; *p; p++) {
        if (!bake_harness_char_is_ident(*p)) {
            return false;
        }
    }
    return true;
}

bool bake_harness_symbol_valid(const char *name) {
    if (!name || (name[0] >= '0' && name[0] <= '9')) {
        return false;
    }
    return bake_harness_symbol_chars_valid(name);
}

static const char* bake_harness_skip_ws(const char *ptr) {
    while (*ptr == ' ' || *ptr == '\t' || *ptr == '\n' || *ptr == '\r' || *ptr == '\f' || *ptr == '\v') {
        ptr++;
    }
    return ptr;
}

static char* bake_harness_strip_strings_and_comments(const char *text) {
    if (!text) {
        return NULL;
    }
    size_t len = strlen(text);
    char *out = ecs_os_malloc(len + 1);
    size_t o = 0;
    const char *p = text;
    while (*p) {
        if (p[0] == '/' && p[1] == '/') {
            p += 2;
            while (*p && *p != '\n') {
                p++;
            }
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) {
                if (*p == '\n') {
                    out[o++] = '\n';
                }
                p++;
            }
            if (*p) {
                p += 2;
            }
            continue;
        }
        if (*p == '"' || *p == '\'') {
            char quote = *p;
            out[o++] = ' ';
            p++;
            while (*p && *p != quote) {
                if (*p == '\\' && p[1]) {
                    p += 2;
                    continue;
                }
                if (*p == '\n') {
                    out[o++] = '\n';
                }
                p++;
            }
            if (*p) {
                p++;
            }
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = '\0';
    return out;
}

int bake_harness_text_has_function(const char *text, const char *function_name) {
    if (!text || !function_name || !function_name[0]) {
        return 0;
    }

    char *clean = bake_harness_strip_strings_and_comments(text);

    size_t name_len = strlen(function_name);
    const char *cursor = clean;
    int found = 0;
    while (true) {
        const char *hit = strstr(cursor, function_name);
        if (!hit) {
            break;
        }

        if (hit != clean && bake_harness_char_is_ident(hit[-1])) {
            cursor = hit + 1;
            continue;
        }

        const char *name_end = hit + name_len;
        if (*name_end && bake_harness_char_is_ident(*name_end)) {
            cursor = hit + 1;
            continue;
        }

        const char *ptr = bake_harness_skip_ws(name_end);
        if (*ptr != '(') {
            cursor = hit + 1;
            continue;
        }

        int depth = 1;
        ptr++;
        while (*ptr && depth) {
            if (*ptr == '(') {
                depth++;
            } else if (*ptr == ')') {
                depth--;
            }
            ptr++;
        }

        if (depth != 0) {
            break;
        }

        ptr = bake_harness_skip_ws(ptr);
        if (*ptr == '{') {
            found = 1;
            break;
        }

        cursor = hit + 1;
    }

    ecs_os_free(clean);
    return found;
}

int bake_harness_suite_has_function(
    const char *text,
    const char *suite,
    const char *suffix)
{
    char *name = flecs_asprintf("%s_%s", suite, suffix);
    int found = bake_harness_text_has_function(text, name);
    ecs_os_free(name);
    return found;
}

char* bake_harness_source_path(const bake_project_cfg_t *cfg, const char *base) {
    const char *ext = bake_language_is_cpp(cfg) ? "cpp" : "c";
    char *file_name = flecs_asprintf("%s.%s", base, ext);
    char *source = bake_path_join3(cfg->path, "src", file_name);
    ecs_os_free(file_name);
    return source;
}

static char* bake_harness_try_project_header(const char *include_dir, const char *project_id) {
    if (!include_dir || !project_id || !project_id[0]) {
        return NULL;
    }

    char *header_id = bake_project_id_as_macro(project_id);
    char *header_name = flecs_asprintf("%s.h", header_id);
    char *header_path = bake_path_join(include_dir, header_name);

    if (bake_path_exists(header_path)) {
        ecs_os_free(header_id);
        ecs_os_free(header_path);
        return header_name;
    }

    ecs_os_free(header_id);
    ecs_os_free(header_name);
    ecs_os_free(header_path);
    return NULL;
}

char* bake_harness_project_header(const bake_project_cfg_t *cfg) {
    if (!cfg || !cfg->path || !cfg->id) {
        return NULL;
    }

    char *include_dir = bake_path_join(cfg->path, "include");
    char *header_name = NULL;
    char *base_id = NULL;

    if (!bake_path_exists(include_dir)) {
        ecs_os_free(include_dir);
        return NULL;
    }

    header_name = bake_harness_try_project_header(include_dir, cfg->id);
    if (!header_name) {
        base_id = bake_project_id_base(cfg->id);
        if (strcmp(base_id, cfg->id)) {
            header_name = bake_harness_try_project_header(include_dir, base_id);
        }
    }

    ecs_os_free(base_id);
    ecs_os_free(include_dir);
    return header_name;
}

void bake_harness_append_separator(
    ecs_strbuf_t *out,
    const char *existing,
    bool *appended)
{
    if (!*appended && existing && existing[0]) {
        size_t len = strlen(existing);
        if (existing[len - 1] != '\n') {
            ecs_strbuf_appendstr(out, "\n");
        }
        ecs_strbuf_appendstr(out, "\n");
    }

    *appended = true;
}
