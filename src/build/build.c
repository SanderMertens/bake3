#include "build_internal.h"
#include "bake2/environment.h"
#include "bake2/os.h"

#include <limits.h>
#include <stdlib.h>

static int bake_strlist_copy(bake_strlist_t *dst, const bake_strlist_t *src) {
    for (int32_t i = 0; i < src->count; i++) {
        if (bake_strlist_append(dst, src->items[i]) != 0) {
            return -1;
        }
    }
    return 0;
}

static int bake_lang_cfg_copy(bake_lang_cfg_t *dst, const bake_lang_cfg_t *src) {
    bake_lang_cfg_init(dst);

    ecs_os_free(dst->c_standard);
    ecs_os_free(dst->cpp_standard);
    dst->c_standard = bake_strdup(src->c_standard);
    dst->cpp_standard = bake_strdup(src->cpp_standard);
    dst->static_lib = src->static_lib;
    dst->export_symbols = src->export_symbols;
    dst->precompile_header = src->precompile_header;

    if (bake_strlist_copy(&dst->cflags, &src->cflags) != 0) return -1;
    if (bake_strlist_copy(&dst->cxxflags, &src->cxxflags) != 0) return -1;
    if (bake_strlist_copy(&dst->defines, &src->defines) != 0) return -1;
    if (bake_strlist_copy(&dst->ldflags, &src->ldflags) != 0) return -1;
    if (bake_strlist_copy(&dst->libs, &src->libs) != 0) return -1;
    if (bake_strlist_copy(&dst->static_libs, &src->static_libs) != 0) return -1;
    if (bake_strlist_copy(&dst->libpaths, &src->libpaths) != 0) return -1;
    if (bake_strlist_copy(&dst->links, &src->links) != 0) return -1;
    if (bake_strlist_copy(&dst->include_paths, &src->include_paths) != 0) return -1;

    return 0;
}

static int bake_mode_lists(
    const char *mode,
    bake_compiler_kind_t kind,
    bool strict,
    bake_strlist_t *cflags,
    bake_strlist_t *cxxflags,
    bake_strlist_t *ldflags)
{
    bake_strlist_init(cflags);
    bake_strlist_init(cxxflags);
    bake_strlist_init(ldflags);
    bake_add_mode_flags(mode, kind, cflags, cxxflags, ldflags);
    bake_add_strict_flags(strict, kind, cflags, cxxflags, ldflags);
    return 0;
}

static void bake_mode_lists_fini(bake_strlist_t *cflags, bake_strlist_t *cxxflags, bake_strlist_t *ldflags) {
    bake_strlist_fini(cflags);
    bake_strlist_fini(cxxflags);
    bake_strlist_fini(ldflags);
}

static bool bake_is_abs_path(const char *path) {
    if (!path || !path[0]) {
        return false;
    }
#if defined(_WIN32)
    if (path[0] == '\\' || path[0] == '/') {
        return true;
    }
    if (path[0] && path[1] == ':') {
        return true;
    }
    return false;
#else
    return path[0] == '/';
#endif
}

static bool bake_path_is_sep(char ch) {
    return ch == '/' || ch == '\\';
}

static bool bake_path_has_prefix(const char *path, const char *prefix, size_t *prefix_len_out) {
    if (!path || !prefix) {
        return false;
    }

    size_t prefix_len = strlen(prefix);
    while (prefix_len > 0 && bake_path_is_sep(prefix[prefix_len - 1])) {
        prefix_len--;
    }

    if (strncmp(path, prefix, prefix_len)) {
        return false;
    }

    if (path[prefix_len] && !bake_path_is_sep(path[prefix_len])) {
        return false;
    }

    if (prefix_len_out) {
        *prefix_len_out = prefix_len;
    }
    return true;
}

static bool bake_path_has_special_dir(const char *path) {
    if (!path || !path[0]) {
        return false;
    }

    const char *seg = path;
    for (const char *p = path;; p++) {
        if (*p && !bake_path_is_sep(*p)) {
            continue;
        }

        size_t len = (size_t)(p - seg);
        if (len) {
            if ((len == 4 && !strncmp(seg, "test", 4)) ||
                (len == 5 && !strncmp(seg, "tests", 5)) ||
                (len == 7 && !strncmp(seg, "example", 7)) ||
                (len == 8 && !strncmp(seg, "examples", 8)))
            {
                return true;
            }
        }

        if (!*p) {
            break;
        }
        seg = p + 1;
    }

    return false;
}

static char* bake_build_display_path(const bake_context_t *ctx, const char *path) {
    if (!path) {
        return bake_strdup(".");
    }

    const char *display = path;
    size_t prefix_len = 0;
    if (ctx && ctx->opts.cwd &&
        bake_path_has_prefix(path, ctx->opts.cwd, &prefix_len))
    {
        display = path + prefix_len;
        while (bake_path_is_sep(*display)) {
            display++;
        }
        if (!display[0]) {
            return bake_strdup(".");
        }
    }

    char *out = bake_strdup(display);
    if (!out) {
        return bake_strdup(".");
    }

    for (char *ch = out; *ch; ch++) {
        if (*ch == '\\') {
            *ch = '/';
        }
    }

    return out;
}

static void bake_log_build_header(const bake_context_t *ctx, const bake_project_cfg_t *cfg) {
    if (!cfg) {
        return;
    }

    const char *command = ctx && ctx->opts.command ? ctx->opts.command : "build";
    const char *kind = bake_project_kind_str(cfg->kind);
    const char *id = cfg->id ? cfg->id : "<unnamed>";
    char *path = bake_build_display_path(ctx, cfg->path);
    ecs_trace("#[green][#[normal]%s#[green]]#[normal] %s %s => '%s'", command, kind, id, path ? path : ".");
    ecs_os_free(path);
}

static char* bake_normalize_existing_path(const char *path) {
    if (!path || !path[0]) {
        return NULL;
    }

#if defined(_WIN32)
    char buf[_MAX_PATH];
    if (_fullpath(buf, path, _MAX_PATH)) {
        return bake_strdup(buf);
    }
#else
    char buf[PATH_MAX];
    if (realpath(path, buf)) {
        return bake_strdup(buf);
    }
#endif

    return bake_strdup(path);
}

static char* bake_resolve_target_path(const bake_context_t *ctx, const char *target) {
    if (!ctx || !target || !target[0]) {
        return NULL;
    }

    if (!bake_path_exists(target)) {
        return NULL;
    }

    if (bake_is_abs_path(target)) {
        return bake_normalize_existing_path(target);
    }

    char *joined = bake_join_path(ctx->opts.cwd, target);
    if (!joined) {
        return NULL;
    }
    char *normalized = bake_normalize_existing_path(joined);
    ecs_os_free(joined);
    return normalized;
}

static int bake_write_standalone_dep_header(
    const bake_project_cfg_t *cfg,
    const char *deps_dir)
{
    if (!cfg || !cfg->id) {
        return -1;
    }

    char *header_base = bake_project_id_as_macro(cfg->id);
    if (!header_base) {
        return -1;
    }

    char *header_name = bake_asprintf("%s.h", header_base);
    char *header_path = header_name ? bake_join_path(deps_dir, header_name) : NULL;
    char *header_content = header_name
        ? bake_asprintf("#pragma once\n#include <%s>\n", header_name)
        : NULL;

    int rc = -1;
    if (header_path && header_content) {
        rc = bake_write_file(header_path, header_content);
    }

    ecs_os_free(header_base);
    ecs_os_free(header_name);
    ecs_os_free(header_path);
    ecs_os_free(header_content);
    return rc;
}

static int bake_prepare_standalone_sources(
    bake_context_t *ctx,
    ecs_entity_t project_entity,
    const bake_project_cfg_t *cfg,
    bool emit_sources)
{

    char *deps_dir = bake_join_path(cfg->path, "deps");
    if (!deps_dir) {
        return -1;
    }

    if (bake_mkdirs(deps_dir) != 0) {
        ecs_os_free(deps_dir);
        return -1;
    }

    bake_dir_entry_t *entries = NULL;
    int32_t entry_count = 0;
    if (bake_dir_list(deps_dir, &entries, &entry_count) == 0) {
        for (int32_t i = 0; i < entry_count; i++) {
            if (!entries[i].is_dir) {
                remove(entries[i].path);
            }
        }
        bake_dir_entries_free(entries, entry_count);
    }

    for (int32_t i = 0;; i++) {
        ecs_entity_t dep = ecs_get_target(ctx->world, project_entity, BakeDependsOn, i);
        if (!dep) {
            break;
        }

        const BakeProject *dep_project = ecs_get(ctx->world, dep, BakeProject);
        if (!dep_project || !dep_project->cfg || !dep_project->cfg->path) {
            continue;
        }
        if (dep_project->cfg->kind == BAKE_PROJECT_CONFIG || dep_project->cfg->kind == BAKE_PROJECT_TEMPLATE) {
            continue;
        }

        if (!emit_sources) {
            if (bake_write_standalone_dep_header(dep_project->cfg, deps_dir) != 0) {
                ecs_os_free(deps_dir);
                return -1;
            }
            continue;
        }

        char *out_c = NULL;
        char *out_h = NULL;
        if (bake_amalgamate_project(dep_project->cfg, deps_dir, &out_c, &out_h) != 0) {
            ecs_os_free(deps_dir);
            return -1;
        }
        ecs_os_free(out_c);
        ecs_os_free(out_h);
    }

    ecs_os_free(deps_dir);
    return 0;
}

static int bake_build_one(bake_context_t *ctx, ecs_entity_t project_entity, const BakeBuildRequest *request) {
    const BakeProject *project = ecs_get(ctx->world, project_entity, BakeProject);
    if (!project || !project->cfg) {
        return -1;
    }

    const bake_project_cfg_t *cfg = project->cfg;
    char *builtin_test_src = NULL;
    if (project->external) {
        return 0;
    }

    if (bake_plugin_load_for_project(ctx, cfg) != 0) {
        ecs_err("plugin loading failed for %s", cfg->id);
        return -1;
    }

    if (cfg->kind == BAKE_PROJECT_CONFIG || cfg->kind == BAKE_PROJECT_TEMPLATE) {
        const BakeBuildResult *prev_result = ecs_get(ctx->world, project_entity, BakeBuildResult);
        if (prev_result && prev_result->artefact) {
            ecs_os_free((char*)prev_result->artefact);
        }

        BakeBuildResult result = { .status = 0, .artefact = NULL };
        ecs_set_ptr(ctx->world, project_entity, BakeBuildResult, &result);
        ecs_add(ctx->world, project_entity, BakeBuilt);
        if (bake_environment_sync_project(ctx, project_entity, &result, request, false) != 0) {
            return -1;
        }
        return 0;
    }

    bake_build_paths_t paths;
    if (bake_build_paths_init(cfg, request->mode, &paths) != 0) {
        ecs_err("failed to initialize build paths for %s (path=%s)", cfg->id, cfg->path ? cfg->path : "<null>");
        return -1;
    }

    if (cfg->kind == BAKE_PROJECT_TEST) {
        if (bake_test_generate_harness(cfg) != 0) {
            ecs_err("test harness generation failed for %s", cfg->id);
            bake_build_paths_fini(&paths);
            return -1;
        }

        if (cfg->has_test_spec) {
            if (bake_test_generate_builtin_api(cfg, paths.gen_dir, &builtin_test_src) != 0) {
                ecs_err("failed to generate test API for %s", cfg->id);
                bake_build_paths_fini(&paths);
                return -1;
            }
        }
    }

    if (cfg->rules.count && bake_execute_rules(cfg, &paths) != 0) {
        ecs_err("rule execution failed for %s", cfg->id);
        ecs_os_free(builtin_test_src);
        bake_build_paths_fini(&paths);
        return -1;
    }

    if (cfg->amalgamate) {
        if (bake_generate_project_amalgamation(cfg) != 0) {
            ecs_err("amalgamation failed for %s", cfg->id);
            ecs_os_free(builtin_test_src);
            bake_build_paths_fini(&paths);
            return -1;
        }
    }

    if (bake_generate_dep_header(ctx->world, cfg, &paths) != 0) {
        ecs_err("dependency header generation failed for %s", cfg->id);
        ecs_os_free(builtin_test_src);
        bake_build_paths_fini(&paths);
        return -1;
    }

    if ((request->standalone || cfg->standalone) && (cfg->kind == BAKE_PROJECT_APPLICATION || cfg->kind == BAKE_PROJECT_TEST)) {
        if (bake_prepare_standalone_sources(
            ctx, project_entity, cfg, request->standalone) != 0)
        {
            ecs_err("standalone amalgamation failed for %s", cfg->id);
            ecs_os_free(builtin_test_src);
            bake_build_paths_fini(&paths);
            return -1;
        }
    }

    bake_lang_cfg_t c_lang;
    bake_lang_cfg_t cpp_lang;
    if (bake_lang_cfg_copy(&c_lang, &cfg->c_lang) != 0 || bake_lang_cfg_copy(&cpp_lang, &cfg->cpp_lang) != 0) {
        ecs_err("failed to clone language config for %s", cfg->id);
        ecs_os_free(builtin_test_src);
        bake_build_paths_fini(&paths);
        return -1;
    }

    bake_apply_dependee_config(ctx->world, project_entity, &c_lang, false);
    bake_apply_dependee_config(ctx->world, project_entity, &cpp_lang, true);

    if (cfg->kind == BAKE_PROJECT_TEST) {
        if (!bake_strlist_contains(&c_lang.include_paths, paths.gen_dir)) {
            bake_strlist_append(&c_lang.include_paths, paths.gen_dir);
        }
        if (!bake_strlist_contains(&cpp_lang.include_paths, paths.gen_dir)) {
            bake_strlist_append(&cpp_lang.include_paths, paths.gen_dir);
        }
    }

    bake_strlist_t mode_cflags, mode_cxxflags, mode_ldflags;
    bake_mode_lists(
        request->mode,
        ctx->compiler_kind,
        ctx->opts.strict,
        &mode_cflags,
        &mode_cxxflags,
        &mode_ldflags);

#if !defined(_WIN32)
    if (cfg->kind == BAKE_PROJECT_TEST) {
        bake_strlist_append(&mode_cflags, "-pthread");
        bake_strlist_append(&mode_cxxflags, "-pthread");
        bake_strlist_append(&mode_ldflags, "-pthread");
    }
#endif

    bake_compile_list_t units;
    bake_compile_list_init(&units);
    bool include_deps = true;
    if (cfg->kind == BAKE_PROJECT_APPLICATION || cfg->kind == BAKE_PROJECT_TEST) {
        include_deps = request->standalone;
    }

    if (bake_collect_compile_units(
        cfg,
        &paths,
        cfg->kind == BAKE_PROJECT_TEST,
        include_deps,
        &units) != 0)
    {
        ecs_err("failed to collect source files for %s", cfg->id);
        bake_compile_list_fini(&units);
        bake_mode_lists_fini(&mode_cflags, &mode_cxxflags, &mode_ldflags);
        bake_lang_cfg_fini(&c_lang);
        bake_lang_cfg_fini(&cpp_lang);
        ecs_os_free(builtin_test_src);
        bake_build_paths_fini(&paths);
        return -1;
    }

    if (builtin_test_src) {
#if defined(_WIN32)
        const char *obj_ext = ".obj";
#else
        const char *obj_ext = ".o";
#endif
        char *obj_name = bake_asprintf("generated_bake_test%s", obj_ext);
        char *obj_path = obj_name ? bake_join_path(paths.obj_dir, obj_name) : NULL;
        if (!obj_name || !obj_path || bake_compile_list_append(&units, builtin_test_src, obj_path, NULL, false) != 0) {
            ecs_os_free(obj_name);
            ecs_os_free(obj_path);
            ecs_err("failed to add generated test API source for %s", cfg->id);
            bake_compile_list_fini(&units);
            bake_mode_lists_fini(&mode_cflags, &mode_cxxflags, &mode_ldflags);
            bake_lang_cfg_fini(&c_lang);
            bake_lang_cfg_fini(&cpp_lang);
            ecs_os_free(builtin_test_src);
            bake_build_paths_fini(&paths);
            return -1;
        }
        ecs_os_free(obj_name);
        ecs_os_free(obj_path);
    }

    const bake_lang_cfg_t *base_lang = &c_lang;
    const bake_lang_cfg_t *base_cpp = &cpp_lang;

    if (!strcmp(cfg->language, "cpp")) {
        base_lang = &cpp_lang;
        base_cpp = &cpp_lang;
    }

    int32_t compiled_count = 0;
    if (bake_compile_units_parallel(
        ctx, cfg, &paths, &units, base_lang, base_cpp,
        &mode_cflags, &mode_cxxflags, &compiled_count) != 0)
    {
        ecs_err("compilation failed for %s", cfg->id);
        bake_compile_list_fini(&units);
        bake_mode_lists_fini(&mode_cflags, &mode_cxxflags, &mode_ldflags);
        bake_lang_cfg_fini(&c_lang);
        bake_lang_cfg_fini(&cpp_lang);
        ecs_os_free(builtin_test_src);
        bake_build_paths_fini(&paths);
        return -1;
    }

    char *artefact = NULL;
    bool linked = false;
    if (bake_link_project_binary(
        ctx, project_entity, cfg, &paths, &units, base_cpp, &mode_ldflags,
        &artefact, &linked) != 0)
    {
        ecs_err("link failed for %s", cfg->id);
        bake_compile_list_fini(&units);
        bake_mode_lists_fini(&mode_cflags, &mode_cxxflags, &mode_ldflags);
        bake_lang_cfg_fini(&c_lang);
        bake_lang_cfg_fini(&cpp_lang);
        ecs_os_free(builtin_test_src);
        bake_build_paths_fini(&paths);
        return -1;
    }

    const BakeBuildResult *prev_result = ecs_get(ctx->world, project_entity, BakeBuildResult);
    if (prev_result && prev_result->artefact) {
        ecs_os_free((char*)prev_result->artefact);
    }

    BakeBuildResult result = {
        .status = 0,
        .artefact = artefact
    };
    ecs_set_ptr(ctx->world, project_entity, BakeBuildResult, &result);
    ecs_add(ctx->world, project_entity, BakeBuilt);

    bool rebuilt = compiled_count > 0 || linked;
    if (bake_environment_sync_project(ctx, project_entity, &result, request, rebuilt) != 0) {
        bake_compile_list_fini(&units);
        bake_mode_lists_fini(&mode_cflags, &mode_cxxflags, &mode_ldflags);
        bake_lang_cfg_fini(&c_lang);
        bake_lang_cfg_fini(&cpp_lang);
        ecs_os_free(builtin_test_src);
        bake_build_paths_fini(&paths);
        return -1;
    }

    bake_compile_list_fini(&units);
    bake_mode_lists_fini(&mode_cflags, &mode_cxxflags, &mode_ldflags);
    bake_lang_cfg_fini(&c_lang);
    bake_lang_cfg_fini(&cpp_lang);
    ecs_os_free(builtin_test_src);
    bake_build_paths_fini(&paths);

    return 0;
}

static int bake_execute_build_graph(bake_context_t *ctx, const char *target, bool recursive, bool standalone) {
    bake_model_mark_build_targets(ctx->world, target, ctx->opts.mode, recursive, standalone);

    ecs_entity_t *order = NULL;
    int32_t count = 0;
    if (bake_model_build_order(ctx->world, &order, &count) != 0) {
        return -1;
    }

    if (target && target[0] && count == 0) {
        ecs_err("target not found: %s", target);
        ecs_os_free(order);
        return -1;
    }

    for (int32_t i = 0; i < count; i++) {
        const BakeBuildRequest *req = ecs_get(ctx->world, order[i], BakeBuildRequest);
        if (!req) {
            continue;
        }

        const BakeProject *project = ecs_get(ctx->world, order[i], BakeProject);
        if (project && project->cfg && !project->external) {
            bake_log_build_header(ctx, project->cfg);
        }

        if (bake_build_one(ctx, order[i], req) != 0) {
            ecs_add(ctx->world, order[i], BakeBuildFailed);
            ecs_os_free(order);
            return -1;
        }
    }

    ecs_os_free(order);
    return 0;
}

static int bake_prepare_discovery(bake_context_t *ctx) {
    char *target_path = bake_resolve_target_path(ctx, ctx->opts.target);
    char *target_root = NULL;
    bool skip_special_dirs = true;
    if (target_path) {
        if (bake_is_dir(target_path)) {
            target_root = bake_strdup(target_path);
        } else {
            target_root = bake_dirname(target_path);
        }
        skip_special_dirs = !bake_path_has_special_dir(target_path);
    } else if (ctx->opts.target && ctx->opts.target[0]) {
        /* Target by id: allow resolving projects from test/example folders */
        skip_special_dirs = false;
    }

    const char *discovery_root = target_root ? target_root : ctx->opts.cwd;

    int discovered = bake_discover_projects(ctx, discovery_root, skip_special_dirs);
    if (discovered < 0) {
        ecs_os_free(target_path);
        ecs_os_free(target_root);
        return -1;
    }
    ecs_os_free(target_path);
    ecs_os_free(target_root);

    ctx->compiler_kind = bake_detect_compiler_kind(ctx->opts.cc, ctx->opts.cxx);
    return 0;
}

static int bake_clean_project(const bake_project_cfg_t *cfg, bool recursive) {
    BAKE_UNUSED(recursive);
    char *bake_dir = bake_join_path(cfg->path, ".bake");
    if (!bake_dir) {
        ecs_os_free(bake_dir);
        return -1;
    }

    int rc = 0;
    if (bake_path_exists(bake_dir) && bake_is_dir(bake_dir)) {
        rc = bake_remove_tree(bake_dir);
    }

    ecs_os_free(bake_dir);
    return rc;
}

int bake_build_clean(bake_context_t *ctx) {
    bake_proc_clear_interrupt();

    if (bake_prepare_discovery(ctx) != 0) {
        return -1;
    }

    char *target_path = bake_resolve_target_path(ctx, ctx->opts.target);
    const char *target = target_path ? target_path : ctx->opts.target;
    bake_model_mark_build_targets(ctx->world, target, ctx->opts.mode, ctx->opts.recursive, ctx->opts.standalone);

    ecs_entity_t *order = NULL;
    int32_t count = 0;
    if (bake_model_build_order(ctx->world, &order, &count) != 0) {
        ecs_os_free(target_path);
        return -1;
    }

    if (ctx->opts.target && ctx->opts.target[0] && count == 0) {
        ecs_err("target not found: %s", ctx->opts.target);
        ecs_os_free(order);
        ecs_os_free(target_path);
        return -1;
    }

    for (int32_t i = 0; i < count; i++) {
        const BakeProject *project = ecs_get(ctx->world, order[i], BakeProject);
        if (!project || !project->cfg || !project->cfg->path || project->external) {
            continue;
        }

        ecs_trace("#[green][#[normal]  clean#[green]]#[normal] %s", project->cfg->id);
        if (bake_clean_project(project->cfg, ctx->opts.recursive) != 0) {
            ecs_os_free(order);
            ecs_os_free(target_path);
            return -1;
        }
    }

    ecs_os_free(order);
    ecs_os_free(target_path);
    return 0;
}

int bake_build_run(bake_context_t *ctx) {
    bake_proc_clear_interrupt();

    if (bake_prepare_discovery(ctx) != 0) {
        return -1;
    }

    char *target_path = bake_resolve_target_path(ctx, ctx->opts.target);
    const char *target_resolved = target_path ? target_path : ctx->opts.target;

    if (bake_execute_build_graph(ctx, target_resolved, true, ctx->opts.standalone) != 0) {
        ecs_os_free(target_path);
        return -1;
    }

    if (!strcmp(ctx->opts.command, "build")) {
        ecs_os_free(target_path);
        return 0;
    }

    const char *target = ctx->opts.target;
    if (!target) {
        ecs_os_free(target_path);
        return 0;
    }

    ecs_entity_t project_entity = 0;
    const BakeProject *project = bake_model_find_project(ctx->world, target, &project_entity);
    if ((!project || !project_entity) && target_path) {
        project = bake_model_find_project_by_path(ctx->world, target_path, &project_entity);
    }

    if (!project || !project_entity) {
        ecs_err("target not found: %s", target);
        ecs_os_free(target_path);
        return -1;
    }

    const BakeBuildResult *result = ecs_get(ctx->world, project_entity, BakeBuildResult);
    if (!result || !result->artefact) {
        ecs_os_free(target_path);
        return -1;
    }

    if (!strcmp(ctx->opts.command, "test") || project->cfg->kind == BAKE_PROJECT_TEST) {
        int rc = bake_test_run_project(ctx, project->cfg, result->artefact);
        ecs_os_free(target_path);
        return rc;
    }

    if (!strcmp(ctx->opts.command, "run")) {
        ecs_strbuf_t cmd = ECS_STRBUF_INIT;
        if (ctx->opts.run_prefix) {
            ecs_strbuf_append(&cmd, "%s ", ctx->opts.run_prefix);
        }
        ecs_strbuf_append(&cmd, "\"%s\"", result->artefact);
        for (int i = 0; i < ctx->opts.run_argc; i++) {
            ecs_strbuf_append(&cmd, " \"%s\"", ctx->opts.run_argv[i]);
        }

        char *cmd_str = ecs_strbuf_get(&cmd);
        int rc = bake_run_command(cmd_str);
        ecs_os_free(cmd_str);
        ecs_os_free(target_path);
        return rc;
    }

    ecs_os_free(target_path);
    return 0;
}

int bake_build_rebuild(bake_context_t *ctx) {
    if (bake_build_clean(ctx) != 0) {
        return -1;
    }
    return bake_build_run(ctx);
}
