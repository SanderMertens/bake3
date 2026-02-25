#include "build_internal.h"
#include "bake2/os.h"

static int bake_has_suffix(const char *value, const char *suffix) {
    size_t value_len = strlen(value);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > value_len) {
        return 0;
    }
    return strcmp(value + value_len - suffix_len, suffix) == 0;
}

static int bake_is_c_source(const char *path, bool *cpp_out) {
    if (bake_has_suffix(path, ".c")) {
        *cpp_out = false;
        return 1;
    }

    if (bake_has_suffix(path, ".cpp") || bake_has_suffix(path, ".cc") || bake_has_suffix(path, ".cxx") || bake_has_suffix(path, ".C")) {
        *cpp_out = true;
        return 1;
    }

    return 0;
}

static char* bake_rel_path(const char *base, const char *path) {
    size_t base_len = strlen(base);
    if (!strncmp(base, path, base_len) && (path[base_len] == bake_os_path_sep() || path[base_len] == '/')) {
        return bake_strdup(path + base_len + 1);
    }
    return bake_basename(path);
}

void bake_compile_list_init(bake_compile_list_t *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void bake_compile_list_fini(bake_compile_list_t *list) {
    for (int32_t i = 0; i < list->count; i++) {
        ecs_os_free(list->items[i].src);
        ecs_os_free(list->items[i].obj);
        ecs_os_free(list->items[i].dep);
    }
    ecs_os_free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

int bake_compile_list_append(
    bake_compile_list_t *list,
    const char *src,
    const char *obj,
    const char *dep,
    bool cpp)
{
    if (list->count == list->capacity) {
        int32_t next = list->capacity ? list->capacity * 2 : 32;
        bake_compile_unit_t *items = ecs_os_realloc_n(list->items, bake_compile_unit_t, next);
        if (!items) {
            return -1;
        }
        list->items = items;
        list->capacity = next;
    }

    bake_compile_unit_t *unit = &list->items[list->count++];
    unit->src = bake_strdup(src);
    unit->obj = bake_strdup(obj);
    unit->dep = dep ? bake_strdup(dep) : NULL;
    unit->cpp = cpp;
    if (!unit->src || !unit->obj || (dep && !unit->dep)) {
        return -1;
    }
    return 0;
}

void bake_build_paths_fini(bake_build_paths_t *paths) {
    ecs_os_free(paths->build_root);
    ecs_os_free(paths->obj_dir);
    ecs_os_free(paths->bin_dir);
    ecs_os_free(paths->lib_dir);
    ecs_os_free(paths->gen_dir);
    memset(paths, 0, sizeof(*paths));
}

int bake_build_paths_init(const bake_project_cfg_t *cfg, const char *mode, bake_build_paths_t *paths) {
    memset(paths, 0, sizeof(*paths));

    paths->build_root = bake_project_build_root(cfg->path, mode);
    if (!paths->build_root) {
        ecs_err("bake_build_paths_init: failed to build build_root");
        return -1;
    }

    paths->obj_dir = bake_join_path(paths->build_root, "obj");
    paths->bin_dir = bake_strdup(paths->build_root);
    paths->lib_dir = bake_strdup(paths->build_root);
    paths->gen_dir = bake_join_path(paths->build_root, "generated");

    if (!paths->obj_dir || !paths->bin_dir || !paths->lib_dir || !paths->gen_dir) {
        ecs_err("bake_build_paths_init: failed to allocate path(s)");
        bake_build_paths_fini(paths);
        return -1;
    }

    if (bake_mkdirs(paths->build_root) != 0 ||
        bake_mkdirs(paths->obj_dir) != 0 ||
        bake_mkdirs(paths->gen_dir) != 0)
    {
        ecs_err("bake_build_paths_init: mkdir failed for %s", paths->build_root);
        bake_build_paths_fini(paths);
        return -1;
    }

    return 0;
}

typedef struct bake_collect_ctx_t {
    const bake_project_cfg_t *cfg;
    const bake_build_paths_t *paths;
    bake_compile_list_t *units;
} bake_collect_ctx_t;

static int bake_collect_visit(const bake_dir_entry_t *entry, void *ctx_ptr) {
    bake_collect_ctx_t *ctx = ctx_ptr;

    if (entry->is_dir) {
        if (!strcmp(entry->name, ".") || !strcmp(entry->name, "..")) {
            return 1;
        }
        if (entry->name[0] == '.') {
            return 1;
        }
        return 0;
    }

    bool cpp = false;
    if (!bake_is_c_source(entry->path, &cpp)) {
        return 0;
    }

    char *rel = bake_rel_path(ctx->cfg->path, entry->path);
    if (!rel) {
        return -1;
    }

    for (char *p = rel; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = '_';
        }
    }

    char *stem = bake_stem(rel);
    ecs_os_free(rel);
    if (!stem) {
        return -1;
    }

#if defined(_WIN32)
    const char *obj_ext = ".obj";
#else
    const char *obj_ext = ".o";
#endif

    char *obj_file = bake_asprintf("%s%s", stem, obj_ext);
    ecs_os_free(stem);
    if (!obj_file) {
        return -1;
    }

    char *obj_path = bake_join_path(ctx->paths->obj_dir, obj_file);
    ecs_os_free(obj_file);
    if (!obj_path) {
        return -1;
    }

#if defined(_WIN32)
    const char *dep_path = NULL;
#else
    char *dep_path = bake_asprintf("%s.d", obj_path);
    if (!dep_path) {
        ecs_os_free(obj_path);
        return -1;
    }
#endif

    int rc = bake_compile_list_append(ctx->units, entry->path, obj_path, dep_path, cpp);
#if !defined(_WIN32)
    ecs_os_free(dep_path);
#endif
    ecs_os_free(obj_path);
    return rc;
}

int bake_collect_compile_units(const bake_project_cfg_t *cfg, const bake_build_paths_t *paths, bool include_tests, bake_compile_list_t *units) {
    bake_collect_ctx_t ctx = {
        .cfg = cfg,
        .paths = paths,
        .units = units
    };

    char *src = bake_join_path(cfg->path, "src");
    if (src && bake_path_exists(src)) {
        if (bake_dir_walk_recursive(src, bake_collect_visit, &ctx) != 0) {
            ecs_os_free(src);
            return -1;
        }
    }
    ecs_os_free(src);

    char *deps = bake_join_path(cfg->path, "deps");
    if (deps && bake_path_exists(deps)) {
        if (bake_dir_walk_recursive(deps, bake_collect_visit, &ctx) != 0) {
            ecs_os_free(deps);
            return -1;
        }
    }
    ecs_os_free(deps);

    if (include_tests) {
        char *test = bake_join_path(cfg->path, "test");
        if (test && bake_path_exists(test)) {
            if (bake_dir_walk_recursive(test, bake_collect_visit, &ctx) != 0) {
                ecs_os_free(test);
                return -1;
            }
        }
        ecs_os_free(test);
    }

    return 0;
}

static char* bake_replace_all(const char *input, const char *needle, const char *replacement) {
    const char *cur = input;
    size_t needle_len = strlen(needle);
    size_t repl_len = strlen(replacement);
    size_t out_len = 1;

    while (true) {
        const char *hit = strstr(cur, needle);
        if (!hit) {
            out_len += strlen(cur);
            break;
        }
        out_len += (size_t)(hit - cur) + repl_len;
        cur = hit + needle_len;
    }

    char *out = ecs_os_malloc(out_len);
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

typedef struct bake_rule_exec_ctx_t {
    const bake_project_cfg_t *cfg;
    const bake_build_paths_t *paths;
    const bake_rule_t *rule;
} bake_rule_exec_ctx_t;

static int bake_rule_visit(const bake_dir_entry_t *entry, void *ctx_ptr) {
    bake_rule_exec_ctx_t *ctx = ctx_ptr;
    if (entry->is_dir) {
        if (!strcmp(entry->name, ".") || !strcmp(entry->name, "..") || entry->name[0] == '.') {
            return 1;
        }
        return 0;
    }

    if (!bake_has_suffix(entry->path, ctx->rule->ext)) {
        return 0;
    }

    char *stem = bake_stem(entry->path);
    if (!stem) {
        return -1;
    }

    char *cmd = bake_replace_all(ctx->rule->command, "{input}", entry->path);
    if (!cmd) {
        ecs_os_free(stem);
        return -1;
    }

    char *tmp = bake_replace_all(cmd, "{project}", ctx->cfg->path);
    ecs_os_free(cmd);
    if (!tmp) {
        ecs_os_free(stem);
        return -1;
    }

    cmd = bake_replace_all(tmp, "{out_dir}", ctx->paths->gen_dir);
    ecs_os_free(tmp);
    if (!cmd) {
        ecs_os_free(stem);
        return -1;
    }

    tmp = bake_replace_all(cmd, "{stem}", stem);
    ecs_os_free(cmd);
    ecs_os_free(stem);
    if (!tmp) {
        return -1;
    }

    int rc = bake_run_command(tmp);
    ecs_os_free(tmp);
    return rc;
}

int bake_execute_rules(const bake_project_cfg_t *cfg, const bake_build_paths_t *paths) {
    for (int32_t i = 0; i < cfg->rules.count; i++) {
        bake_rule_exec_ctx_t ctx = {
            .cfg = cfg,
            .paths = paths,
            .rule = &cfg->rules.items[i]
        };

        if (bake_dir_walk_recursive(cfg->path, bake_rule_visit, &ctx) != 0) {
            return -1;
        }
    }

    return 0;
}

int bake_generate_dep_header(ecs_world_t *world, const bake_project_cfg_t *cfg, const bake_build_paths_t *paths) {
    ecs_entity_t project = 0;
    const BakeProject *p = bake_model_find_project(world, cfg->id, &project);
    if (!p || !project) {
        return -1;
    }

    ecs_strbuf_t header = ECS_STRBUF_INIT;
    ecs_strbuf_appendstr(&header, "/* Generated by bake2 */\n");
    ecs_strbuf_appendstr(&header, "#pragma once\n\n");

    for (int32_t i = 0;; i++) {
        ecs_entity_t dep = ecs_get_target(world, project, BakeDependsOn, i);
        if (!dep) {
            break;
        }

        const BakeProject *dep_p = ecs_get(world, dep, BakeProject);
        if (!dep_p || !dep_p->cfg || !dep_p->cfg->id) {
            continue;
        }

        char *include = bake_strdup(dep_p->cfg->id);
        if (!include) {
            continue;
        }

        for (char *c = include; *c; c++) {
            if (*c == '.') {
                *c = '/';
            }
        }

        ecs_strbuf_append(&header, "#include <%s.h>\n", include);
        ecs_os_free(include);
    }

    char *file = bake_join_path(paths->gen_dir, "dep_includes.h");
    char *content = ecs_strbuf_get(&header);
    int rc = bake_write_file(file, content);
    ecs_os_free(file);
    ecs_os_free(content);

    return rc;
}

static void bake_merge_strlist_unique(bake_strlist_t *dst, const bake_strlist_t *src) {
    for (int32_t i = 0; i < src->count; i++) {
        if (!bake_strlist_contains(dst, src->items[i])) {
            bake_strlist_append(dst, src->items[i]);
        }
    }
}

static void bake_merge_lang_cfg_unique(
    bake_lang_cfg_t *dst,
    const bake_lang_cfg_t *src)
{
    bake_merge_strlist_unique(&dst->cflags, &src->cflags);
    bake_merge_strlist_unique(&dst->cxxflags, &src->cxxflags);
    bake_merge_strlist_unique(&dst->defines, &src->defines);
    bake_merge_strlist_unique(&dst->ldflags, &src->ldflags);
    bake_merge_strlist_unique(&dst->libs, &src->libs);
    bake_merge_strlist_unique(&dst->static_libs, &src->static_libs);
    bake_merge_strlist_unique(&dst->libpaths, &src->libpaths);
    bake_merge_strlist_unique(&dst->links, &src->links);
    bake_merge_strlist_unique(&dst->include_paths, &src->include_paths);
}

int bake_apply_dependee_config(
    ecs_world_t *world,
    ecs_entity_t project_entity,
    bake_lang_cfg_t *dst,
    bool cpp_lang)
{
    for (int32_t i = 0;; i++) {
        ecs_entity_t dep = ecs_get_target(world, project_entity, BakeDependsOn, i);
        if (!dep) {
            break;
        }

        const BakeProject *dep_p = ecs_get(world, dep, BakeProject);
        if (!dep_p || !dep_p->cfg) {
            continue;
        }

        if (!dep_p->cfg->dependee.cfg) {
            continue;
        }

        const bake_lang_cfg_t *dep_lang = cpp_lang
            ? &dep_p->cfg->dependee.cfg->cpp_lang
            : &dep_p->cfg->dependee.cfg->c_lang;
        bake_merge_lang_cfg_unique(dst, dep_lang);
    }

    return 0;
}
