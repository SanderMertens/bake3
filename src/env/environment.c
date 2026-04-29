#include "bake/environment.h"
#include "bake/os.h"
#include "env_internal.h"

bool bake_env_is_local(void) {
    const char *local_env = getenv("BAKE_LOCAL_ENV");
    return local_env && !strcmp(local_env, "1");
}

const char* bake_env_home(void) {
    return getenv("BAKE_HOME");
}

static char* bake_env_meta_dir(const bake_context_t *ctx) {
    return bake_path_join(ctx->bake_home, "meta");
}

static char* bake_env_meta_project_json_path(const bake_context_t *ctx, const char *id) {
    if (!ctx || !id || !id[0]) {
        return NULL;
    }

    char *meta_dir = bake_path_join3(ctx->bake_home, "meta", id);
    if (!meta_dir) {
        return NULL;
    }

    char *project_json = bake_path_join(meta_dir, "project.json");
    ecs_os_free(meta_dir);
    return project_json;
}

int bake_env_remove_if_exists(const char *path) {
    if (!path || !bake_path_exists(path)) {
        return 0;
    }
    return bake_rmtree(path);
}

static int bake_env_write_dependee_json(const char *path, const bake_project_cfg_t *cfg) {
    const char *dependee = "{}";
    if (cfg->dependee.json && cfg->dependee.json[0]) {
        dependee = cfg->dependee.json;
    }

    char *json = flecs_asprintf("%s\n", dependee);
    if (!json) {
        return -1;
    }

    int rc = bake_file_write(path, json);
    ecs_os_free(json);
    return rc;
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
    if (bake_mkdirs(dst_dir) != 0 || bake_file_copy(src_artefact, dst_path) != 0) {
        rc = -1;
    }

    ecs_os_free(dst_dir);
    return rc;
}

static int bake_env_copy_file(const char *src_dir, const char *dst_dir, const char *name, bool required) {
    int rc = -1;
    char *src = bake_path_join(src_dir, name);
    char *dst = bake_path_join(dst_dir, name);
    if (src && dst) {
        if (bake_path_exists(src)) {
            rc = bake_file_copy(src, dst);
        } else if (!required) {
            rc = bake_env_remove_if_exists(dst);
        }
    }
    ecs_os_free(src);
    ecs_os_free(dst);
    return rc;
}

static char* bake_env_templates_dir(const bake_project_cfg_t *cfg) {
    char *path = bake_path_join(cfg->path, "templates");
    if (path && bake_path_exists(path) && bake_path_is_dir(path)) {
        return path;
    }
    ecs_os_free(path);

    path = bake_path_join(cfg->path, "template");
    if (path && bake_path_exists(path) && bake_path_is_dir(path)) {
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
    char *meta_dir = bake_path_join3(ctx->bake_home, "meta", cfg->id);
    if (!meta_dir) {
        return 0;
    }

    char *project_json = bake_path_join(meta_dir, "project.json");
    char *source_txt = bake_path_join(meta_dir, "source.txt");
    char *dependee_json = bake_path_join(meta_dir, "dependee.json");
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
    if (!project || !project->external) {
        return false;
    }
    return bake_project_is_placeholder(project);
}

int bake_env_import_project_by_id(bake_context_t *ctx, const char *id) {
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
    cfg->id = ecs_os_strdup(id);
    if (!cfg->id) {
        rc = -1;
        goto cleanup;
    }

    if (!cfg->public_project) {
        goto cleanup;
    }

    meta_dir = bake_path_join3(ctx->bake_home, "meta", id);
    source_txt = meta_dir ? bake_path_join(meta_dir, "source.txt") : NULL;
    source_path = source_txt ? bake_file_read_trimmed(source_txt) : NULL;
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

int bake_env_import_dependency_closure(bake_context_t *ctx) {
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
            int import_rc = bake_env_import_project_by_id(ctx, id);
            if (import_rc < 0) {
                goto cleanup;
            }
            if (import_rc > 0) {
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

    dep_id = ecs_os_strdup(dep_project->cfg->id);
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

    int import_rc = bake_env_import_project_by_id(ctx, dep_id);
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

int bake_env_resolve_external_dependency_binaries(bake_context_t *ctx) {
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

static int bake_env_sync_metadata(
    const bake_project_cfg_t *cfg,
    const char *meta_dir)
{
    if (bake_mkdirs(meta_dir) != 0) {
        return -1;
    }

    if (bake_env_copy_file(cfg->path, meta_dir, "project.json", true) != 0) {
        return -1;
    }

    char *source_txt = bake_path_join(meta_dir, "source.txt");
    char *source_text = flecs_asprintf("%s\n", cfg->path);
    if (!source_txt || !source_text || bake_file_write(source_txt, source_text) != 0) {
        ecs_os_free(source_txt);
        ecs_os_free(source_text);
        return -1;
    }
    ecs_os_free(source_txt);
    ecs_os_free(source_text);

    char *dependee_json = bake_path_join(meta_dir, "dependee.json");
    if (!dependee_json || bake_env_write_dependee_json(dependee_json, cfg) != 0) {
        ecs_os_free(dependee_json);
        return -1;
    }
    ecs_os_free(dependee_json);

    if (bake_env_copy_file(cfg->path, meta_dir, "LICENSE", false) != 0) {
        return -1;
    }

    return 0;
}

static int bake_env_sync_includes(
    const bake_project_cfg_t *cfg,
    const char *include_dst)
{
    char *src_include = bake_path_join(cfg->path, "include");
    if (!src_include) {
        return -1;
    }

    int rc = bake_env_copy_tree_exact(src_include, include_dst);
    ecs_os_free(src_include);
    return rc;
}

static int bake_env_sync_templates(
    const bake_project_cfg_t *cfg,
    const char *template_dst)
{
    char *src_templates = bake_env_templates_dir(cfg);
    if (src_templates) {
        int rc = bake_env_copy_tree_exact(src_templates, template_dst);
        ecs_os_free(src_templates);
        return rc;
    }

    return bake_env_remove_if_exists(template_dst);
}

static int bake_env_sync_artefacts(
    const bake_context_t *ctx,
    const bake_project_cfg_t *cfg,
    const BakeBuildResult *result,
    const char *mode)
{
    if (!result || !result->artefact || !result->artefact[0]) {
        return 0;
    }

    if (cfg->kind != BAKE_PROJECT_PACKAGE &&
        cfg->kind != BAKE_PROJECT_APPLICATION &&
        cfg->kind != BAKE_PROJECT_TEST)
    {
        return 0;
    }

    int rc = -1;
    bool copy_scoped = true;
    size_t legacy_prefix_len = 0;
    char *legacy_path = bake_env_artefact_path(ctx, cfg, mode);
    char *scoped_path = bake_env_artefact_path_scoped(ctx, cfg, mode);

    if (legacy_path && scoped_path &&
        bake_path_has_prefix_normalized(scoped_path, legacy_path, &legacy_prefix_len) &&
        (scoped_path[legacy_prefix_len] == '/' || scoped_path[legacy_prefix_len] == '\\'))
    {
        /* Scoped artefact path nests under legacy file path when id equals output name. */
        copy_scoped = false;
    }

    if (!legacy_path ||
        bake_env_copy_artefact_to_path(result->artefact, legacy_path) != 0)
    {
        goto cleanup;
    }

    if (copy_scoped &&
        (!scoped_path ||
         bake_env_copy_artefact_to_path(result->artefact, scoped_path) != 0))
    {
        goto cleanup;
    }

    rc = 0;

cleanup:
    ecs_os_free(legacy_path);
    ecs_os_free(scoped_path);
    return rc;
}

int bake_env_sync_project(
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
    char *meta_dir = bake_path_join3(ctx->bake_home, "meta", cfg->id);
    char *include_dst = bake_path_join3(ctx->bake_home, "include", cfg->id);
    char *template_dst = bake_path_join3(ctx->bake_home, "template", cfg->id);

    if (!meta_dir || !include_dst || !template_dst) {
        goto cleanup;
    }

    if (bake_env_sync_metadata(cfg, meta_dir) != 0) {
        goto cleanup;
    }

    if (bake_env_sync_includes(cfg, include_dst) != 0) {
        goto cleanup;
    }

    if (bake_env_sync_templates(cfg, template_dst) != 0) {
        goto cleanup;
    }

    if (bake_env_sync_artefacts(ctx, cfg, result, mode) != 0) {
        goto cleanup;
    }

    rc = 0;

cleanup:
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
    char *base = bake_path_join(cfg_path, subdir);
    char *path = base ? bake_path_join(base, file_name) : NULL;
    int rc = -1;

    if (!base || !path) {
        goto cleanup;
    }

    if (bake_remove_file_if_exists(path) != 0) {
        goto cleanup;
    }

    /* Scoped path is only applicable when the project has an id */
    if (cfg->id && cfg->id[0]) {
        char *scoped_dir = bake_path_join(base, cfg->id);
        char *scoped_path = scoped_dir ? bake_path_join(scoped_dir, file_name) : NULL;

        if (scoped_path) {
            if (bake_remove_file_if_exists(scoped_path) != 0) {
                ecs_os_free(scoped_path);
                ecs_os_free(scoped_dir);
                goto cleanup;
            }
        }

        if (scoped_dir && bake_path_exists(scoped_dir)) {
            if (bake_os_rmdir(scoped_dir) != 0 && errno != ENOENT && errno != ENOTEMPTY) {
                bake_log_last_errno("remove empty directory", scoped_dir);
            }
        }

        ecs_os_free(scoped_path);
        ecs_os_free(scoped_dir);
    }

    rc = 0;
cleanup:
    ecs_os_free(base);
    ecs_os_free(path);
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
        if (!platform_dir->is_dir || bake_is_dot_dir(platform_dir->name)) {
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
            if (!cfg_dir->is_dir || bake_is_dot_dir(cfg_dir->name)) {
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
    char *meta_dir = bake_path_join3(ctx->bake_home, "meta", id);
    char *include_dir = bake_path_join3(ctx->bake_home, "include", id);
    char *template_dir = bake_path_join3(ctx->bake_home, "template", id);
    if (!meta_dir || !include_dir || !template_dir) {
        goto cleanup;
    }

    bake_project_cfg_t cfg;
    bake_project_cfg_init(&cfg);
    bool has_cfg = false;

    char *project_json = bake_path_join(meta_dir, "project.json");
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

int bake_env_reset(bake_context_t *ctx) {
    bake_dir_entry_t *entries = NULL;
    int32_t count = 0;
    if (bake_dir_list(ctx->bake_home, &entries, &count) != 0) {
        return -1;
    }

    for (int32_t i = 0; i < count; i++) {
        bake_dir_entry_t *entry = &entries[i];
        if (bake_is_dot_dir(entry->name)) {
            continue;
        }
        if (!strcmp(entry->name, "test")) {
            continue;
        }
        if (!strcmp(entry->name, "bake3")) {
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

int bake_env_cleanup(bake_context_t *ctx, int32_t *removed_out) {
    if (removed_out) {
        *removed_out = 0;
    }

    char *meta_root = bake_env_meta_dir(ctx);
    if (!meta_root) {
        return -1;
    }

    if (!bake_path_exists(meta_root) || !bake_path_is_dir(meta_root)) {
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
        if (!entry->is_dir || bake_is_dot_dir(entry->name)) {
            continue;
        }

        char *source_txt = bake_path_join(entry->path, "source.txt");
        char *source_path = source_txt ? bake_file_read_trimmed(source_txt) : NULL;
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
