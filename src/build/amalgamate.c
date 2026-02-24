#include "build_internal.h"
#include "bake2/os.h"

typedef struct bake_concat_ctx_t {
    ecs_strbuf_t *out;
    const char *ext;
    const char *base;
} bake_concat_ctx_t;

static int bake_has_suffix(const char *value, const char *suffix) {
    size_t value_len = strlen(value);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > value_len) {
        return 0;
    }
    return strcmp(value + value_len - suffix_len, suffix) == 0;
}

static int bake_concat_visit(const bake_dir_entry_t *entry, void *ctx_ptr) {
    bake_concat_ctx_t *ctx = ctx_ptr;
    if (entry->is_dir) {
        if (!strcmp(entry->name, ".") || !strcmp(entry->name, "..") || entry->name[0] == '.' || !strcmp(entry->name, "build")) {
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
            .ext = ".h",
            .base = include_dir
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
            .ext = ".c",
            .base = src_dir
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
