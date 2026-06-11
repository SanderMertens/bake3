#include "build_internal.h"
#include "bake/environment.h"
#include "bake/test_harness.h"
#include "bake/os.h"

#include <limits.h>
#include <stdlib.h>

ECS_COMPONENT_DECLARE(BakeBuildRequest);
ECS_COMPONENT_DECLARE(BakeBuildResult);

char* bake_project_build_root(const char *project_path, const char *project_id, const char *mode) {
    if (!project_path || !project_path[0]) {
        return NULL;
    }

    char *triplet = bake_host_triplet(mode);
    const char *bake_home = bake_env_home();
    if (bake_env_is_local() &&
        bake_home &&
        bake_home[0] &&
        project_id &&
        project_id[0])
    {
        char *build_dir = bake_path_join(bake_home, "build");
        char *project_dir = bake_path_join(build_dir, project_id);
        ecs_os_free(build_dir);

        char *root = bake_path_join(project_dir, triplet);
        ecs_os_free(project_dir);
        ecs_os_free(triplet);
        return root;
    }

    char *bake_dir = bake_path_join(project_path, ".bake");
    char *root = bake_path_join(bake_dir, triplet);
    ecs_os_free(triplet);
    ecs_os_free(bake_dir);
    return root;
}

static const char *bake_standalone_deps_marker = ".bake_standalone_deps";

char* bake_display_path(const char *full_path, const char *strip_prefix) {
    if (!full_path) {
        return ecs_os_strdup(".");
    }

    const char *display = full_path;
    size_t prefix_len = 0;
    if (strip_prefix &&
        bake_path_has_prefix_normalized(full_path, strip_prefix, &prefix_len))
    {
        display = full_path + prefix_len;
        while (*display == '/' || *display == '\\') {
            display++;
        }
        if (!display[0]) {
            return ecs_os_strdup(".");
        }
    }

    char *out = ecs_os_strdup(display);

    for (char *ch = out; *ch; ch++) {
        if (*ch == '\\') {
            *ch = '/';
        }
    }

    return out;
}

static void bake_log_build_header(const bake_context_t *ctx, const bake_project_cfg_t *cfg) {
    const char *command = ctx->opts.command ? ctx->opts.command : "build";
    const char *kind = bake_project_kind_str(cfg->kind);
    const char *id = cfg->id ? cfg->id : "<unnamed>";
    char *path = bake_display_path(cfg->path, ctx->opts.cwd);
    ecs_trace("#[green][#[normal]%s#[green]] %s#[normal] %s => '%s'", command, kind, id, path);
    ecs_os_free(path);
}

static char* bake_resolve_target_path(const bake_context_t *ctx, const char *target) {
    if (!ctx || !target || !target[0]) {
        return NULL;
    }

    if (!bake_path_exists(target)) {
        return NULL;
    }

    if (bake_path_is_abs(target)) {
        return bake_path_resolve(target);
    }

    char *joined = bake_path_join(ctx->opts.cwd, target);
    char *normalized = bake_path_resolve(joined);
    ecs_os_free(joined);
    return normalized;
}

static const char* bake_effective_build_target(const bake_context_t *ctx) {
    if (ctx && ctx->opts.target && ctx->opts.target[0]) {
        return ctx->opts.target;
    }
    return ".";
}

static int bake_write_standalone_dep_header(
    const bake_project_cfg_t *cfg,
    const char *deps_dir)
{
    if (!cfg || !cfg->id) {
        return -1;
    }

    char *header_base = bake_project_id_as_macro(cfg->id);
    char *header_name = flecs_asprintf("%s.h", header_base);
    char *header_path = bake_path_join(deps_dir, header_name);
    char *header_content = flecs_asprintf("#pragma once\n#include <%s>\n", header_name);

    int rc = bake_file_write(header_path, header_content);

    ecs_os_free(header_base);
    ecs_os_free(header_name);
    ecs_os_free(header_path);
    ecs_os_free(header_content);
    return rc;
}

static void bake_track_standalone_dep_outputs(
    const bake_project_cfg_t *cfg,
    bool emit_sources,
    bake_strlist_t *expected_outputs)
{
    char *base = bake_project_id_as_macro(cfg->id);
    char *header_name = flecs_asprintf("%s.h", base);
    char *source_name = emit_sources ? flecs_asprintf("%s.c", base) : NULL;

    bake_strlist_append_unique(expected_outputs, header_name);
    if (emit_sources) {
        bake_strlist_append_unique(expected_outputs, source_name);
    }

    ecs_os_free(base);
    ecs_os_free(header_name);
    ecs_os_free(source_name);
}

static int bake_cleanup_standalone_outputs(
    const char *deps_dir,
    const bake_strlist_t *expected_outputs)
{
    bake_dir_entry_t *entries = NULL;
    int32_t entry_count = 0;
    if (bake_dir_list(deps_dir, &entries, &entry_count) != 0) {
        return -1;
    }

    int rc = 0;
    for (int32_t i = 0; i < entry_count; i++) {
        if (entries[i].is_dir) {
            continue;
        }

        if (!bake_strlist_contains(expected_outputs, entries[i].name)) {
            if (bake_remove_file(entries[i].path) != 0) {
                rc = -1;
                break;
            }
        }
    }

    bake_dir_entries_free(entries, entry_count);
    return rc;
}

static int bake_prepare_standalone_sources(
    bake_context_t *ctx,
    ecs_entity_t project_entity,
    const bake_project_cfg_t *cfg,
    bool emit_sources)
{
    int rc = -1;
    char *deps_dir = bake_path_join(cfg->path, "deps");
    bake_strlist_t expected_outputs = {0};
    bake_strlist_init(&expected_outputs);

    if (bake_os_mkdirs(deps_dir) != 0) {
        ecs_os_free(deps_dir);
        bake_strlist_fini(&expected_outputs);
        return -1;
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

        bake_track_standalone_dep_outputs(dep_project->cfg, emit_sources, &expected_outputs);

        if (!emit_sources) {
            if (bake_write_standalone_dep_header(dep_project->cfg, deps_dir) != 0) {
                goto cleanup;
            }
            continue;
        }

        if (bake_amalgamate_project(dep_project->cfg, deps_dir) != 0) {
            goto cleanup;
        }
    }

    bake_strlist_append_unique(&expected_outputs, bake_standalone_deps_marker);

    char *marker_path = bake_path_join(deps_dir, bake_standalone_deps_marker);
    if (bake_file_write(marker_path, "generated by bake\n") != 0) {
        ecs_os_free(marker_path);
        goto cleanup;
    }
    ecs_os_free(marker_path);

    if (bake_cleanup_standalone_outputs(deps_dir, &expected_outputs) != 0) {
        goto cleanup;
    }

    rc = 0;
cleanup:
    ecs_os_free(deps_dir);
    bake_strlist_fini(&expected_outputs);
    return rc;
}

static void bake_fingerprint_append_list(
    ecs_strbuf_t *buf,
    const char *key,
    const bake_strlist_t *list)
{
    ecs_strbuf_append(buf, "%s=", key);
    for (int32_t i = 0; i < list->count; i++) {
        ecs_strbuf_append(buf, "%s;", list->items[i]);
    }
    ecs_strbuf_appendch(buf, '\n');
}

static void bake_fingerprint_append_lang(
    ecs_strbuf_t *buf,
    const char *prefix,
    const bake_lang_cfg_t *lang)
{
    ecs_strbuf_append(buf, "%s.std=%s/%s\n", prefix,
        lang->c_standard ? lang->c_standard : "",
        lang->cpp_standard ? lang->cpp_standard : "");
    ecs_strbuf_append(buf, "%s.bools=%d%d%d\n", prefix,
        lang->static_lib ? 1 : 0,
        lang->export_symbols ? 1 : 0,
        lang->precompile_header ? 1 : 0);

#define L(f) bake_fingerprint_append_list(buf, prefix, &lang->f)
    L(cflags); L(cxxflags); L(defines); L(ldflags); L(libs);
    L(static_libs); L(libpaths); L(links); L(include_paths); L(embed);
#undef L
}

/* Captures the configuration that shapes compile and link commands. When it
 * differs from the stored value, all objects are stale even if their mtimes
 * are not: flag changes and bake upgrades do not touch source files.
 * Dependency-derived paths are deliberately not included: they vary between
 * a cold and a warm build (install dirs appear after the first pass), and
 * dependency changes are already tracked through project.json and artefact
 * timestamps. */
static char* bake_compose_build_fingerprint(
    const bake_context_t *ctx,
    const BakeBuildRequest *request,
    const bake_lang_cfg_t *c_lang,
    const bake_lang_cfg_t *cpp_lang,
    const bake_strlist_t *mode_cflags,
    const bake_strlist_t *mode_cxxflags,
    const bake_strlist_t *mode_ldflags)
{
    ecs_strbuf_t buf = ECS_STRBUF_INIT;

    char *exe = bake_os_executable_path();
    ecs_strbuf_append(&buf, "bake=%lld\n",
        exe ? (long long)bake_os_file_mtime(exe) : 0);
    ecs_os_free(exe);

    ecs_strbuf_append(&buf, "cc=%s\ncxx=%s\nkind=%d\nmode=%s\nstrict=%d\ntarget=%s-%s\n",
        ctx->opts.cc ? ctx->opts.cc : "",
        ctx->opts.cxx ? ctx->opts.cxx : "",
        (int)ctx->compiler_kind,
        bake_effective_mode(request->mode),
        ctx->opts.strict ? 1 : 0,
        bake_target_arch(),
        bake_target_os());

    bake_fingerprint_append_lang(&buf, "c", c_lang);
    bake_fingerprint_append_lang(&buf, "cpp", cpp_lang);
    bake_fingerprint_append_list(&buf, "mode_cflags", mode_cflags);
    bake_fingerprint_append_list(&buf, "mode_cxxflags", mode_cxxflags);
    bake_fingerprint_append_list(&buf, "mode_ldflags", mode_ldflags);

    return ecs_strbuf_get(&buf);
}

static int bake_build_one(bake_context_t *ctx, ecs_entity_t project_entity, const BakeBuildRequest *request) {
    const BakeProject *project = ecs_get(ctx->world, project_entity, BakeProject);
    if (!project || !project->cfg) {
        const char *name = ecs_get_name(ctx->world, project_entity);
        ecs_err("cannot build '%s': not a known project", name ? name : "<unnamed>");
        return -1;
    }

    const bake_project_cfg_t *cfg = project->cfg;
    char *builtin_test_src = NULL;
    char *test_exe_path = NULL;
    if (project->external) {
        return 0;
    }

    if (cfg->kind == BAKE_PROJECT_CONFIG || cfg->kind == BAKE_PROJECT_TEMPLATE) {
        const BakeBuildResult *prev_result = ecs_get(ctx->world, project_entity, BakeBuildResult);
        if (prev_result && prev_result->artefact) {
            ecs_os_free((char*)prev_result->artefact);
        }

        BakeBuildResult result = { .status = 0, .artefact = NULL };
        ecs_set_ptr(ctx->world, project_entity, BakeBuildResult, &result);
        if (bake_env_sync_project(ctx, project_entity, &result, request, false) != 0) {
            return -1;
        }
        return 0;
    }

    int rc = -1;
    char *fingerprint = NULL;
    char *fingerprint_path = NULL;
    bake_build_paths_t paths = {0};
    bake_lang_cfg_t c_lang = {0};
    bake_lang_cfg_t cpp_lang = {0};
    bake_strlist_t mode_cflags = {0};
    bake_strlist_t mode_cxxflags = {0};
    bake_strlist_t mode_ldflags = {0};
    bake_compile_list_t units = {0};

    if (bake_build_paths_init(cfg, request->mode, &paths) != 0) {
        ecs_err("failed to initialize build paths for %s (path=%s)", cfg->id, cfg->path ? cfg->path : "<null>");
        goto cleanup;
    }

    if (cfg->kind == BAKE_PROJECT_TEST) {
        char *artefact_name = bake_project_cfg_artefact_name(cfg);
        test_exe_path = artefact_name ? bake_path_join(paths.bin_dir, artefact_name) : NULL;
        ecs_os_free(artefact_name);
        if (!test_exe_path) {
            ecs_err("failed to resolve test executable path for %s", cfg->id);
            goto cleanup;
        }

        if (bake_test_generate_harness(ctx, cfg, test_exe_path) != 0) {
            ecs_err("test harness generation failed for %s", cfg->id);
            goto cleanup;
        }

        if (cfg->has_test_spec) {
            if (bake_test_generate_builtin_api(ctx, cfg, paths.gen_dir, &builtin_test_src) != 0) {
                ecs_err("failed to generate test API for %s", cfg->id);
                goto cleanup;
            }
        }
    }

    if (ecs_vec_count(&cfg->rules.vec) &&
        bake_execute_rules(ctx->world, project_entity, cfg, &paths) != 0)
    {
        ecs_err("rule execution failed for %s", cfg->id);
        goto cleanup;
    }

    if (bake_amalgamate_list_count(&cfg->amalgamate) > 0) {
        if (bake_generate_project_amalgamation(cfg) != 0) {
            ecs_err("amalgamation failed for %s", cfg->id);
            goto cleanup;
        }
    }

    if (bake_generate_config_header(ctx->world, cfg) != 0) {
        ecs_err("bake_config.h generation failed for %s", cfg->id);
        goto cleanup;
    }

    if ((request->standalone || cfg->standalone) && (cfg->kind == BAKE_PROJECT_APPLICATION || cfg->kind == BAKE_PROJECT_TEST)) {
        if (bake_prepare_standalone_sources(
            ctx, project_entity, cfg, request->standalone) != 0)
        {
            ecs_err("standalone amalgamation failed for %s", cfg->id);
            goto cleanup;
        }
    }

    bake_lang_cfg_copy(&c_lang, &cfg->c_lang);
    bake_lang_cfg_copy(&cpp_lang, &cfg->cpp_lang);

    bake_apply_dependee_cfg(ctx->world, project_entity, &c_lang, false);
    bake_apply_dependee_cfg(ctx->world, project_entity, &cpp_lang, true);

    /* Link uses a single language config: fold the C++ link inputs into the C
     * config so that link flags declared under either lang.c or lang.cpp are
     * applied to the project's own binary. */
    bake_strlist_merge_unique(&c_lang.ldflags, &cpp_lang.ldflags);
    bake_strlist_merge_unique(&c_lang.libs, &cpp_lang.libs);
    bake_strlist_merge_unique(&c_lang.libpaths, &cpp_lang.libpaths);
    bake_strlist_merge_unique(&c_lang.embed, &cpp_lang.embed);

    if (cfg->kind == BAKE_PROJECT_TEST) {
        bake_strlist_append_unique(&c_lang.include_paths, paths.gen_dir);
        bake_strlist_append_unique(&cpp_lang.include_paths, paths.gen_dir);
    }

    bake_strlist_init(&mode_cflags);
    bake_strlist_init(&mode_cxxflags);
    bake_strlist_init(&mode_ldflags);
    bake_add_mode_flags(request->mode, ctx->compiler_kind, &mode_cflags, &mode_cxxflags, &mode_ldflags);
    bake_add_strict_flags(ctx->opts.strict, ctx->compiler_kind, &mode_cflags, &mode_cxxflags, &mode_ldflags);

    if (cfg->kind == BAKE_PROJECT_TEST &&
        ctx->compiler_kind != BAKE_COMPILER_MSVC &&
        !bake_target_is_emscripten())
    {
        bake_strlist_append(&mode_cflags, "-pthread");
        bake_strlist_append(&mode_cxxflags, "-pthread");
        bake_strlist_append(&mode_ldflags, "-pthread");
    }

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
        ctx->compiler_kind,
        &units) != 0)
    {
        ecs_err("failed to collect source files for %s", cfg->id);
        goto cleanup;
    }

    if (builtin_test_src) {
#if defined(_WIN32)
        const char *obj_ext = ".obj";
#else
        const char *obj_ext = ".o";
#endif
        char *obj_name = flecs_asprintf("generated_bake_test%s", obj_ext);
        char *obj_path = bake_path_join(paths.obj_dir, obj_name);
        if (bake_os_mkdirs(paths.obj_dir) != 0) {
            ecs_os_free(obj_name);
            ecs_os_free(obj_path);
            ecs_err("failed to add generated test API source for %s", cfg->id);
            goto cleanup;
        }
        bake_compile_list_append(&units, builtin_test_src, obj_path, NULL, false);
        ecs_os_free(obj_name);
        ecs_os_free(obj_path);
    }

    fingerprint = bake_compose_build_fingerprint(
        ctx, request, &c_lang, &cpp_lang,
        &mode_cflags, &mode_cxxflags, &mode_ldflags);
    fingerprint_path = bake_path_join(paths.build_root, ".bake_cmd");
    char *prev_fingerprint = bake_file_read(fingerprint_path, NULL);
    bool flags_changed = !prev_fingerprint ||
        strcmp(prev_fingerprint, fingerprint) != 0;
    ecs_os_free(prev_fingerprint);

    int32_t compiled_count = 0;
    if (bake_compile_units_parallel(
        ctx, project_entity, cfg, &units, &c_lang, &cpp_lang,
        &mode_cflags, &mode_cxxflags, flags_changed, &compiled_count) != 0)
    {
        ecs_err("compilation failed for %s", cfg->id);
        goto cleanup;
    }

    char *artefact = NULL;
    bool linked = false;
    if (bake_link_project_binary(
        ctx, project_entity, cfg, &paths, &units, &c_lang, &mode_ldflags,
        flags_changed, &artefact, &linked) != 0)
    {
        ecs_err("link failed for %s", cfg->id);
        goto cleanup;
    }

    if (flags_changed && bake_file_write(fingerprint_path, fingerprint) != 0) {
        ecs_os_free(artefact);
        goto cleanup;
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

    bool rebuilt = compiled_count > 0 || linked;
    if (bake_env_sync_project(ctx, project_entity, &result, request, rebuilt) != 0) {
        goto cleanup;
    }

    rc = 0;

cleanup:
    ecs_os_free(fingerprint);
    ecs_os_free(fingerprint_path);
    bake_compile_list_fini(&units);
    bake_strlist_fini(&mode_cflags);
    bake_strlist_fini(&mode_cxxflags);
    bake_strlist_fini(&mode_ldflags);
    bake_lang_cfg_fini(&c_lang);
    bake_lang_cfg_fini(&cpp_lang);
    ecs_os_free(test_exe_path);
    ecs_os_free(builtin_test_src);
    bake_build_paths_fini(&paths);
    return rc;
}

static bool bake_is_unresolved_external_dependency(const BakeProject *project) {
    if (!project || !project->external) {
        return false;
    }
    return bake_project_is_placeholder(project);
}

static int bake_validate_build_graph_dependencies(const ecs_world_t *world, ecs_entity_t *order, int32_t count) {
    int32_t unresolved_count = 0;

    for (int32_t i = 0; i < count; i++) {
        const BakeProject *project = ecs_get(world, order[i], BakeProject);
        if (!bake_is_unresolved_external_dependency(project)) {
            continue;
        }

        ecs_err("unresolved dependency: %s", project->cfg->id);
        unresolved_count++;
    }

    if (unresolved_count) {
        return -1;
    }

    return 0;
}

static int bake_execute_build_graph(bake_context_t *ctx, const char *target, bool recursive, bool standalone) {
    bake_model_mark_build_targets(ctx->world, target, ctx->opts.mode, recursive, standalone);

    int rc = -1;
    ecs_entity_t *order = NULL;
    int32_t count = 0;
    if (bake_model_build_order(ctx->world, &order, &count) != 0) goto cleanup;

    if (target && target[0] && count == 0) {
        ecs_err("target not found: %s", target);
        goto cleanup;
    }

    if (bake_validate_build_graph_dependencies(ctx->world, order, count) != 0) goto cleanup;

    for (int32_t i = 0; i < count; i++) {
        const BakeBuildRequest *req = ecs_get(ctx->world, order[i], BakeBuildRequest);
        if (!req) continue;

        const BakeProject *project = ecs_get(ctx->world, order[i], BakeProject);
        if (project && project->cfg && !project->external) {
            bake_log_build_header(ctx, project->cfg);
        }

        if (bake_build_one(ctx, order[i], req) != 0) {
            goto cleanup;
        }
    }
    rc = 0;

cleanup:
    ecs_os_free(order);
    return rc;
}

/* Discovers projects for the effective target and returns the resolved
 * target path (NULL when the target is not an existing path) through
 * target_path_out so commands do not have to resolve it again. */
static int bake_prepare_discovery(bake_context_t *ctx, char **target_path_out) {
    int rc = -1;
    const char *target = bake_effective_build_target(ctx);
    char *target_path = bake_resolve_target_path(ctx, target);
    char *target_root = NULL;
    if (target_path) {
        target_root = bake_path_is_dir(target_path)
            ? ecs_os_strdup(target_path)
            : bake_path_dirname(target_path);
    }

    if (target_root && bake_discover_projects(ctx, target_root, true) < 0) {
        goto cleanup;
    }

    bool discover_cwd = !(target_root && bake_path_equal_normalized(target_root, ctx->opts.cwd));
    if (discover_cwd && bake_discover_projects(ctx, ctx->opts.cwd, true) < 0) {
        goto cleanup;
    }

    ctx->compiler_kind = bake_detect_compiler_kind(ctx->opts.cc, ctx->opts.cxx);
    rc = 0;

cleanup:
    if (rc == 0 && target_path_out) {
        *target_path_out = target_path;
        target_path = NULL;
    }
    ecs_os_free(target_path);
    ecs_os_free(target_root);
    return rc;
}

static int bake_clean_project(const bake_context_t *ctx, const bake_project_cfg_t *cfg) {
    char *bake_dir = NULL;
    if (ctx && ctx->opts.local_env && ctx->bake_home && cfg->id && cfg->id[0]) {
        char *build_root = bake_path_join(ctx->bake_home, "build");
        bake_dir = bake_path_join(build_root, cfg->id);
        ecs_os_free(build_root);
    } else {
        bake_dir = bake_path_join(cfg->path, ".bake");
    }

    int rc = 0;
    if (bake_path_exists(bake_dir) && bake_path_is_dir(bake_dir)) {
        bake_dir_entry_t *entries = NULL;
        int32_t entry_count = 0;
        if (bake_dir_list(bake_dir, &entries, &entry_count) != 0) {
            ecs_os_free(bake_dir);
            return -1;
        }

        bool kept_bundles = false;
        for (int32_t i = 0; i < entry_count && rc == 0; i++) {
            const bake_dir_entry_t *entry = &entries[i];
            if (bake_is_dot_dir(entry->name)) {
                continue;
            }
            /* Preserve fetched/built bundle artefacts so a clean does not
             * trigger expensive re-clones and re-builds. */
            if (!strcmp(entry->name, "bundles")) {
                kept_bundles = true;
                continue;
            }
            if (entry->is_dir) {
                rc = bake_os_rmtree(entry->path);
            } else {
                rc = bake_remove_file_if_exists(entry->path);
            }
        }
        bake_dir_entries_free(entries, entry_count);

        if (rc == 0 && !kept_bundles) {
            /* Match the historical clean behaviour: leave nothing behind when
             * the project has no bundle artefacts to preserve. */
            bake_os_rmdir(bake_dir);
        }
    }

    ecs_os_free(bake_dir);

    if (rc == 0 &&
        (cfg->kind == BAKE_PROJECT_APPLICATION || cfg->kind == BAKE_PROJECT_TEST) &&
        cfg->standalone)
    {
        char *deps_dir = bake_path_join(cfg->path, "deps");
        char *marker = bake_path_join(deps_dir, bake_standalone_deps_marker);

        if (bake_path_exists(marker) && bake_path_exists(deps_dir) && bake_path_is_dir(deps_dir)) {
            rc = bake_os_rmtree(deps_dir);
        }

        ecs_os_free(marker);
        ecs_os_free(deps_dir);
    }

    return rc;
}

static int bake_build_clean_prepared(bake_context_t *ctx, const char *target) {
    int rc = -1;
    bake_model_mark_build_targets(ctx->world, target, ctx->opts.mode, ctx->opts.recursive, ctx->opts.standalone);

    ecs_entity_t *order = NULL;
    int32_t count = 0;
    if (bake_model_build_order(ctx->world, &order, &count) != 0) {
        goto cleanup;
    }

    if (count == 0) {
        rc = 0;
        goto cleanup;
    }

    for (int32_t i = 0; i < count; i++) {
        const BakeProject *project = ecs_get(ctx->world, order[i], BakeProject);
        if (!project || !project->cfg || !project->cfg->path || project->external) {
            continue;
        }

        ecs_trace("#[green][#[normal]  clean#[green]]#[normal] %s", project->cfg->id);
        if (bake_clean_project(ctx, project->cfg) != 0) {
            goto cleanup;
        }
    }

    rc = 0;
cleanup:
    ecs_os_free(order);
    return rc;
}

int bake_build_clean(bake_context_t *ctx) {
    char *target_path = NULL;
    if (bake_prepare_discovery(ctx, &target_path) != 0) {
        return -1;
    }

    const char *target = target_path
        ? target_path
        : bake_effective_build_target(ctx);
    int rc = bake_build_clean_prepared(ctx, target);
    ecs_os_free(target_path);
    return rc;
}

int bake_build(bake_context_t *ctx) {
    char *target_path = NULL;
    if (bake_prepare_discovery(ctx, &target_path) != 0) {
        return -1;
    }

    const char *target_resolved = target_path
        ? target_path
        : bake_effective_build_target(ctx);

    int rc = 0;
    if (bake_execute_build_graph(ctx, target_resolved, true, ctx->opts.standalone) != 0) {
        rc = -1;
    }

    ecs_os_free(target_path);
    return rc;
}

int bake_build_rebuild(bake_context_t *ctx) {
    char *target_path = NULL;
    if (bake_prepare_discovery(ctx, &target_path) != 0) {
        return -1;
    }

    const char *target = target_path
        ? target_path
        : bake_effective_build_target(ctx);

    int rc = bake_build_clean_prepared(ctx, target);
    if (rc == 0 && bake_execute_build_graph(ctx, target, true, ctx->opts.standalone) != 0) {
        rc = -1;
    }

    ecs_os_free(target_path);
    return rc;
}

int bake_build_run(bake_context_t *ctx) {
    int rc = 0;

    char *target_path = NULL;
    if (bake_prepare_discovery(ctx, &target_path) != 0) {
        return -1;
    }

    const char *effective_target = bake_effective_build_target(ctx);
    const char *target_resolved = target_path ? target_path : effective_target;

    if (bake_execute_build_graph(ctx, target_resolved, true, ctx->opts.standalone) != 0) {
        rc = -1;
        goto cleanup;
    }

    const char *target = effective_target;

    ecs_entity_t project_entity = 0;
    const BakeProject *project = bake_model_find_project(ctx->world, target, &project_entity);
    if ((!project || !project_entity) && target_path) {
        project = bake_model_find_project_by_path(ctx->world, target_path, &project_entity);
    }

    if (!project || !project_entity) {
        ecs_err("target not found: %s", target);
        rc = -1;
        goto cleanup;
    }

    const BakeBuildResult *result = ecs_get(ctx->world, project_entity, BakeBuildResult);
    if (!result || !result->artefact) {
        rc = -1;
        goto cleanup;
    }

    if (!strcmp(ctx->opts.command, "test") || project->cfg->kind == BAKE_PROJECT_TEST) {
        rc = bake_test_run_project(ctx, project->cfg, result->artefact);
        goto cleanup;
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
        rc = bake_run_command(cmd_str, true);
        ecs_os_free(cmd_str);
        goto cleanup;
    }

cleanup:
    ecs_os_free(target_path);
    return rc;
}
