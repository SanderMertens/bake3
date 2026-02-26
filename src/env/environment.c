#include "bake2/environment.h"
#include "bake2/os.h"

static bool bake_env_is_dot(const char *name) {
    return !strcmp(name, ".") || !strcmp(name, "..");
}

static char* bake_env_meta_dir(const bake_context_t *ctx) {
    return bake_join_path(ctx->bake_home, "meta");
}

static char* bake_env_meta_project_json_path(const bake_context_t *ctx, const char *id) {
    if (!ctx || !id || !id[0]) {
        return NULL;
    }

    char *meta_dir = bake_join3_path(ctx->bake_home, "meta", id);
    if (!meta_dir) {
        return NULL;
    }

    char *project_json = bake_join_path(meta_dir, "project.json");
    ecs_os_free(meta_dir);
    return project_json;
}

static int bake_env_remove_if_exists(const char *path) {
    if (!path || !bake_path_exists(path)) {
        return 0;
    }
    return bake_remove_tree(path);
}

static int bake_env_copy_tree_recursive(const char *src, const char *dst) {
    bake_dir_entry_t *entries = NULL;
    int32_t count = 0;
    if (bake_dir_list(src, &entries, &count) != 0) {
        return -1;
    }

    for (int32_t i = 0; i < count; i++) {
        bake_dir_entry_t *entry = &entries[i];
        if (bake_env_is_dot(entry->name)) {
            continue;
        }

        char *dst_path = bake_join_path(dst, entry->name);
        if (!dst_path) {
            bake_dir_entries_free(entries, count);
            return -1;
        }

        int rc = 0;
        if (entry->is_dir) {
            rc = bake_mkdirs(dst_path);
            if (rc == 0) {
                rc = bake_env_copy_tree_recursive(entry->path, dst_path);
            }
        } else {
            rc = bake_copy_file(entry->path, dst_path);
        }

        ecs_os_free(dst_path);
        if (rc != 0) {
            bake_dir_entries_free(entries, count);
            return -1;
        }
    }

    bake_dir_entries_free(entries, count);
    return 0;
}

static int bake_env_copy_tree_exact(const char *src, const char *dst) {
    if (bake_env_remove_if_exists(dst) != 0) {
        return -1;
    }

    if (!src || !bake_path_exists(src) || !bake_is_dir(src)) {
        return 0;
    }

    if (bake_mkdirs(dst) != 0) {
        return -1;
    }

    return bake_env_copy_tree_recursive(src, dst);
}

static char* bake_env_read_trimmed_file(const char *path) {
    size_t len = 0;
    char *text = bake_read_file(path, &len);
    if (!text) {
        return NULL;
    }

    while (len > 0) {
        char ch = text[len - 1];
        if (ch != '\n' && ch != '\r') {
            break;
        }
        text[len - 1] = '\0';
        len--;
    }

    return text;
}

static int bake_env_write_dependee_json(const char *path, const bake_project_cfg_t *cfg) {
    const char *dependee = "{}";
    if (cfg->dependee.json && cfg->dependee.json[0]) {
        dependee = cfg->dependee.json;
    }

    char *json = bake_asprintf("%s\n", dependee);
    if (!json) {
        return -1;
    }

    int rc = bake_write_file(path, json);
    ecs_os_free(json);
    return rc;
}

static char* bake_env_artefact_path_impl(
    const bake_context_t *ctx,
    const bake_project_cfg_t *cfg,
    const char *mode,
    bool scoped)
{
    if (!cfg || (scoped && (!cfg->id || !cfg->id[0]))) {
        return NULL;
    }

    char *out_path = NULL;
    char *file_name = NULL;
    char *platform = NULL;
    char *platform_dir = NULL;
    char *cfg_dir = NULL;
    char *out_dir = NULL;
    char *id_dir = NULL;

    file_name = bake_project_cfg_artefact_name(cfg);
    if (!file_name) {
        goto cleanup;
    }

    platform = bake_host_platform();
    platform_dir = platform ? bake_join_path(ctx->bake_home, platform) : NULL;
    cfg_dir = platform_dir ? bake_join_path(platform_dir, mode && mode[0] ? mode : "debug") : NULL;
    const char *subdir = cfg->kind == BAKE_PROJECT_PACKAGE ? "lib" : "bin";
    out_dir = cfg_dir ? bake_join_path(cfg_dir, subdir) : NULL;
    id_dir = scoped ? (out_dir ? bake_join_path(out_dir, cfg->id) : NULL) : NULL;
    out_path = scoped ? (id_dir ? bake_join_path(id_dir, file_name) : NULL) :
        (out_dir ? bake_join_path(out_dir, file_name) : NULL);

cleanup:
    ecs_os_free(file_name);
    ecs_os_free(platform);
    ecs_os_free(platform_dir);
    ecs_os_free(cfg_dir);
    ecs_os_free(out_dir);
    ecs_os_free(id_dir);
    return out_path;
}

static char* bake_env_artefact_path(
    const bake_context_t *ctx,
    const bake_project_cfg_t *cfg,
    const char *mode)
{
    return bake_env_artefact_path_impl(ctx, cfg, mode, false);
}

static char* bake_env_artefact_path_scoped(
    const bake_context_t *ctx,
    const bake_project_cfg_t *cfg,
    const char *mode)
{
    return bake_env_artefact_path_impl(ctx, cfg, mode, true);
}

static char* bake_env_find_artefact_path_current_mode(
    const bake_context_t *ctx,
    const bake_project_cfg_t *cfg,
    const char *mode)
{
    char *scoped = bake_env_artefact_path_scoped(ctx, cfg, mode);
    if (scoped && bake_path_exists(scoped)) {
        return scoped;
    }
    ecs_os_free(scoped);

    char *legacy = bake_env_artefact_path(ctx, cfg, mode);
    if (legacy && bake_path_exists(legacy)) {
        return legacy;
    }
    ecs_os_free(legacy);
    return NULL;
}

static int bake_env_set_project_artefact_result(
    ecs_world_t *world,
    ecs_entity_t entity,
    char *artefact_owned)
{
    const BakeBuildResult *prev = ecs_get(world, entity, BakeBuildResult);
    if (prev && prev->artefact) {
        ecs_os_free((char*)prev->artefact);
    }

    BakeBuildResult result = {
        .status = 0,
        .artefact = artefact_owned
    };
    ecs_set_ptr(world, entity, BakeBuildResult, &result);
    return 0;
}

static int bake_env_copy_artefact_to_path(const char *src_artefact, const char *dst_path) {
    if (!src_artefact || !src_artefact[0] || !dst_path || !dst_path[0]) {
        return -1;
    }

    char *dst_dir = bake_dirname(dst_path);
    if (!dst_dir) {
        return -1;
    }

    int rc = 0;
    if (bake_mkdirs(dst_dir) != 0 || bake_copy_file(src_artefact, dst_path) != 0) {
        rc = -1;
    }

    ecs_os_free(dst_dir);
    return rc;
}

static int bake_env_copy_required_file(const char *src_dir, const char *dst_dir, const char *name) {
    int rc = -1;
    char *src = bake_join_path(src_dir, name);
    char *dst = bake_join_path(dst_dir, name);
    if (src && dst && bake_path_exists(src)) {
        rc = bake_copy_file(src, dst);
    }
    ecs_os_free(src);
    ecs_os_free(dst);
    return rc;
}

static int bake_env_copy_optional_file(const char *src_dir, const char *dst_dir, const char *name) {
    int rc = -1;
    char *src = bake_join_path(src_dir, name);
    char *dst = bake_join_path(dst_dir, name);
    if (src && dst) {
        rc = bake_path_exists(src) ? bake_copy_file(src, dst) : bake_env_remove_if_exists(dst);
    }
    ecs_os_free(src);
    ecs_os_free(dst);
    return rc;
}

static char* bake_env_templates_dir(const bake_project_cfg_t *cfg) {
    char *path = bake_join_path(cfg->path, "templates");
    if (path && bake_path_exists(path) && bake_is_dir(path)) {
        return path;
    }
    ecs_os_free(path);

    path = bake_join_path(cfg->path, "template");
    if (path && bake_path_exists(path) && bake_is_dir(path)) {
        return path;
    }
    ecs_os_free(path);
    return NULL;
}

static int bake_env_project_entry_complete(
    const bake_context_t *ctx,
    const bake_project_cfg_t *cfg,
    const char *mode)
{
    char *meta_dir = bake_join3_path(ctx->bake_home, "meta", cfg->id);
    if (!meta_dir) {
        return 0;
    }

    char *project_json = bake_join_path(meta_dir, "project.json");
    char *source_txt = bake_join_path(meta_dir, "source.txt");
    char *dependee_json = bake_join_path(meta_dir, "dependee.json");
    bool has_meta = project_json && source_txt && dependee_json &&
        bake_path_exists(project_json) &&
        bake_path_exists(source_txt) &&
        bake_path_exists(dependee_json);

    ecs_os_free(project_json);
    ecs_os_free(source_txt);
    ecs_os_free(dependee_json);
    ecs_os_free(meta_dir);

    if (!has_meta) {
        return 0;
    }

    if (cfg->kind == BAKE_PROJECT_PACKAGE ||
        cfg->kind == BAKE_PROJECT_APPLICATION ||
        cfg->kind == BAKE_PROJECT_TEST)
    {
        char *artefact = bake_env_find_artefact_path_current_mode(ctx, cfg, mode);
        bool has_artefact = artefact != NULL;
        ecs_os_free(artefact);
        return has_artefact ? 1 : 0;
    }

    return 1;
}

static int bake_env_add_dependency_ids(bake_strlist_t *queue, const bake_strlist_t *deps) {
    for (int32_t i = 0; i < deps->count; i++) {
        const char *id = deps->items[i];
        if (!id || !id[0] || bake_strlist_contains(queue, id)) {
            continue;
        }
        if (bake_strlist_append(queue, id) != 0) {
            return -1;
        }
    }
    return 0;
}

static int bake_env_queue_project_deps(bake_strlist_t *queue, const bake_project_cfg_t *cfg) {
    const bake_strlist_t *lists[] = {
        &cfg->use, &cfg->use_private, &cfg->use_build, &cfg->use_runtime
    };

    for (int32_t i = 0; i < (int32_t)(sizeof(lists) / sizeof(lists[0])); i++) {
        if (bake_env_add_dependency_ids(queue, lists[i]) != 0) {
            return -1;
        }
    }

    if (cfg->dependee.cfg) {
        const bake_strlist_t *dep_lists[] = {
            &cfg->dependee.cfg->use,
            &cfg->dependee.cfg->use_private,
            &cfg->dependee.cfg->use_build,
            &cfg->dependee.cfg->use_runtime
        };

        for (int32_t i = 0; i < (int32_t)(sizeof(dep_lists) / sizeof(dep_lists[0])); i++) {
            if (bake_env_add_dependency_ids(queue, dep_lists[i]) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

static bool bake_env_external_placeholder_project(const BakeProject *project) {
    if (!project || !project->cfg || !project->external) {
        return false;
    }

    const bake_project_cfg_t *cfg = project->cfg;
    return cfg->id && !cfg->path && !cfg->output_name;
}

static char* bake_env_resolve_home_path(const char *env_home) {
    if (!env_home || !env_home[0]) {
        return NULL;
    }

    if (bake_os_path_is_abs(env_home)) {
        return bake_strdup(env_home);
    }

    char *cwd = bake_getcwd();
    if (!cwd) {
        return NULL;
    }

    /* Use cwd-relative path as fallback, but prefer an existing ancestor match.
     * This keeps a relative BAKE_HOME stable when invoking bake from subdirs. */
    char *resolved = bake_join_path(cwd, env_home);
    if (!resolved) {
        ecs_os_free(cwd);
        return NULL;
    }

    char *probe = bake_strdup(cwd);
    ecs_os_free(cwd);
    if (!probe) {
        return resolved;
    }

    while (probe[0]) {
        char *candidate = bake_join_path(probe, env_home);
        if (!candidate) {
            break;
        }

        if (bake_path_exists(candidate)) {
            ecs_os_free(resolved);
            resolved = candidate;
            candidate = NULL;
        }
        ecs_os_free(candidate);

        char *parent = bake_dirname(probe);
        if (parent && !parent[0] && bake_os_path_is_abs(probe)) {
            ecs_os_free(parent);
            parent = bake_strdup("/");
        }

        if (!parent || !parent[0] || !strcmp(parent, ".") || !strcmp(parent, probe)) {
            ecs_os_free(parent);
            break;
        }

        ecs_os_free(probe);
        probe = parent;
    }

    ecs_os_free(probe);
    return resolved;
}

int bake_environment_init_paths(bake_context_t *ctx) {
    const char *env_home = getenv("BAKE_HOME");
    if (env_home && env_home[0]) {
        ctx->bake_home = bake_env_resolve_home_path(env_home);
    } else {
        char *home = bake_os_get_home();
        if (!home) {
            return -1;
        }
        ctx->bake_home = bake_join_path(home, "bake");
        ecs_os_free(home);
    }

    if (!ctx->bake_home) {
        return -1;
    }

    bake_os_setenv("BAKE_HOME", ctx->bake_home);

    if (bake_mkdirs(ctx->bake_home) != 0) {
        return -1;
    }

    return 0;
}

int bake_environment_import_project_by_id(bake_context_t *ctx, const char *id) {
    int rc = 0;
    char *project_json = NULL;
    char *meta_dir = NULL;
    char *source_txt = NULL;
    char *source_path = NULL;
    char *artefact = NULL;
    bake_project_cfg_t *cfg = NULL;

    if (!id || !id[0]) {
        return 0;
    }

    const BakeProject *existing = bake_model_find_project(ctx->world, id, NULL);
    if (existing && !bake_env_external_placeholder_project(existing)) {
        return 0;
    }

    project_json = bake_env_meta_project_json_path(ctx, id);
    if (!project_json) {
        return -1;
    }

    if (!bake_path_exists(project_json)) {
        goto cleanup;
    }

    cfg = ecs_os_calloc_t(bake_project_cfg_t);
    if (!cfg) {
        rc = -1;
        goto cleanup;
    }

    bake_project_cfg_init(cfg);
    if (bake_project_cfg_load_file(project_json, cfg) != 0) {
        rc = -1;
        goto cleanup;
    }

    ecs_os_free(cfg->id);
    cfg->id = bake_strdup(id);
    if (!cfg->id) {
        rc = -1;
        goto cleanup;
    }

    if (!cfg->public_project) {
        goto cleanup;
    }

    meta_dir = bake_join3_path(ctx->bake_home, "meta", id);
    source_txt = meta_dir ? bake_join_path(meta_dir, "source.txt") : NULL;
    source_path = source_txt ? bake_env_read_trimmed_file(source_txt) : NULL;
    if (source_path && source_path[0]) {
        ecs_os_free(cfg->path);
        cfg->path = source_path;
        source_path = NULL;
    }

    const char *mode = ctx->opts.mode ? ctx->opts.mode : "debug";
    artefact = bake_env_find_artefact_path_current_mode(ctx, cfg, mode);
    ecs_entity_t entity = bake_model_add_project(ctx->world, cfg, true);
    if (!entity) {
        rc = -1;
        goto cleanup;
    }
    cfg = NULL;

    if (artefact) {
        bake_env_set_project_artefact_result(ctx->world, entity, artefact);
        artefact = NULL;
    }

    rc = 1;
cleanup:
    if (cfg) {
        bake_project_cfg_fini(cfg);
        ecs_os_free(cfg);
    }
    ecs_os_free(artefact);
    ecs_os_free(source_path);
    ecs_os_free(source_txt);
    ecs_os_free(meta_dir);
    ecs_os_free(project_json);
    return rc;
}

int bake_environment_import_dependency_closure(bake_context_t *ctx) {
    bake_strlist_t queue;
    bake_strlist_t seen;
    bake_strlist_init(&queue);
    bake_strlist_init(&seen);

    int rc = -1;
    int imported = 0;
    ecs_iter_t seed = ecs_each_id(ctx->world, ecs_id(BakeProject));
    while (ecs_each_next(&seed)) {
        const BakeProject *projects = ecs_field(&seed, BakeProject, 0);
        for (int32_t i = 0; i < seed.count; i++) {
            const bake_project_cfg_t *cfg = projects[i].cfg;
            if (!cfg) {
                continue;
            }
            if (bake_env_queue_project_deps(&queue, cfg) != 0) {
                goto cleanup;
            }
        }
    }

    for (int32_t i = 0; i < queue.count; i++) {
        const char *id = queue.items[i];
        if (bake_strlist_contains(&seen, id)) {
            continue;
        }

        if (bake_strlist_append(&seen, id) != 0) {
            goto cleanup;
        }

        ecs_entity_t entity = 0;
        const BakeProject *project = bake_model_find_project(ctx->world, id, &entity);
        if (!project) {
            int rc = bake_environment_import_project_by_id(ctx, id);
            if (rc < 0) {
                goto cleanup;
            }
            if (rc > 0) {
                imported++;
            }
            project = bake_model_find_project(ctx->world, id, &entity);
        }

        if (project && project->cfg) {
            if (bake_env_queue_project_deps(&queue, project->cfg) != 0) {
                goto cleanup;
            }
        }
    }

    rc = imported;
cleanup:
    bake_strlist_fini(&queue);
    bake_strlist_fini(&seen);
    return rc;
}

static int bake_env_resolve_external_dep_entity(
    bake_context_t *ctx,
    ecs_entity_t dep_entity,
    const char *mode)
{
    int rc = 0;
    char *dep_id = NULL;
    char *project_json = NULL;

    const BakeProject *dep_project = ecs_get(ctx->world, dep_entity, BakeProject);
    if (!dep_project || !dep_project->external || !dep_project->cfg || !dep_project->cfg->id) {
        return 0;
    }

    const BakeBuildResult *dep_result = ecs_get(ctx->world, dep_entity, BakeBuildResult);
    if (dep_result && dep_result->artefact && bake_path_exists(dep_result->artefact)) {
        return 0;
    }

    dep_id = bake_strdup(dep_project->cfg->id);
    if (!dep_id) {
        return -1;
    }

    project_json = bake_env_meta_project_json_path(ctx, dep_id);
    if (!project_json) {
        rc = -1;
        goto cleanup;
    }

    bool has_meta = bake_path_exists(project_json);
    if (!has_meta) {
        goto cleanup;
    }

    int import_rc = bake_environment_import_project_by_id(ctx, dep_id);
    if (import_rc < 0) {
        rc = -1;
        goto cleanup;
    }

    ecs_entity_t resolved_entity = 0;
    const BakeProject *resolved = bake_model_find_project(ctx->world, dep_id, &resolved_entity);
    if (resolved && resolved->cfg) {
        char *artefact = bake_env_find_artefact_path_current_mode(ctx, resolved->cfg, mode);
        if (artefact) {
            bake_env_set_project_artefact_result(ctx->world, resolved_entity, artefact);
        }
    }

cleanup:
    ecs_os_free(project_json);
    ecs_os_free(dep_id);
    return rc;
}

int bake_environment_resolve_external_dependency_binaries(bake_context_t *ctx) {
    if (!ctx || !ctx->world) {
        return -1;
    }

    const char *mode = (ctx->opts.mode && ctx->opts.mode[0]) ? ctx->opts.mode : "debug";
    if (bake_model_refresh_resolved_deps(ctx->world, mode) != 0) {
        return -1;
    }

    ecs_iter_t it = ecs_each_id(ctx->world, ecs_id(BakeProject));
    while (ecs_each_next(&it)) {
        const BakeProject *projects = ecs_field(&it, BakeProject, 0);
        for (int32_t i = 0; i < it.count; i++) {
            const BakeProject *project = &projects[i];
            if (!project->cfg || project->external || !project->discovered) {
                continue;
            }

            const BakeResolvedDeps *resolved =
                ecs_get(ctx->world, it.entities[i], BakeResolvedDeps);
            if (!resolved) {
                continue;
            }

            for (int32_t d = 0; d < resolved->dep_count; d++) {
                if (bake_env_resolve_external_dep_entity(
                    ctx, resolved->deps[d], mode) != 0)
                {
                    return -1;
                }
            }
        }
    }

    return bake_model_refresh_resolved_deps(ctx->world, mode);
}

int bake_environment_sync_project(
    bake_context_t *ctx,
    ecs_entity_t project_entity,
    const BakeBuildResult *result,
    const BakeBuildRequest *req,
    bool rebuilt)
{
    const BakeProject *project = ecs_get(ctx->world, project_entity, BakeProject);
    if (!project || !project->cfg || project->external) {
        return 0;
    }

    const bake_project_cfg_t *cfg = project->cfg;
    if (!cfg->public_project || !cfg->id || !cfg->path) {
        return 0;
    }

    const char *mode = req && req->mode ? req->mode : (ctx->opts.mode ? ctx->opts.mode : "debug");
    if (!rebuilt && bake_env_project_entry_complete(ctx, cfg, mode)) {
        return 0;
    }

    int rc = -1;
    char *meta_dir = bake_join3_path(ctx->bake_home, "meta", cfg->id);
    char *include_dst = bake_join3_path(ctx->bake_home, "include", cfg->id);
    char *template_dst = bake_join3_path(ctx->bake_home, "template", cfg->id);
    char *source_txt = NULL;
    char *source_text = NULL;
    char *dependee_json = NULL;
    char *src_include = NULL;
    char *src_templates = NULL;
    char *legacy_path = NULL;
    char *scoped_path = NULL;

    if (!meta_dir || !include_dst || !template_dst) {
        goto cleanup;
    }

    if (bake_mkdirs(meta_dir) != 0) {
        goto cleanup;
    }

    if (bake_env_copy_required_file(cfg->path, meta_dir, "project.json") != 0) {
        goto cleanup;
    }

    source_txt = bake_join_path(meta_dir, "source.txt");
    source_text = bake_asprintf("%s\n", cfg->path);
    if (!source_txt || !source_text || bake_write_file(source_txt, source_text) != 0) {
        goto cleanup;
    }
    ecs_os_free(source_txt); source_txt = NULL;
    ecs_os_free(source_text); source_text = NULL;

    dependee_json = bake_join_path(meta_dir, "dependee.json");
    if (!dependee_json || bake_env_write_dependee_json(dependee_json, cfg) != 0) {
        goto cleanup;
    }
    ecs_os_free(dependee_json); dependee_json = NULL;

    if (bake_env_copy_optional_file(cfg->path, meta_dir, "LICENSE") != 0) {
        goto cleanup;
    }

    src_include = bake_join_path(cfg->path, "include");
    if (!src_include) {
        goto cleanup;
    }
    if (bake_env_copy_tree_exact(src_include, include_dst) != 0) {
        goto cleanup;
    }
    ecs_os_free(src_include); src_include = NULL;

    src_templates = bake_env_templates_dir(cfg);
    if (src_templates) {
        if (bake_env_copy_tree_exact(src_templates, template_dst) != 0) {
            goto cleanup;
        }
    } else if (bake_env_remove_if_exists(template_dst) != 0) {
        goto cleanup;
    }
    ecs_os_free(src_templates); src_templates = NULL;

    if (result && result->artefact && result->artefact[0] &&
        (cfg->kind == BAKE_PROJECT_PACKAGE ||
         cfg->kind == BAKE_PROJECT_APPLICATION ||
         cfg->kind == BAKE_PROJECT_TEST))
    {
        legacy_path = bake_env_artefact_path(ctx, cfg, mode);
        scoped_path = bake_env_artefact_path_scoped(ctx, cfg, mode);
        if (!legacy_path || !scoped_path ||
            bake_env_copy_artefact_to_path(result->artefact, legacy_path) != 0 ||
            bake_env_copy_artefact_to_path(result->artefact, scoped_path) != 0)
        {
            goto cleanup;
        }
        ecs_os_free(legacy_path); legacy_path = NULL;
        ecs_os_free(scoped_path); scoped_path = NULL;
    }

    rc = 0;

cleanup:
    ecs_os_free(source_txt);
    ecs_os_free(source_text);
    ecs_os_free(dependee_json);
    ecs_os_free(src_include);
    ecs_os_free(src_templates);
    ecs_os_free(legacy_path);
    ecs_os_free(scoped_path);
    ecs_os_free(meta_dir);
    ecs_os_free(include_dst);
    ecs_os_free(template_dst);
    return rc;
}

static int bake_env_remove_cfg_artefacts(
    const bake_project_cfg_t *cfg,
    const char *cfg_path,
    const char *file_name)
{
    const char *subdir = (cfg->kind == BAKE_PROJECT_PACKAGE) ? "lib" : "bin";
    char *base = bake_join_path(cfg_path, subdir);
    char *path = base ? bake_join_path(base, file_name) : NULL;
    char *scoped_dir = (base && cfg->id && cfg->id[0]) ? bake_join_path(base, cfg->id) : NULL;
    char *scoped_path = scoped_dir ? bake_join_path(scoped_dir, file_name) : NULL;
    int rc = -1;

    if (!base || !path || !scoped_dir || !scoped_path) {
        goto cleanup;
    }

    if ((bake_path_exists(path) && remove(path) != 0) ||
        (bake_path_exists(scoped_path) && remove(scoped_path) != 0))
    {
        goto cleanup;
    }

    if (bake_path_exists(scoped_dir)) {
        (void)bake_os_rmdir(scoped_dir);
    }

    rc = 0;
cleanup:
    ecs_os_free(base);
    ecs_os_free(path);
    ecs_os_free(scoped_dir);
    ecs_os_free(scoped_path);
    return rc;
}

static int bake_env_remove_project_artefacts(const bake_context_t *ctx, const bake_project_cfg_t *cfg) {
    int rc = -1;
    char *file_name = bake_project_cfg_artefact_name(cfg);
    if (!file_name) {
        return 0;
    }

    bake_dir_entry_t *roots = NULL;
    int32_t root_count = 0;
    if (bake_dir_list(ctx->bake_home, &roots, &root_count) != 0) {
        goto cleanup;
    }

    for (int32_t i = 0; i < root_count; i++) {
        bake_dir_entry_t *platform_dir = &roots[i];
        if (!platform_dir->is_dir || bake_env_is_dot(platform_dir->name)) {
            continue;
        }

        if (!strcmp(platform_dir->name, "meta") ||
            !strcmp(platform_dir->name, "include") ||
            !strcmp(platform_dir->name, "template") ||
            !strcmp(platform_dir->name, "bin"))
        {
            continue;
        }

        bake_dir_entry_t *cfg_dirs = NULL;
        int32_t cfg_count = 0;
        if (bake_dir_list(platform_dir->path, &cfg_dirs, &cfg_count) != 0) {
            goto cleanup;
        }

        for (int32_t c = 0; c < cfg_count; c++) {
            bake_dir_entry_t *cfg_dir = &cfg_dirs[c];
            if (!cfg_dir->is_dir || bake_env_is_dot(cfg_dir->name)) {
                continue;
            }
            if (bake_env_remove_cfg_artefacts(cfg, cfg_dir->path, file_name) != 0) {
                bake_dir_entries_free(cfg_dirs, cfg_count);
                goto cleanup;
            }
        }

        bake_dir_entries_free(cfg_dirs, cfg_count);
    }

    rc = 0;
cleanup:
    bake_dir_entries_free(roots, root_count);
    ecs_os_free(file_name);
    return rc;
}

static int bake_env_remove_project_entry(const bake_context_t *ctx, const char *id) {
    int rc = -1;
    char *meta_dir = bake_join3_path(ctx->bake_home, "meta", id);
    char *include_dir = bake_join3_path(ctx->bake_home, "include", id);
    char *template_dir = bake_join3_path(ctx->bake_home, "template", id);
    if (!meta_dir || !include_dir || !template_dir) {
        goto cleanup;
    }

    bake_project_cfg_t cfg;
    bake_project_cfg_init(&cfg);
    bool has_cfg = false;

    char *project_json = bake_join_path(meta_dir, "project.json");
    if (project_json && bake_path_exists(project_json)) {
        has_cfg = bake_project_cfg_load_file(project_json, &cfg) == 0;
    }
    ecs_os_free(project_json);

    if (bake_env_remove_if_exists(meta_dir) != 0 ||
        bake_env_remove_if_exists(include_dir) != 0 ||
        bake_env_remove_if_exists(template_dir) != 0)
    {
        goto cleanup_cfg;
    }

    if (has_cfg &&
        (cfg.kind == BAKE_PROJECT_PACKAGE ||
         cfg.kind == BAKE_PROJECT_APPLICATION ||
         cfg.kind == BAKE_PROJECT_TEST))
    {
        if (bake_env_remove_project_artefacts(ctx, &cfg) != 0) {
            goto cleanup_cfg;
        }
    }

    rc = 0;
cleanup_cfg:
    bake_project_cfg_fini(&cfg);
cleanup:
    ecs_os_free(meta_dir);
    ecs_os_free(include_dir);
    ecs_os_free(template_dir);
    return rc;
}

int bake_environment_reset(bake_context_t *ctx) {
    bake_dir_entry_t *entries = NULL;
    int32_t count = 0;
    if (bake_dir_list(ctx->bake_home, &entries, &count) != 0) {
        return -1;
    }

    for (int32_t i = 0; i < count; i++) {
        bake_dir_entry_t *entry = &entries[i];
        if (bake_env_is_dot(entry->name)) {
            continue;
        }
        if (!strcmp(entry->name, "bin")) {
            continue;
        }
        if (bake_env_remove_if_exists(entry->path) != 0) {
            bake_dir_entries_free(entries, count);
            return -1;
        }
    }

    bake_dir_entries_free(entries, count);
    return 0;
}

int bake_environment_cleanup(bake_context_t *ctx, int32_t *removed_out) {
    if (removed_out) {
        *removed_out = 0;
    }

    char *meta_root = bake_env_meta_dir(ctx);
    if (!meta_root) {
        return -1;
    }

    if (!bake_path_exists(meta_root) || !bake_is_dir(meta_root)) {
        ecs_os_free(meta_root);
        return 0;
    }

    int32_t removed = 0;
    bake_dir_entry_t *entries = NULL;
    int32_t count = 0;
    if (bake_dir_list(meta_root, &entries, &count) != 0) {
        ecs_os_free(meta_root);
        return -1;
    }

    for (int32_t i = 0; i < count; i++) {
        bake_dir_entry_t *entry = &entries[i];
        if (!entry->is_dir || bake_env_is_dot(entry->name)) {
            continue;
        }

        char *source_txt = bake_join_path(entry->path, "source.txt");
        char *source_path = source_txt ? bake_env_read_trimmed_file(source_txt) : NULL;
        ecs_os_free(source_txt);

        bool stale = true;
        if (source_path && source_path[0] && bake_path_exists(source_path)) {
            stale = false;
        }
        ecs_os_free(source_path);

        if (!stale) {
            continue;
        }

        if (bake_env_remove_project_entry(ctx, entry->name) != 0) {
            bake_dir_entries_free(entries, count);
            ecs_os_free(meta_root);
            return -1;
        }

        removed++;
    }

    bake_dir_entries_free(entries, count);
    ecs_os_free(meta_root);

    if (removed_out) {
        *removed_out = removed;
    }
    return 0;
}

int bake_environment_setup(bake_context_t *ctx, const char *argv0) {
    char *bin = bake_join_path(ctx->bake_home, "bin");
    if (!bin) {
        return -1;
    }

    if (bake_mkdirs(bin) != 0) {
        ecs_os_free(bin);
        return -1;
    }

    const char *target_name = bake_os_executable_name();

    char *dst = bake_join_path(bin, target_name);
    ecs_os_free(bin);
    if (!dst) {
        return -1;
    }

    char *src = NULL;
    if (argv0 && bake_path_exists(argv0)) {
        src = bake_strdup(argv0);
    } else {
        char *cwd = bake_getcwd();
        if (cwd) {
            src = bake_join_path(cwd, argv0);
            ecs_os_free(cwd);
        }
    }

    if (!src) {
        ecs_os_free(dst);
        return -1;
    }

    int rc = bake_copy_file(src, dst);
    if (rc == 0) {
        ecs_trace("installed bake to %s", dst);
    }

    ecs_os_free(src);
    ecs_os_free(dst);
    return rc;
}
