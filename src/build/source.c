#include "build_internal.h"
#include "bake/os.h"

#include <ctype.h>

static int bake_has_suffix(const char *value, const char *suffix) {
    size_t value_len = strlen(value);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > value_len) {
        return 0;
    }
    return strcmp(value + value_len - suffix_len, suffix) == 0;
}

static int bake_is_compile_source(const char *path, bool *cpp_out) {
    if (bake_has_suffix(path, ".c")) {
        *cpp_out = false;
        return 1;
    }

    if (bake_has_suffix(path, ".cpp") || bake_has_suffix(path, ".cc") || bake_has_suffix(path, ".cxx") || bake_has_suffix(path, ".C")) {
        *cpp_out = true;
        return 1;
    }

#if defined(__APPLE__)
    if (bake_has_suffix(path, ".m")) {
        *cpp_out = false;
        return 1;
    }

    if (bake_has_suffix(path, ".mm")) {
        *cpp_out = true;
        return 1;
    }
#endif

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
    if (!bake_is_compile_source(entry->path, &cpp)) {
        return 0;
    }

    char *rel = bake_rel_path(ctx->cfg->path, entry->path);
    if (!rel) {
        return -1;
    }

    for (char *p = rel; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == '.' || *p == ':') {
            *p = '_';
        }
    }

#if defined(_WIN32)
    const char *obj_ext = ".obj";
#else
    const char *obj_ext = ".o";
#endif

    char *obj_file = flecs_asprintf("%s%s", rel, obj_ext);
    ecs_os_free(rel);
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
    char *dep_path = flecs_asprintf("%s.d", obj_path);
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

int bake_collect_compile_units(
    const bake_project_cfg_t *cfg,
    const bake_build_paths_t *paths,
    bool include_tests,
    bool include_deps,
    bake_compile_list_t *units)
{
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

    if (include_deps) {
        char *deps = bake_join_path(cfg->path, "deps");
        if (deps && bake_path_exists(deps)) {
            if (bake_dir_walk_recursive(deps, bake_collect_visit, &ctx) != 0) {
                ecs_os_free(deps);
                return -1;
            }
        }
        ecs_os_free(deps);
    }

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
    const char *ext;
    const char *command;
} bake_rule_exec_ctx_t;

static int bake_rule_visit(const bake_dir_entry_t *entry, void *ctx_ptr) {
    bake_rule_exec_ctx_t *ctx = ctx_ptr;
    if (entry->is_dir) {
        if (!strcmp(entry->name, ".") || !strcmp(entry->name, "..") || entry->name[0] == '.') {
            return 1;
        }
        return 0;
    }

    if (!bake_has_suffix(entry->path, ctx->ext)) {
        return 0;
    }

    char *stem = bake_stem(entry->path);
    if (!stem) {
        return -1;
    }

    char *cmd = bake_replace_all(ctx->command, "{input}", entry->path);
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

int bake_execute_rules(
    ecs_world_t *world,
    ecs_entity_t project_entity,
    const bake_project_cfg_t *cfg,
    const bake_build_paths_t *paths)
{
    ecs_iter_t children = ecs_children(world, project_entity);
    while (ecs_children_next(&children)) {
        for (int32_t i = 0; i < children.count; i++) {
            const BakeBuildRule *rule = ecs_get(world, children.entities[i], BakeBuildRule);
            if (!rule || !rule->ext || !rule->command) {
                continue;
            }

            bake_rule_exec_ctx_t ctx = {
                .cfg = cfg,
                .paths = paths,
                .ext = rule->ext,
                .command = rule->command
            };
            if (bake_dir_walk_recursive(cfg->path, bake_rule_visit, &ctx) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

static char* bake_project_id_as_dash(const char *id) {
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

static char* bake_macro_upper(const char *value) {
    if (!value) {
        return NULL;
    }

    char *out = bake_strdup(value);
    if (!out) {
        return NULL;
    }

    for (char *ptr = out; *ptr; ptr++) {
        *ptr = (char)toupper((unsigned char)*ptr);
    }

    return out;
}

static int bake_append_dep_include(
    ecs_strbuf_t *header,
    const char *dep_id,
    bool standalone_local_headers)
{
    char *dep_macro = bake_project_id_as_macro(dep_id);
    if (!dep_macro) {
        return -1;
    }

    if (standalone_local_headers) {
        ecs_strbuf_append(header, "#include \"../../deps/%s.h\"\n", dep_macro);
    } else {
        ecs_strbuf_append(header, "#include <%s.h>\n", dep_macro);
    }

    ecs_os_free(dep_macro);
    return 0;
}

static int bake_append_dep_includes(
    ecs_strbuf_t *header,
    const bake_strlist_t *deps,
    bool standalone_local_headers)
{
    if (!deps || deps->count == 0) {
        ecs_strbuf_appendstr(header, "/* No dependencies */\n");
        return 0;
    }

    for (int32_t i = 0; i < deps->count; i++) {
        if (bake_append_dep_include(header, deps->items[i], standalone_local_headers) != 0) {
            return -1;
        }
    }

    return 0;
}

int bake_generate_config_header(ecs_world_t *world, const bake_project_cfg_t *cfg) {
    int rc = -1;
    ecs_entity_t project = 0;
    const BakeProject *p = bake_model_find_project(world, cfg->id, &project);
    if (!p || !project) {
        return -1;
    }

    char *include_root = NULL;
    char *project_dir = NULL;
    char *header_path = NULL;
    char *project_macro = NULL;
    char *project_macro_upper = NULL;
    char *guard_macro = NULL;
    char *api_macro = NULL;
    char *content = NULL;
    char *existing_content = NULL;
    bool public_deps_ready = false;
    bake_strlist_t public_deps = {0};
    ecs_strbuf_t header = ECS_STRBUF_INIT;

    if (!cfg || !cfg->id || !cfg->path) {
        goto cleanup;
    }

    include_root = bake_join_path(cfg->path, "include");
    if (!include_root) {
        goto cleanup;
    }

    if (!bake_path_exists(include_root)) {
        rc = 0;
        goto cleanup;
    }

    project_dir = bake_project_id_as_dash(cfg->id);
    if (!project_dir) {
        goto cleanup;
    }

    char *tmp = bake_join_path(include_root, project_dir);
    if (!tmp) {
        goto cleanup;
    }
    ecs_os_free(project_dir);
    project_dir = tmp;
    tmp = NULL;

    if (bake_mkdirs(project_dir) != 0) {
        goto cleanup;
    }

    header_path = bake_join_path(project_dir, "bake_config.h");
    project_macro = bake_project_id_as_macro(cfg->id);
    project_macro_upper = bake_macro_upper(project_macro);
    guard_macro = project_macro_upper ? flecs_asprintf("%s_BAKE_CONFIG_H", project_macro_upper) : NULL;
    api_macro = project_macro_upper ? flecs_asprintf("%s_API", project_macro_upper) : NULL;
    if (!header_path || !project_macro || !project_macro_upper || !guard_macro || !api_macro) {
        goto cleanup;
    }

    bool standalone_local_headers =
        cfg->standalone &&
        (cfg->kind == BAKE_PROJECT_APPLICATION || cfg->kind == BAKE_PROJECT_TEST);

    bake_strlist_init(&public_deps);
    public_deps_ready = true;

    for (int32_t i = 0; i < cfg->use.count; i++) {
        if (!bake_strlist_contains(&public_deps, cfg->use.items[i])) {
            bake_strlist_append(&public_deps, cfg->use.items[i]);
        }
    }

    for (int32_t i = 0; i < cfg->use.count; i++) {
        const BakeProject *dep_p = bake_model_find_project(world, cfg->use.items[i], NULL);
        if (!dep_p || !dep_p->cfg || !dep_p->cfg->dependee.cfg) {
            continue;
        }

        const bake_strlist_t *dependee_use = &dep_p->cfg->dependee.cfg->use;
        for (int32_t d = 0; d < dependee_use->count; d++) {
            const char *dep_id = dependee_use->items[d];
            if (!dep_id) {
                continue;
            }

            if (bake_strlist_contains(&cfg->use_private, dep_id)) {
                continue;
            }
            if (!bake_strlist_contains(&public_deps, dep_id)) {
                bake_strlist_append(&public_deps, dep_id);
            }
        }
    }

    ecs_strbuf_appendstr(&header,
        "/*\n"
        "                                   )\n"
        "                                  (.)\n"
        "                                  .|.\n"
        "                                  | |\n"
        "                              _.--| |--._\n"
        "                           .-';  ;`-'& ; `&.\n"
        "                          \\   &  ;    &   &_/\n"
        "                           |\"\"\"---...---\"\"\"|\n"
        "                           \\ | | | | | | | /\n"
        "                            `---.|.|.|.---'\n"
        "\n"
        " * This file is generated by bake.lang.c for your convenience. Headers of\n"
        " * dependencies will automatically show up in this file. Include bake_config.h\n"
        " * in your main project file. Do not edit! */\n\n");

    ecs_strbuf_append(&header, "#ifndef %s\n", guard_macro);
    ecs_strbuf_append(&header, "#define %s\n\n", guard_macro);

    ecs_strbuf_appendstr(&header, "/* Headers of public dependencies */\n");
    if (bake_append_dep_includes(&header, &public_deps, standalone_local_headers) != 0) {
        goto cleanup;
    }
    if (cfg->kind == BAKE_PROJECT_TEST && cfg->has_test_spec) {
        ecs_strbuf_appendstr(&header, "#include <bake_test.h>\n");
    }
    ecs_strbuf_appendstr(&header, "\n");

    if (cfg->use_private.count) {
        ecs_strbuf_appendstr(&header, "/* Headers of private dependencies */\n");
        if (cfg->kind == BAKE_PROJECT_PACKAGE) {
            ecs_strbuf_append(&header, "#ifdef %s_EXPORTS\n", project_macro);
            if (bake_append_dep_includes(&header, &cfg->use_private, standalone_local_headers) != 0) {
                goto cleanup;
            }
            ecs_strbuf_appendstr(&header, "#endif\n\n");
        } else {
            if (bake_append_dep_includes(&header, &cfg->use_private, standalone_local_headers) != 0) {
                goto cleanup;
            }
            ecs_strbuf_appendstr(&header, "\n");
        }
    }

    if (cfg->kind == BAKE_PROJECT_PACKAGE) {
        ecs_strbuf_appendstr(&header, "/* Convenience macro for exporting symbols */\n");
        ecs_strbuf_append(&header, "#ifndef %s_STATIC\n", project_macro);
        ecs_strbuf_append(&header, "#if defined(%s_EXPORTS) && (defined(_MSC_VER) || defined(__MINGW32__))\n", project_macro);
        ecs_strbuf_append(&header, "  #define %s __declspec(dllexport)\n", api_macro);
        ecs_strbuf_append(&header, "#elif defined(%s_EXPORTS)\n", project_macro);
        ecs_strbuf_append(&header, "  #define %s __attribute__((__visibility__(\"default\")))\n", api_macro);
        ecs_strbuf_appendstr(&header, "#elif defined(_MSC_VER)\n");
        ecs_strbuf_append(&header, "  #define %s __declspec(dllimport)\n", api_macro);
        ecs_strbuf_appendstr(&header, "#else\n");
        ecs_strbuf_append(&header, "  #define %s\n", api_macro);
        ecs_strbuf_appendstr(&header, "#endif\n");
        ecs_strbuf_appendstr(&header, "#else\n");
        ecs_strbuf_append(&header, "  #define %s\n", api_macro);
        ecs_strbuf_appendstr(&header, "#endif\n\n");
    }

    ecs_strbuf_appendstr(&header, "#endif\n\n");

    content = ecs_strbuf_get(&header);
    if (!content) {
        goto cleanup;
    }

    if (bake_path_exists(header_path)) {
        existing_content = bake_read_file(header_path, NULL);
        if (existing_content && !strcmp(existing_content, content)) {
            rc = 0;
            goto cleanup;
        }
    }

    if (bake_write_file(header_path, content) != 0) {
        goto cleanup;
    }

    rc = 0;

cleanup:
    if (!content) {
        content = ecs_strbuf_get(&header);
    }
    ecs_os_free(content);
    ecs_os_free(include_root);
    ecs_os_free(project_dir);
    ecs_os_free(header_path);
    ecs_os_free(project_macro);
    ecs_os_free(project_macro_upper);
    ecs_os_free(guard_macro);
    ecs_os_free(api_macro);
    ecs_os_free(existing_content);
    if (public_deps_ready) {
        bake_strlist_fini(&public_deps);
    }
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
