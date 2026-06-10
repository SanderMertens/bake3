#include "build_internal.h"
#include "bake/os.h"

#include <ctype.h>

#define BAKE_AMALG_MAX_LINE (4096)

typedef struct bake_concat_ctx_t {
    ecs_strbuf_t *out;
    const char *ext;
} bake_concat_ctx_t;

typedef struct bake_amalgamate_ctx_t {
    const bake_project_cfg_t *cfg;
    const char *include_name;
    const char *include_path;
    const bake_strlist_t *disable;
} bake_amalgamate_ctx_t;

typedef struct bake_collect_sources_ctx_t {
    bake_strlist_t *sources;
} bake_collect_sources_ctx_t;

static int bake_concat_visit(const bake_dir_entry_t *entry, void *ctx_ptr) {
    bake_concat_ctx_t *ctx = ctx_ptr;
    if (entry->is_dir) {
        if (bake_is_dot_dir(entry->name) || entry->name[0] == '.') {
            return 1;
        }
        return 0;
    }

    if (!bake_has_suffix(entry->path, ctx->ext)) {
        return 0;
    }

    char *content = bake_file_read(entry->path, NULL);
    if (!content) {
        return -1;
    }

    char *rel = bake_path_basename(entry->path);
    ecs_strbuf_append(ctx->out, "\n/* --- %s --- */\n", rel ? rel : entry->path);
    ecs_strbuf_appendstr(ctx->out, content);
    ecs_strbuf_appendstr(ctx->out, "\n");

    ecs_os_free(rel);
    ecs_os_free(content);
    return 0;
}

int bake_amalgamate_project(const bake_project_cfg_t *cfg, const char *dst_dir) {
    int rc = -1;
    char *base = NULL;
    char *h_name = NULL;
    char *c_name = NULL;
    char *h_path = NULL;
    char *c_path = NULL;
    char *h_content = NULL;
    char *c_content = NULL;
    ecs_strbuf_t h_buf = ECS_STRBUF_INIT;
    ecs_strbuf_t c_buf = ECS_STRBUF_INIT;

    if (bake_os_mkdirs(dst_dir) != 0) {
        goto cleanup;
    }

    base = bake_project_id_as_macro(cfg->id);
    h_name = base ? flecs_asprintf("%s.h", base) : NULL;
    c_name = base ? flecs_asprintf("%s.c", base) : NULL;
    h_path = h_name ? bake_path_join(dst_dir, h_name) : NULL;
    c_path = c_name ? bake_path_join(dst_dir, c_name) : NULL;
    if (!base || !h_name || !c_name || !h_path || !c_path) {
        goto cleanup;
    }

    ecs_strbuf_append(&h_buf, "/* Amalgamated headers for %s */\n", cfg->id);
    ecs_strbuf_append(&h_buf, "#ifndef %s_H\n", base);
    ecs_strbuf_append(&h_buf, "#define %s_H\n", base);

    char *include_dir = bake_path_join(cfg->path, "include");
    if (include_dir && bake_path_exists(include_dir)) {
        bake_concat_ctx_t ctx = {.out = &h_buf, .ext = ".h"};
        if (bake_dir_walk_recursive(include_dir, bake_concat_visit, &ctx) != 0) {
            ecs_os_free(include_dir);
            goto cleanup;
        }
    }
    ecs_os_free(include_dir);

    ecs_strbuf_appendstr(&h_buf, "\n#endif\n");
    ecs_strbuf_append(&c_buf, "/* Amalgamated sources for %s */\n", cfg->id);
    ecs_strbuf_append(&c_buf, "#include \"%s\"\n", h_name);

    char *src_dir = bake_path_join(cfg->path, "src");
    if (src_dir && bake_path_exists(src_dir)) {
        bake_concat_ctx_t ctx = {.out = &c_buf, .ext = ".c"};
        if (bake_dir_walk_recursive(src_dir, bake_concat_visit, &ctx) != 0) {
            ecs_os_free(src_dir);
            goto cleanup;
        }
    }
    ecs_os_free(src_dir);

    h_content = ecs_strbuf_get(&h_buf);
    c_content = ecs_strbuf_get(&c_buf);
    if (!h_content || !c_content) {
        goto cleanup;
    }

    if (bake_file_write(h_path, h_content) != 0 || bake_file_write(c_path, c_content) != 0) {
        goto cleanup;
    }

    rc = 0;

cleanup:
#define F(p) ecs_os_free(p)
    F(base); F(h_name); F(c_name); F(h_path); F(c_path); F(h_content); F(c_content);
#undef F
    return rc;
}

static const char* bake_skip_ws(const char *ptr) {
    while (ptr && *ptr && isspace((unsigned char)*ptr)) {
        ptr++;
    }
    return ptr;
}

static char* bake_parse_include_file(const char *line, bool *relative_out) {
    if (relative_out) {
        *relative_out = false;
    }

    const char *p = bake_skip_ws(line);
    if (!p || p[0] != '#') {
        return NULL;
    }

    p = bake_skip_ws(p + 1);
    if (strncmp(p, "include", 7)) {
        return NULL;
    }

    p = bake_skip_ws(p + 7);
    if (!p || !p[0]) {
        return NULL;
    }

    char end = '>';
    if (p[0] == '"') {
        end = '"';
        if (relative_out) {
            *relative_out = true;
        }
    } else if (p[0] != '<') {
        return NULL;
    }

    p++;
    const char *end_ptr = strchr(p, end);
    if (!end_ptr) {
        return NULL;
    }

    size_t len = (size_t)(end_ptr - p);
    char *out = ecs_os_malloc(len + 1);
    if (!out) {
        return NULL;
    }

    memcpy(out, p, len);
    out[len] = '\0';
    return out;
}

static bool bake_path_mark_parsed(bake_strlist_t *parsed, const char *path) {
    char *normalized = bake_path_resolve(path);
    if (!normalized) {
        return false;
    }

    if (bake_strlist_contains(parsed, normalized)) {
        ecs_os_free(normalized);
        return false;
    }

    if (bake_strlist_append_owned(parsed, normalized) != 0) {
        ecs_os_free(normalized);
        return false;
    }

    return true;
}

static int bake_source_depth(const char *path) {
    int depth = 0;
    for (const char *p = path; p && *p; p++) {
        if (*p == '/' || *p == '\\') {
            depth++;
        }
    }
    return depth;
}

static int bake_source_path_compare(const void *ptr1, const void *ptr2) {
    const char *path1 = *(const char* const*)ptr1;
    const char *path2 = *(const char* const*)ptr2;

    int depth1 = bake_source_depth(path1);
    int depth2 = bake_source_depth(path2);
    if (depth1 != depth2) {
        return depth1 - depth2;
    }
    return strcmp(path1, path2);
}

#define BAKE_AMALG_MAX_COND (256)

typedef enum bake_cpp_directive_kind_t {
    BAKE_CPP_NONE,
    BAKE_CPP_IF,
    BAKE_CPP_IFDEF,
    BAKE_CPP_IFNDEF,
    BAKE_CPP_ELIF,
    BAKE_CPP_ELSE,
    BAKE_CPP_ENDIF
} bake_cpp_directive_kind_t;

typedef struct bake_cond_frame_t {
    bool managed;
    bool emit;
    bool taken;
} bake_cond_frame_t;

static bool bake_is_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

static bake_cpp_directive_kind_t bake_parse_cpp_directive(
    const char *line,
    const char **arg_out)
{
    if (arg_out) {
        *arg_out = NULL;
    }

    const char *p = bake_skip_ws(line);
    if (!p || p[0] != '#') {
        return BAKE_CPP_NONE;
    }

    p = bake_skip_ws(p + 1);
    const char *word = p;
    while (bake_is_ident_char(*p)) {
        p++;
    }

    size_t len = (size_t)(p - word);
    bake_cpp_directive_kind_t kind = BAKE_CPP_NONE;
    if (len == 2 && !strncmp(word, "if", 2)) {
        kind = BAKE_CPP_IF;
    } else if (len == 5 && !strncmp(word, "ifdef", 5)) {
        kind = BAKE_CPP_IFDEF;
    } else if (len == 6 && !strncmp(word, "ifndef", 6)) {
        kind = BAKE_CPP_IFNDEF;
    } else if (len == 4 && !strncmp(word, "elif", 4)) {
        kind = BAKE_CPP_ELIF;
    } else if (len == 4 && !strncmp(word, "else", 4)) {
        kind = BAKE_CPP_ELSE;
    } else if (len == 5 && !strncmp(word, "endif", 5)) {
        kind = BAKE_CPP_ENDIF;
    }

    if (kind != BAKE_CPP_NONE && arg_out) {
        *arg_out = p;
    }
    return kind;
}

static bool bake_disable_contains(const bake_strlist_t *disable, const char *name, size_t len) {
    if (!disable || len == 0 || len >= 256) {
        return false;
    }
    char buf[256];
    memcpy(buf, name, len);
    buf[len] = '\0';
    return bake_strlist_contains(disable, buf) != 0;
}

static bool bake_arg_is_disabled_macro(const char *arg, const bake_strlist_t *disable) {
    const char *p = bake_skip_ws(arg);
    const char *name = p;
    while (bake_is_ident_char(*p)) {
        p++;
    }
    return bake_disable_contains(disable, name, (size_t)(p - name));
}

static bool bake_is_disabled_define(const char *line, const bake_strlist_t *disable) {
    const char *p = bake_skip_ws(line);
    if (p[0] != '#') {
        return false;
    }
    p = bake_skip_ws(p + 1);
    if (strncmp(p, "define", 6) || bake_is_ident_char(p[6])) {
        return false;
    }
    p = bake_skip_ws(p + 6);
    const char *name = p;
    while (bake_is_ident_char(*p)) {
        p++;
    }
    if (p[0] == '(') {
        return false;
    }
    return bake_disable_contains(disable, name, (size_t)(p - name));
}

static bool bake_eval_defined_expr(
    const char *expr,
    const bake_strlist_t *disable,
    bool *value_out)
{
    const char *p = bake_skip_ws(expr);
    bool negate = false;
    if (p[0] == '!') {
        negate = true;
        p = bake_skip_ws(p + 1);
    }

    if (strncmp(p, "defined", 7)) {
        return false;
    }
    p += 7;

    bool had_paren = false;
    p = bake_skip_ws(p);
    if (p[0] == '(') {
        had_paren = true;
        p = bake_skip_ws(p + 1);
    } else if (bake_is_ident_char(p[0]) == false) {
        return false;
    }

    const char *name = p;
    while (bake_is_ident_char(*p)) {
        p++;
    }
    size_t name_len = (size_t)(p - name);

    p = bake_skip_ws(p);
    if (had_paren) {
        if (p[0] != ')') {
            return false;
        }
        p = bake_skip_ws(p + 1);
    }

    if (p[0] != '\0' && p[0] != '\n' && p[0] != '\r') {
        return false;
    }

    if (!bake_disable_contains(disable, name, name_len)) {
        return false;
    }

    *value_out = negate ? true : false;
    return true;
}

static char* bake_condition_text(const char *arg) {
    const char *start = bake_skip_ws(arg);
    size_t len = strlen(start);
    char *text = ecs_os_malloc(len + 1);
    if (!text) {
        return NULL;
    }
    memcpy(text, start, len);
    text[len] = '\0';

    for (char *p = text; *p; p++) {
        if (p[0] == '/' && (p[1] == '/' || p[1] == '*')) {
            *p = '\0';
            break;
        }
        if (p[0] == '\n' || p[0] == '\r') {
            *p = '\0';
            break;
        }
    }

    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        *(--end) = '\0';
    }

    return text;
}

static bool bake_cond_eval_managed(
    bake_cpp_directive_kind_t kind,
    const char *arg,
    const bake_strlist_t *disable,
    bool *body_emit_out)
{
    if (kind == BAKE_CPP_IFDEF) {
        if (bake_arg_is_disabled_macro(arg, disable)) {
            *body_emit_out = false;
            return true;
        }
        return false;
    }

    if (kind == BAKE_CPP_IFNDEF) {
        if (bake_arg_is_disabled_macro(arg, disable)) {
            *body_emit_out = true;
            return true;
        }
        return false;
    }

    if (kind == BAKE_CPP_IF) {
        char *cond = bake_condition_text(arg);
        if (!cond) {
            return false;
        }
        bool value = false;
        bool managed = bake_eval_defined_expr(cond, disable, &value);
        ecs_os_free(cond);
        if (managed) {
            *body_emit_out = value;
            return true;
        }
        return false;
    }

    return false;
}

static int bake_amalgamate_file(
    const bake_amalgamate_ctx_t *ctx,
    FILE *out,
    bool is_include,
    const char *file,
    const char *src_file,
    int32_t src_line,
    bake_strlist_t *parsed,
    bool *main_included)
{
    if (!bake_path_mark_parsed(parsed, file)) {
        return 0;
    }

    FILE *in = fopen(file, "rb");
    if (!in) {
        ecs_err(
            "cannot read file '%s' while amalgamating '%s' (from '%s:%d')",
            file, ctx->cfg->id, src_file, src_line);
        return -1;
    }

    char *cur_path = bake_path_dirname(file);
    char *base_name = bake_path_basename(file);
    bool bake_config_h = base_name && !strcmp(base_name, "bake_config.h");
    ecs_os_free(base_name);

    char line[BAKE_AMALG_MAX_LINE];
    int32_t line_count = 0;
    bool in_block_comment = false;
    bool has_disable = ctx->disable && ctx->disable->count > 0;
    bake_cond_frame_t cond_stack[BAKE_AMALG_MAX_COND];
    int32_t cond_depth = 0;
    int32_t suppressed = 0;
    while (fgets(line, BAKE_AMALG_MAX_LINE, in)) {
        line_count++;

        bool line_in_block_comment_at_start = in_block_comment;
        const char *scan = line;
        while (*scan) {
            if (in_block_comment) {
                if (scan[0] == '*' && scan[1] == '/') {
                    in_block_comment = false;
                    scan += 2;
                    continue;
                }
                scan++;
                continue;
            }
            if (scan[0] == '/' && scan[1] == '*') {
                in_block_comment = true;
                scan += 2;
                continue;
            }
            scan++;
        }

        if (has_disable && !line_in_block_comment_at_start) {
            const char *arg = NULL;
            bake_cpp_directive_kind_t directive =
                bake_parse_cpp_directive(line, &arg);

            if (directive == BAKE_CPP_IF ||
                directive == BAKE_CPP_IFDEF ||
                directive == BAKE_CPP_IFNDEF)
            {
                if (cond_depth >= BAKE_AMALG_MAX_COND) {
                    ecs_err(
                        "preprocessor nesting too deep while amalgamating '%s'",
                        file);
                    ecs_os_free(cur_path);
                    fclose(in);
                    return -1;
                }

                bool body_emit = true;
                bool managed = bake_cond_eval_managed(
                    directive, arg, ctx->disable, &body_emit);

                bake_cond_frame_t *frame = &cond_stack[cond_depth++];
                frame->managed = managed;
                if (managed) {
                    frame->emit = body_emit;
                    frame->taken = body_emit;
                    if (!body_emit) {
                        suppressed++;
                    }
                } else {
                    frame->emit = true;
                    frame->taken = false;
                    if (suppressed == 0) {
                        fprintf(out, "%s", line);
                    }
                }
                continue;
            }

            if (directive == BAKE_CPP_ELSE && cond_depth > 0) {
                bake_cond_frame_t *frame = &cond_stack[cond_depth - 1];
                if (frame->managed) {
                    if (frame->taken) {
                        if (frame->emit) {
                            frame->emit = false;
                            suppressed++;
                        }
                    } else {
                        if (!frame->emit) {
                            suppressed--;
                        }
                        frame->emit = true;
                        frame->taken = true;
                    }
                } else if (suppressed == 0) {
                    fprintf(out, "%s", line);
                }
                continue;
            }

            if (directive == BAKE_CPP_ELIF && cond_depth > 0) {
                bake_cond_frame_t *frame = &cond_stack[cond_depth - 1];
                if (frame->managed) {
                    if (frame->taken) {
                        if (frame->emit) {
                            frame->emit = false;
                            suppressed++;
                        }
                    } else {
                        if (!frame->emit) {
                            suppressed--;
                        }
                        frame->emit = true;
                        frame->managed = false;
                        if (suppressed == 0) {
                            fprintf(out, "#if%s", arg);
                        }
                    }
                } else if (suppressed == 0) {
                    fprintf(out, "%s", line);
                }
                continue;
            }

            if (directive == BAKE_CPP_ENDIF && cond_depth > 0) {
                bake_cond_frame_t *frame = &cond_stack[--cond_depth];
                if (frame->managed) {
                    if (!frame->emit) {
                        suppressed--;
                    }
                } else if (suppressed == 0) {
                    fprintf(out, "%s", line);
                }
                continue;
            }
        }

        if (suppressed > 0) {
            continue;
        }

        if (has_disable && !line_in_block_comment_at_start &&
            bake_is_disabled_define(line, ctx->disable))
        {
            continue;
        }

        bool include_relative = false;
        char *include = NULL;
        if (!line_in_block_comment_at_start) {
            include = bake_parse_include_file(line, &include_relative);
        }
        if (!include) {
            fprintf(out, "%s", line);
            continue;
        }

        if (!is_include && main_included && !main_included[0]) {
            fprintf(out, "#include \"%s.h\"\n", ctx->include_name);
            main_included[0] = true;
        }

        bool recurse = false;
        char *include_path = NULL;

        if (!include_relative) {
            if (bake_config_h) {
                fprintf(out, "#include \"%s\"\n", include);
                ecs_os_free(include);
                continue;
            }

            include_path = bake_path_join(ctx->include_path, include);
            if (include_path && bake_path_exists(include_path)) {
                recurse = true;
            }
        } else {
            include_path = cur_path ? bake_path_join(cur_path, include) : NULL;
            if (!include_path || !bake_path_exists(include_path)) {
                ecs_os_free(include_path);
                include_path = bake_path_join(ctx->include_path, include);
                if (include_path && bake_path_exists(include_path)) {
                    recurse = true;
                } else {
                    ecs_os_free(include_path);
                    include_path = NULL;
                }
            } else {
                recurse = true;
            }
        }

        if (recurse) {
            if (bake_amalgamate_file(
                ctx,
                out,
                is_include,
                include_path,
                file,
                line_count,
                parsed,
                main_included) != 0)
            {
                ecs_os_free(include_path);
                ecs_os_free(include);
                ecs_os_free(cur_path);
                fclose(in);
                return -1;
            }
        } else {
            fprintf(out, "%s", line);
        }

        ecs_os_free(include_path);
        ecs_os_free(include);
    }

    fprintf(out, "\n");
    ecs_os_free(cur_path);

    if (ferror(in)) {
        ecs_err("read error while amalgamating '%s'", file);
        fclose(in);
        return -1;
    }

    if (ferror(out)) {
        ecs_err("write error while amalgamating '%s'", file);
        fclose(in);
        return -1;
    }

    fclose(in);
    return 0;
}

static int bake_try_source_name(
    const char *src_dir,
    const char *name,
    const char *ext,
    char **out)
{
    if (*out) {
        return 0;
    }

    char *path = flecs_asprintf("%s/%s.%s", src_dir, name, ext);
    if (!path) {
        return -1;
    }
    if (bake_path_exists(path)) {
        *out = path;
        return 0;
    }

    ecs_os_free(path);
    return 0;
}

static char* bake_find_main_src_file(
    const bake_project_cfg_t *cfg,
    const char *src_dir,
    const char *project_id)
{
    static const char *c_exts[] = {"c", NULL};
    static const char *cpp_exts[] = {"cpp", "cc", "cxx", "C", NULL};
    const char *const *exts = bake_language_is_cpp(cfg) ? cpp_exts : c_exts;

    char *id_base = bake_project_id_base(cfg->id);
    if (!id_base) {
        return NULL;
    }

    char *found = NULL;
    for (int i = 0; exts[i] && !found; i++) {
        if (bake_try_source_name(src_dir, "main", exts[i], &found) != 0) {
            ecs_os_free(found);
            ecs_os_free(id_base);
            return NULL;
        }
        if (!found && bake_try_source_name(src_dir, project_id, exts[i], &found) != 0) {
            ecs_os_free(found);
            ecs_os_free(id_base);
            return NULL;
        }
        if (!found && bake_try_source_name(src_dir, id_base, exts[i], &found) != 0) {
            ecs_os_free(found);
            ecs_os_free(id_base);
            return NULL;
        }
    }

    ecs_os_free(id_base);
    return found;
}

static bool bake_is_supported_source(const char *path) {
    return bake_has_suffix(path, ".c") ||
        bake_has_suffix(path, ".cpp") ||
        bake_has_suffix(path, ".cc") ||
        bake_has_suffix(path, ".cxx") ||
        bake_has_suffix(path, ".C");
}

static int bake_collect_source_files_visit(const bake_dir_entry_t *entry, void *ctx_ptr) {
    bake_collect_sources_ctx_t *ctx = ctx_ptr;
    if (entry->is_dir) {
        if (bake_is_dot_dir(entry->name) || entry->name[0] == '.') {
            return 1;
        }
        return 0;
    }

    if (!bake_is_supported_source(entry->path)) {
        return 0;
    }

    return bake_strlist_append(ctx->sources, entry->path);
}

static int bake_collect_source_files(
    const char *src_dir,
    bake_strlist_t *sources)
{
    bake_collect_sources_ctx_t ctx = {
        .sources = sources
    };

    if (!bake_path_exists(src_dir)) {
        return 0;
    }

    if (bake_dir_walk_recursive(src_dir, bake_collect_source_files_visit, &ctx) != 0) {
        return -1;
    }

    if (sources->count > 1) {
        qsort(
            sources->items,
            (size_t)sources->count,
            sizeof(char*),
            bake_source_path_compare);
    }

    return 0;
}

static bool bake_comment_has_file_directive(const char *start, const char *end) {
    for (const char *q = start; q + 5 <= end; q++) {
        if ((q[0] == '@' || q[0] == '\\') &&
            q[1] == 'f' && q[2] == 'i' && q[3] == 'l' && q[4] == 'e')
        {
            return true;
        }
    }
    return false;
}

static char* bake_clean_amalgamation(const char *in, size_t in_len) {
    char *out = ecs_os_malloc(in_len + 1);
    if (!out) {
        return NULL;
    }

    size_t w = 0;
    int newline_run = 2;
    const char *p = in;
    const char *in_end = in + in_len;

    while (*p) {
        char c = *p;

        if (c == '"' || c == '\'') {
            char quote = c;
            out[w++] = *p++;
            while (*p) {
                if (*p == '\\' && p[1]) {
                    out[w++] = *p++;
                    out[w++] = *p++;
                    continue;
                }
                char s = *p;
                out[w++] = *p++;
                if (s == quote) {
                    break;
                }
            }
            newline_run = 0;
            continue;
        }

        if (c == '/' && p[1] == '/') {
            while (*p && *p != '\n') {
                out[w++] = *p++;
            }
            newline_run = 0;
            continue;
        }

        if (c == '/' && p[1] == '*') {
            const char *end = strstr(p + 2, "*/");
            const char *comment_end = end ? end + 2 : in_end;
            if (bake_comment_has_file_directive(p, comment_end)) {
                p = comment_end;
                continue;
            }
            while (p < comment_end) {
                out[w++] = *p++;
            }
            newline_run = 0;
            continue;
        }

        if (c == '\n') {
            if (newline_run >= 2) {
                p++;
                continue;
            }
            out[w++] = '\n';
            newline_run++;
            p++;
            continue;
        }

        out[w++] = c;
        newline_run = 0;
        p++;
    }

    out[w] = '\0';
    return out;
}

static int bake_finalize_amalgamation(
    const char *tmp_file,
    const char *out_file)
{
    size_t len = 0;
    char *raw = bake_file_read(tmp_file, &len);
    remove(tmp_file);
    if (!raw) {
        return -1;
    }

    char *cleaned = bake_clean_amalgamation(raw, len);
    ecs_os_free(raw);
    if (!cleaned) {
        return -1;
    }

    int rc = bake_file_write(out_file, cleaned);
    ecs_os_free(cleaned);
    return rc;
}

static int bake_generate_one_amalgamation(
    const bake_project_cfg_t *cfg,
    const char *project_id,
    const char *include_path,
    const char *src_path,
    const char *main_header,
    const bake_amalgamate_cfg_t *amalg)
{
    int rc = -1;
    char *output_path = NULL;
    char *include_out = NULL;
    char *include_tmp = NULL;
    char *src_out = NULL;
    char *src_tmp = NULL;
    char *main_src = NULL;
    FILE *include_fp = NULL;
    FILE *src_fp = NULL;
    bool main_included = false;
    bake_strlist_t parsed = {0};
    bake_strlist_t sources = {0};
    bool parsed_ready = false;
    bool sources_ready = false;

    const char *output_base =
        (amalg->prefix && amalg->prefix[0]) ? amalg->prefix : project_id;

    output_path = (amalg->path && amalg->path[0]) ?
        bake_path_join(cfg->path, amalg->path) : ecs_os_strdup(cfg->path);
    if (!output_path || bake_os_mkdirs(output_path) != 0) {
        goto cleanup;
    }

    const char *src_ext = bake_language_is_cpp(cfg) ? "cpp" : "c";
    include_out = flecs_asprintf("%s/%s.h", output_path, output_base);
    include_tmp = flecs_asprintf("%s/%s.h.tmp", output_path, output_base);
    src_out = flecs_asprintf("%s/%s.%s", output_path, output_base, src_ext);
    src_tmp = flecs_asprintf("%s/%s.%s.tmp", output_path, output_base, src_ext);
    if (!include_out || !include_tmp || !src_out || !src_tmp) {
        goto cleanup;
    }

    bake_amalgamate_ctx_t ctx = {
        .cfg = cfg,
        .include_name = output_base,
        .include_path = include_path,
        .disable = &amalg->disable_flags
    };

    bake_strlist_init(&parsed);
    parsed_ready = true;

    include_fp = fopen(include_tmp, "wb");
    if (!include_fp) {
        goto cleanup;
    }

    fprintf(include_fp, "// Comment out this line when using as DLL\n");
    fprintf(include_fp, "#define %s_STATIC\n", project_id);
    if (bake_amalgamate_file(
        &ctx, include_fp, true, main_header, "(main header)", 0,
        &parsed, NULL) != 0)
    {
        goto cleanup;
    }
    fclose(include_fp);
    include_fp = NULL;

    src_fp = fopen(src_tmp, "wb");
    if (!src_fp) {
        goto cleanup;
    }

    main_src = bake_find_main_src_file(cfg, src_path, project_id);
    if (main_src && bake_amalgamate_file(
        &ctx, src_fp, false, main_src, "(main source)", 0,
        &parsed, &main_included) != 0)
    {
        goto cleanup;
    }

    bake_strlist_init(&sources);
    sources_ready = true;
    if (bake_collect_source_files(src_path, &sources) != 0) {
        goto cleanup;
    }

    for (int32_t i = 0; i < sources.count; i++) {
        if (main_src && !strcmp(sources.items[i], main_src)) {
            continue;
        }
        if (bake_amalgamate_file(
            &ctx, src_fp, false, sources.items[i], "(source)", 0,
            &parsed, &main_included) != 0)
        {
            goto cleanup;
        }
    }

    if (!main_included) {
        fprintf(src_fp, "#include \"%s.h\"\n", output_base);
    }
    fclose(src_fp);
    src_fp = NULL;

    if (bake_finalize_amalgamation(include_tmp, include_out) != 0 ||
        bake_finalize_amalgamation(src_tmp, src_out) != 0)
    {
        goto cleanup;
    }

    rc = 0;
cleanup:
    if (include_fp) {
        fclose(include_fp);
    }
    if (src_fp) {
        fclose(src_fp);
    }
    if (rc != 0) {
        if (include_tmp) {
            remove(include_tmp);
        }
        if (src_tmp) {
            remove(src_tmp);
        }
    }
    if (sources_ready) {
        bake_strlist_fini(&sources);
    }
    if (parsed_ready) {
        bake_strlist_fini(&parsed);
    }
    ecs_os_free(output_path);
    ecs_os_free(include_out);
    ecs_os_free(include_tmp);
    ecs_os_free(src_out);
    ecs_os_free(src_tmp);
    ecs_os_free(main_src);
    return rc;
}

int bake_generate_project_amalgamation(const bake_project_cfg_t *cfg) {
    if (!cfg) {
        return 0;
    }

    int32_t count = bake_amalgamate_list_count(&cfg->amalgamate);
    if (count == 0) {
        return 0;
    }

    int rc = -1;
    char *project_id = bake_project_id_as_macro(cfg->id);
    char *include_path = bake_path_join(cfg->path, "include");
    char *src_path = bake_path_join(cfg->path, "src");
    char *main_header = include_path && project_id ?
        flecs_asprintf("%s/%s.h", include_path, project_id) : NULL;
    if (!project_id || !include_path || !src_path || !main_header) {
        goto cleanup;
    }

    if (!bake_path_exists(main_header)) {
        char *id_base = bake_project_id_base(cfg->id);
        char *base_project_id = id_base ? bake_project_id_as_macro(id_base) : NULL;
        char *base_header = base_project_id ? flecs_asprintf("%s/%s.h", include_path, base_project_id) : NULL;
        if (base_project_id && base_header && bake_path_exists(base_header)) {
            ecs_os_free(project_id);
            project_id = base_project_id;
            base_project_id = NULL;
            ecs_os_free(main_header);
            main_header = base_header;
            base_header = NULL;
        }
        ecs_os_free(id_base);
        ecs_os_free(base_project_id);
        ecs_os_free(base_header);
    }

    if (!bake_path_exists(main_header)) {
        ecs_err("cannot find include file '%s' for amalgamation", main_header);
        goto cleanup;
    }

    rc = 0;
    for (int32_t i = 0; i < count; i++) {
        const bake_amalgamate_cfg_t *amalg = bake_amalgamate_list_get(&cfg->amalgamate, i);
        if (bake_generate_one_amalgamation(
            cfg, project_id, include_path, src_path, main_header, amalg) != 0)
        {
            rc = -1;
            goto cleanup;
        }
    }

cleanup:
    ecs_os_free(project_id);
    ecs_os_free(include_path);
    ecs_os_free(src_path);
    ecs_os_free(main_header);
    return rc;
}
