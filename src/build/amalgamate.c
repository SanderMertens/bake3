#include "build_internal.h"
#include "bake2/os.h"

#include <ctype.h>

#define B2_AMALG_MAX_LINE (4096)

typedef struct bake_concat_ctx_t {
    ecs_strbuf_t *out;
    const char *ext;
} bake_concat_ctx_t;

typedef struct bake_amalgamate_ctx_t {
    const bake_project_cfg_t *cfg;
    const char *project_id;
    const char *include_path;
} bake_amalgamate_ctx_t;

typedef struct bake_collect_sources_ctx_t {
    bake_strlist_t *sources;
} bake_collect_sources_ctx_t;

static int bake_has_suffix(const char *value, const char *suffix) {
    size_t value_len = strlen(value);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > value_len) {
        return 0;
    }
    return strcmp(value + value_len - suffix_len, suffix) == 0;
}

static bool bake_source_is_cpp(const bake_project_cfg_t *cfg) {
    return cfg->language && !strcmp(cfg->language, "cpp");
}

static int bake_concat_visit(const bake_dir_entry_t *entry, void *ctx_ptr) {
    bake_concat_ctx_t *ctx = ctx_ptr;
    if (entry->is_dir) {
        if (!strcmp(entry->name, ".") || !strcmp(entry->name, "..") || entry->name[0] == '.') {
            return 1;
        }
        return 0;
    }

    if (!bake_has_suffix(entry->path, ctx->ext)) {
        return 0;
    }

    char *content = bake_read_file(entry->path, NULL);
    if (!content) {
        return -1;
    }

    char *rel = bake_basename(entry->path);
    ecs_strbuf_append(ctx->out, "\n/* --- %s --- */\n", rel ? rel : entry->path);
    ecs_strbuf_appendstr(ctx->out, content);
    ecs_strbuf_appendstr(ctx->out, "\n");

    ecs_os_free(rel);
    ecs_os_free(content);
    return 0;
}

int bake_amalgamate_project(const bake_project_cfg_t *cfg, const char *dst_dir, char **out_c, char **out_h) {
    if (bake_mkdirs(dst_dir) != 0) {
        return -1;
    }

    char *base = bake_stem(cfg->output_name);
    if (!base) {
        return -1;
    }

    char *h_name = bake_asprintf("%s_amalgamated.h", base);
    char *c_name = bake_asprintf("%s_amalgamated.c", base);
    ecs_os_free(base);
    if (!h_name || !c_name) {
        ecs_os_free(h_name);
        ecs_os_free(c_name);
        return -1;
    }

    char *h_path = bake_join_path(dst_dir, h_name);
    char *c_path = bake_join_path(dst_dir, c_name);
    if (!h_path || !c_path) {
        ecs_os_free(h_name);
        ecs_os_free(c_name);
        ecs_os_free(h_path);
        ecs_os_free(c_path);
        return -1;
    }

    ecs_strbuf_t h_buf = ECS_STRBUF_INIT;
    ecs_strbuf_t c_buf = ECS_STRBUF_INIT;

    ecs_strbuf_append(&h_buf, "/* Amalgamated headers for %s */\n", cfg->id);
    ecs_strbuf_append(&h_buf, "#ifndef %s_AMALGAMATED_H\n", cfg->output_name);
    ecs_strbuf_append(&h_buf, "#define %s_AMALGAMATED_H\n", cfg->output_name);

    char *include_dir = bake_join_path(cfg->path, "include");
    if (include_dir && bake_path_exists(include_dir)) {
        bake_concat_ctx_t ctx = {
            .out = &h_buf,
            .ext = ".h"
        };
        if (bake_dir_walk_recursive(include_dir, bake_concat_visit, &ctx) != 0) {
            ecs_os_free(include_dir);
            ecs_os_free(h_name);
            ecs_os_free(c_name);
            ecs_os_free(h_path);
            ecs_os_free(c_path);
            return -1;
        }
    }
    ecs_os_free(include_dir);

    ecs_strbuf_appendstr(&h_buf, "\n#endif\n");

    ecs_strbuf_append(&c_buf, "/* Amalgamated sources for %s */\n", cfg->id);
    ecs_strbuf_append(&c_buf, "#include \"%s\"\n", h_name);

    char *src_dir = bake_join_path(cfg->path, "src");
    if (src_dir && bake_path_exists(src_dir)) {
        bake_concat_ctx_t ctx = {
            .out = &c_buf,
            .ext = ".c"
        };
        if (bake_dir_walk_recursive(src_dir, bake_concat_visit, &ctx) != 0) {
            ecs_os_free(src_dir);
            ecs_os_free(h_name);
            ecs_os_free(c_name);
            ecs_os_free(h_path);
            ecs_os_free(c_path);
            return -1;
        }
    }
    ecs_os_free(src_dir);

    char *h_content = ecs_strbuf_get(&h_buf);
    char *c_content = ecs_strbuf_get(&c_buf);

    int rc = bake_write_file(h_path, h_content);
    if (rc == 0) {
        rc = bake_write_file(c_path, c_content);
    }

    ecs_os_free(h_content);
    ecs_os_free(c_content);
    ecs_os_free(h_name);
    ecs_os_free(c_name);

    if (rc != 0) {
        ecs_os_free(h_path);
        ecs_os_free(c_path);
        return -1;
    }

    if (out_h) {
        *out_h = h_path;
    } else {
        ecs_os_free(h_path);
    }
    if (out_c) {
        *out_c = c_path;
    } else {
        ecs_os_free(c_path);
    }

    return 0;
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

static char* bake_path_normalize(const char *path) {
    char *out = bake_strdup(path);
    if (!out) {
        return NULL;
    }
    for (char *p = out; *p; p++) {
        if (*p == '\\') {
            *p = '/';
        }
    }
    return out;
}

static char* bake_path_canonical(const char *path) {
#if defined(_WIN32)
    char buffer[PATH_MAX];
    if (_fullpath(buffer, path, PATH_MAX)) {
        return bake_path_normalize(buffer);
    }
#else
    char *resolved = realpath(path, NULL);
    if (resolved) {
        char *normalized = bake_path_normalize(resolved);
        free(resolved);
        return normalized;
    }
#endif
    return bake_path_normalize(path);
}

static bool bake_path_mark_parsed(bake_strlist_t *parsed, const char *path) {
    char *normalized = bake_path_canonical(path);
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

static int bake_amalgamate_file(
    const bake_amalgamate_ctx_t *ctx,
    FILE *out,
    bool is_include,
    const char *file,
    const char *src_file,
    int32_t src_line,
    bake_strlist_t *parsed,
    int64_t *latest_mtime,
    bool *main_included)
{
    if (!bake_path_mark_parsed(parsed, file)) {
        return 0;
    }

    int64_t mtime = bake_file_mtime(file);
    if (mtime > *latest_mtime) {
        *latest_mtime = mtime;
    }

    FILE *in = fopen(file, "rb");
    if (!in) {
        B2_ERR(
            "cannot read file '%s' while amalgamating '%s' (from '%s:%d')",
            file, ctx->cfg->id, src_file, src_line);
        return -1;
    }

    char *cur_path = bake_dirname(file);
    char *base_name = bake_basename(file);
    bool bake_config_h = base_name && !strcmp(base_name, "bake_config.h");
    ecs_os_free(base_name);

    char line[B2_AMALG_MAX_LINE];
    int32_t line_count = 0;
    while (fgets(line, B2_AMALG_MAX_LINE, in)) {
        line_count++;

        bool include_relative = false;
        char *include = bake_parse_include_file(line, &include_relative);
        if (!include) {
            fprintf(out, "%s", line);
            continue;
        }

        if (!is_include && main_included && !main_included[0]) {
            fprintf(out, "#include \"%s.h\"\n", ctx->project_id);
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

            include_path = bake_join_path(ctx->include_path, include);
            if (include_path && bake_path_exists(include_path)) {
                recurse = true;
            }
        } else {
            include_path = cur_path ? bake_join_path(cur_path, include) : NULL;
            if (!include_path || !bake_path_exists(include_path)) {
                ecs_os_free(include_path);
                include_path = bake_join_path(ctx->include_path, include);
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
                latest_mtime,
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
    fclose(in);
    return 0;
}

static char* bake_project_id_base(const char *id) {
    if (!id) {
        return NULL;
    }

    const char *dot = strrchr(id, '.');
    if (!dot || !dot[1]) {
        return bake_strdup(id);
    }

    return bake_strdup(dot + 1);
}

static int bake_try_source_name(
    const char *src_dir,
    const char *name,
    const char *ext,
    char **out)
{
    char *path = bake_asprintf("%s/%s.%s", src_dir, name, ext);
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
    const char *const *exts = bake_source_is_cpp(cfg) ? cpp_exts : c_exts;

    char *id_base = bake_project_id_base(cfg->id);
    if (!id_base) {
        return NULL;
    }

    char *found = NULL;
    for (int i = 0; exts[i] && !found; i++) {
        if (bake_try_source_name(src_dir, "main", exts[i], &found) != 0) {
            ecs_os_free(id_base);
            return NULL;
        }
        if (!found && bake_try_source_name(src_dir, project_id, exts[i], &found) != 0) {
            ecs_os_free(id_base);
            return NULL;
        }
        if (!found && bake_try_source_name(src_dir, id_base, exts[i], &found) != 0) {
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
        if (!strcmp(entry->name, ".") || !strcmp(entry->name, "..") || entry->name[0] == '.') {
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

static int bake_replace_if_changed(
    const char *tmp_file,
    const char *out_file,
    int64_t latest_input_mtime)
{
    int64_t output_mtime = bake_file_mtime(out_file);
    if (latest_input_mtime > output_mtime) {
        if (rename(tmp_file, out_file) != 0) {
            return -1;
        }
    } else {
        remove(tmp_file);
    }
    return 0;
}

int bake_generate_project_amalgamation(const bake_project_cfg_t *cfg) {
    if (!cfg || !cfg->amalgamate) {
        return 0;
    }

    char *project_id = bake_project_id_as_macro(cfg->id);
    if (!project_id) {
        return -1;
    }

    char *output_path = NULL;
    if (cfg->amalgamate_path && cfg->amalgamate_path[0]) {
        output_path = bake_join_path(cfg->path, cfg->amalgamate_path);
    } else {
        output_path = bake_strdup(cfg->path);
    }
    if (!output_path || bake_mkdirs(output_path) != 0) {
        ecs_os_free(project_id);
        ecs_os_free(output_path);
        return -1;
    }

    const char *src_ext = bake_source_is_cpp(cfg) ? "cpp" : "c";
    char *include_out = bake_asprintf("%s/%s.h", output_path, project_id);
    char *include_tmp = bake_asprintf("%s/%s.h.tmp", output_path, project_id);
    char *src_out = bake_asprintf("%s/%s.%s", output_path, project_id, src_ext);
    char *src_tmp = bake_asprintf("%s/%s.%s.tmp", output_path, project_id, src_ext);
    char *include_path = bake_join_path(cfg->path, "include");
    char *src_path = bake_join_path(cfg->path, "src");
    char *main_header = include_path ? bake_asprintf("%s/%s.h", include_path, project_id) : NULL;
    if (!include_out || !include_tmp || !src_out || !src_tmp ||
        !include_path || !src_path || !main_header)
    {
        ecs_os_free(project_id);
        ecs_os_free(output_path);
        ecs_os_free(include_out);
        ecs_os_free(include_tmp);
        ecs_os_free(src_out);
        ecs_os_free(src_tmp);
        ecs_os_free(include_path);
        ecs_os_free(src_path);
        ecs_os_free(main_header);
        return -1;
    }

    if (!bake_path_exists(main_header)) {
        B2_ERR("cannot find include file '%s' for amalgamation", main_header);
        ecs_os_free(project_id);
        ecs_os_free(output_path);
        ecs_os_free(include_out);
        ecs_os_free(include_tmp);
        ecs_os_free(src_out);
        ecs_os_free(src_tmp);
        ecs_os_free(include_path);
        ecs_os_free(src_path);
        ecs_os_free(main_header);
        return -1;
    }

    bake_amalgamate_ctx_t ctx = {
        .cfg = cfg,
        .project_id = project_id,
        .include_path = include_path
    };

    int64_t latest_input_mtime = 0;
    bake_strlist_t parsed;
    bake_strlist_init(&parsed);

    FILE *include_fp = fopen(include_tmp, "wb");
    if (!include_fp) {
        bake_strlist_fini(&parsed);
        ecs_os_free(project_id);
        ecs_os_free(output_path);
        ecs_os_free(include_out);
        ecs_os_free(include_tmp);
        ecs_os_free(src_out);
        ecs_os_free(src_tmp);
        ecs_os_free(include_path);
        ecs_os_free(src_path);
        ecs_os_free(main_header);
        return -1;
    }

    fprintf(include_fp, "// Comment out this line when using as DLL\n");
    fprintf(include_fp, "#define %s_STATIC\n", project_id);
    if (bake_amalgamate_file(
        &ctx,
        include_fp,
        true,
        main_header,
        "(main header)",
        0,
        &parsed,
        &latest_input_mtime,
        NULL) != 0)
    {
        fclose(include_fp);
        remove(include_tmp);
        bake_strlist_fini(&parsed);
        ecs_os_free(project_id);
        ecs_os_free(output_path);
        ecs_os_free(include_out);
        ecs_os_free(include_tmp);
        ecs_os_free(src_out);
        ecs_os_free(src_tmp);
        ecs_os_free(include_path);
        ecs_os_free(src_path);
        ecs_os_free(main_header);
        return -1;
    }
    fclose(include_fp);

    FILE *src_fp = fopen(src_tmp, "wb");
    if (!src_fp) {
        remove(include_tmp);
        bake_strlist_fini(&parsed);
        ecs_os_free(project_id);
        ecs_os_free(output_path);
        ecs_os_free(include_out);
        ecs_os_free(include_tmp);
        ecs_os_free(src_out);
        ecs_os_free(src_tmp);
        ecs_os_free(include_path);
        ecs_os_free(src_path);
        ecs_os_free(main_header);
        return -1;
    }

    bool main_included = false;
    char *main_src = bake_find_main_src_file(cfg, src_path, project_id);
    if (main_src) {
        if (bake_amalgamate_file(
            &ctx,
            src_fp,
            false,
            main_src,
            "(main source)",
            0,
            &parsed,
            &latest_input_mtime,
            &main_included) != 0)
        {
            fclose(src_fp);
            remove(include_tmp);
            remove(src_tmp);
            ecs_os_free(main_src);
            bake_strlist_fini(&parsed);
            ecs_os_free(project_id);
            ecs_os_free(output_path);
            ecs_os_free(include_out);
            ecs_os_free(include_tmp);
            ecs_os_free(src_out);
            ecs_os_free(src_tmp);
            ecs_os_free(include_path);
            ecs_os_free(src_path);
            ecs_os_free(main_header);
            return -1;
        }
    }

    bake_strlist_t sources;
    bake_strlist_init(&sources);
    if (bake_collect_source_files(src_path, &sources) != 0) {
        fclose(src_fp);
        remove(include_tmp);
        remove(src_tmp);
        ecs_os_free(main_src);
        bake_strlist_fini(&sources);
        bake_strlist_fini(&parsed);
        ecs_os_free(project_id);
        ecs_os_free(output_path);
        ecs_os_free(include_out);
        ecs_os_free(include_tmp);
        ecs_os_free(src_out);
        ecs_os_free(src_tmp);
        ecs_os_free(include_path);
        ecs_os_free(src_path);
        ecs_os_free(main_header);
        return -1;
    }

    for (int32_t i = 0; i < sources.count; i++) {
        if (main_src && !strcmp(sources.items[i], main_src)) {
            continue;
        }
        if (bake_amalgamate_file(
            &ctx,
            src_fp,
            false,
            sources.items[i],
            "(source)",
            0,
            &parsed,
            &latest_input_mtime,
            &main_included) != 0)
        {
            fclose(src_fp);
            remove(include_tmp);
            remove(src_tmp);
            ecs_os_free(main_src);
            bake_strlist_fini(&sources);
            bake_strlist_fini(&parsed);
            ecs_os_free(project_id);
            ecs_os_free(output_path);
            ecs_os_free(include_out);
            ecs_os_free(include_tmp);
            ecs_os_free(src_out);
            ecs_os_free(src_tmp);
            ecs_os_free(include_path);
            ecs_os_free(src_path);
            ecs_os_free(main_header);
            return -1;
        }
    }

    if (!main_included) {
        fprintf(src_fp, "#include \"%s.h\"\n", project_id);
    }
    fclose(src_fp);

    int rc = 0;
    if (bake_replace_if_changed(include_tmp, include_out, latest_input_mtime) != 0) {
        rc = -1;
    } else if (bake_replace_if_changed(src_tmp, src_out, latest_input_mtime) != 0) {
        rc = -1;
    }

    ecs_os_free(main_src);
    bake_strlist_fini(&sources);
    bake_strlist_fini(&parsed);
    ecs_os_free(project_id);
    ecs_os_free(output_path);
    ecs_os_free(include_out);
    ecs_os_free(include_tmp);
    ecs_os_free(src_out);
    ecs_os_free(src_tmp);
    ecs_os_free(include_path);
    ecs_os_free(src_path);
    ecs_os_free(main_header);
    return rc;
}
