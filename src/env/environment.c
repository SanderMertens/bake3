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

static char* bake_env_artefact_file_name(const bake_project_cfg_t *cfg) {
    if (cfg->kind != BAKE_PROJECT_PACKAGE &&
        cfg->kind != BAKE_PROJECT_APPLICATION &&
        cfg->kind != BAKE_PROJECT_TEST)
    {
        return NULL;
    }

#if defined(_WIN32)
    const char *exe_ext = ".exe";
    const char *lib_ext = ".lib";
    const char *lib_prefix = "";
#else
    const char *exe_ext = "";
    const char *lib_ext = ".a";
    const char *lib_prefix = "lib";
#endif

    if (cfg->kind == BAKE_PROJECT_PACKAGE) {
        return bake_asprintf("%s%s%s", lib_prefix, cfg->output_name, lib_ext);
    }

    return bake_asprintf("%s%s", cfg->output_name, exe_ext);
}

static char* bake_env_artefact_path(const bake_context_t *ctx, const bake_project_cfg_t *cfg, const char *mode) {
    char *file_name = bake_env_artefact_file_name(cfg);
    if (!file_name) {
        return NULL;
    }

    char *platform = bake_host_platform();
    char *platform_dir = platform ? bake_join_path(ctx->bake_home, platform) : NULL;
    char *cfg_dir = platform_dir ? bake_join_path(platform_dir, mode && mode[0] ? mode : "debug") : NULL;
    const char *subdir = cfg->kind == BAKE_PROJECT_PACKAGE ? "lib" : "bin";
    char *out_dir = cfg_dir ? bake_join_path(cfg_dir, subdir) : NULL;
    char *out_path = out_dir ? bake_join_path(out_dir, file_name) : NULL;

    ecs_os_free(file_name);
    ecs_os_free(platform);
    ecs_os_free(platform_dir);
    ecs_os_free(cfg_dir);
    ecs_os_free(out_dir);
    return out_path;
}

static char* bake_env_artefact_path_scoped(
    const bake_context_t *ctx,
    const bake_project_cfg_t *cfg,
    const char *mode)
{
    if (!cfg || !cfg->id || !cfg->id[0]) {
        return NULL;
    }

    char *file_name = bake_env_artefact_file_name(cfg);
    if (!file_name) {
        return NULL;
    }

    char *platform = bake_host_platform();
    char *platform_dir = platform ? bake_join_path(ctx->bake_home, platform) : NULL;
    char *cfg_dir = platform_dir ? bake_join_path(platform_dir, mode && mode[0] ? mode : "debug") : NULL;
    const char *subdir = cfg->kind == BAKE_PROJECT_PACKAGE ? "lib" : "bin";
    char *out_dir = cfg_dir ? bake_join_path(cfg_dir, subdir) : NULL;
    char *id_dir = out_dir ? bake_join_path(out_dir, cfg->id) : NULL;
    char *out_path = id_dir ? bake_join_path(id_dir, file_name) : NULL;

    ecs_os_free(file_name);
    ecs_os_free(platform);
    ecs_os_free(platform_dir);
    ecs_os_free(cfg_dir);
    ecs_os_free(out_dir);
    ecs_os_free(id_dir);
    return out_path;
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
    if (bake_env_add_dependency_ids(queue, &cfg->use) != 0) return -1;
    if (bake_env_add_dependency_ids(queue, &cfg->use_private) != 0) return -1;
    if (bake_env_add_dependency_ids(queue, &cfg->use_build) != 0) return -1;
    if (bake_env_add_dependency_ids(queue, &cfg->use_runtime) != 0) return -1;
    if (cfg->dependee.cfg) {
        if (bake_env_add_dependency_ids(queue, &cfg->dependee.cfg->use) != 0) return -1;
        if (bake_env_add_dependency_ids(queue, &cfg->dependee.cfg->use_private) != 0) return -1;
        if (bake_env_add_dependency_ids(queue, &cfg->dependee.cfg->use_build) != 0) return -1;
        if (bake_env_add_dependency_ids(queue, &cfg->dependee.cfg->use_runtime) != 0) return -1;
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

int bake_environment_init_paths(bake_context_t *ctx) {
    const char *env_home = getenv("BAKE_HOME");
    if (env_home && env_home[0]) {
        ctx->bake_home = bake_strdup(env_home);
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

    if (bake_mkdirs(ctx->bake_home) != 0) {
        return -1;
    }

    return 0;
}

int bake_environment_import_project_by_id(bake_context_t *ctx, const char *id) {
    if (!id || !id[0]) {
        return 0;
    }

    const BakeProject *existing = bake_model_find_project(ctx->world, id, NULL);
    if (existing && !bake_env_external_placeholder_project(existing)) {
        return 0;
    }

    char *project_json = bake_env_meta_project_json_path(ctx, id);
    if (!project_json) {
        return -1;
    }

    if (!bake_path_exists(project_json)) {
        ecs_os_free(project_json);
        return 0;
    }

    bake_project_cfg_t *cfg = ecs_os_calloc_t(bake_project_cfg_t);
    if (!cfg) {
        ecs_os_free(project_json);
        return -1;
    }

    bake_project_cfg_init(cfg);
    if (bake_project_cfg_load_file(project_json, cfg) != 0) {
        bake_project_cfg_fini(cfg);
        ecs_os_free(cfg);
        ecs_os_free(project_json);
        return -1;
    }

    ecs_os_free(cfg->id);
    cfg->id = bake_strdup(id);
    if (!cfg->id) {
        bake_project_cfg_fini(cfg);
        ecs_os_free(cfg);
        ecs_os_free(project_json);
        return -1;
    }

    if (!cfg->public_project) {
        bake_project_cfg_fini(cfg);
        ecs_os_free(cfg);
        ecs_os_free(project_json);
        return 0;
    }

    char *meta_dir = bake_join3_path(ctx->bake_home, "meta", id);
    char *source_txt = meta_dir ? bake_join_path(meta_dir, "source.txt") : NULL;
    char *source_path = source_txt ? bake_env_read_trimmed_file(source_txt) : NULL;
    if (source_path && source_path[0]) {
        ecs_os_free(cfg->path);
        cfg->path = source_path;
        source_path = NULL;
    }
    ecs_os_free(source_path);
    ecs_os_free(source_txt);

    const char *mode = ctx->opts.mode ? ctx->opts.mode : "debug";
    char *artefact = bake_env_find_artefact_path_current_mode(ctx, cfg, mode);
    ecs_entity_t entity = bake_model_add_project(ctx->world, cfg, true);
    if (!entity) {
        ecs_os_free(artefact);
        ecs_os_free(project_json);
        ecs_os_free(meta_dir);
        return -1;
    }

    if (artefact) {
        bake_env_set_project_artefact_result(ctx->world, entity, artefact);
    }

    ecs_os_free(project_json);
    ecs_os_free(meta_dir);
    return 1;
}

int bake_environment_import_dependency_closure(bake_context_t *ctx) {
    bake_strlist_t queue;
    bake_strlist_t seen;
    bake_strlist_init(&queue);
    bake_strlist_init(&seen);

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
                bake_strlist_fini(&queue);
                bake_strlist_fini(&seen);
                return -1;
            }
        }
    }

    for (int32_t i = 0; i < queue.count; i++) {
        const char *id = queue.items[i];
        if (bake_strlist_contains(&seen, id)) {
            continue;
        }

        if (bake_strlist_append(&seen, id) != 0) {
            bake_strlist_fini(&queue);
            bake_strlist_fini(&seen);
            return -1;
        }

        ecs_entity_t entity = 0;
        const BakeProject *project = bake_model_find_project(ctx->world, id, &entity);
        if (!project) {
            int rc = bake_environment_import_project_by_id(ctx, id);
            if (rc < 0) {
                bake_strlist_fini(&queue);
                bake_strlist_fini(&seen);
                return -1;
            }
            if (rc > 0) {
                imported++;
            }
            project = bake_model_find_project(ctx->world, id, &entity);
        }

        if (project && project->cfg) {
            if (bake_env_queue_project_deps(&queue, project->cfg) != 0) {
                bake_strlist_fini(&queue);
                bake_strlist_fini(&seen);
                return -1;
            }
        }
    }

    bake_strlist_fini(&queue);
    bake_strlist_fini(&seen);
    return imported;
}

static int bake_env_entity_list_append_unique(
    ecs_entity_t **entities,
    int32_t *count,
    int32_t *capacity,
    ecs_entity_t entity)
{
    for (int32_t i = 0; i < *count; i++) {
        if ((*entities)[i] == entity) {
            return 0;
        }
    }

    if (*count == *capacity) {
        int32_t next = *capacity ? *capacity * 2 : 64;
        ecs_entity_t *next_entities = ecs_os_realloc_n(*entities, ecs_entity_t, next);
        if (!next_entities) {
            return -1;
        }
        *entities = next_entities;
        *capacity = next;
    }

    (*entities)[(*count)++] = entity;
    return 1;
}

static int bake_env_resolve_external_dep_entity(
    bake_context_t *ctx,
    ecs_entity_t dep_entity,
    const char *mode)
{
    const BakeProject *dep_project = ecs_get(ctx->world, dep_entity, BakeProject);
    if (!dep_project || !dep_project->external || !dep_project->cfg || !dep_project->cfg->id) {
        return 0;
    }

    const BakeBuildResult *dep_result = ecs_get(ctx->world, dep_entity, BakeBuildResult);
    if (dep_result && dep_result->artefact && bake_path_exists(dep_result->artefact)) {
        return 0;
    }

    char *dep_id = bake_strdup(dep_project->cfg->id);
    if (!dep_id) {
        return -1;
    }

    char *project_json = bake_env_meta_project_json_path(ctx, dep_id);
    if (!project_json) {
        ecs_os_free(dep_id);
        return -1;
    }

    bool has_meta = bake_path_exists(project_json);
    ecs_os_free(project_json);
    if (!has_meta) {
        ecs_os_free(dep_id);
        return 0;
    }

    int import_rc = bake_environment_import_project_by_id(ctx, dep_id);
    if (import_rc < 0) {
        ecs_os_free(dep_id);
        return -1;
    }

    ecs_entity_t resolved_entity = 0;
    const BakeProject *resolved = bake_model_find_project(ctx->world, dep_id, &resolved_entity);
    if (resolved && resolved->cfg) {
        char *artefact = bake_env_find_artefact_path_current_mode(ctx, resolved->cfg, mode);
        if (artefact) {
            bake_env_set_project_artefact_result(ctx->world, resolved_entity, artefact);
        }
    }

    ecs_os_free(dep_id);
    return 0;
}

static int bake_env_resolve_dependency_recursive(
    bake_context_t *ctx,
    ecs_entity_t project_entity,
    const char *mode,
    ecs_entity_t **visited,
    int32_t *visited_count,
    int32_t *visited_capacity)
{
    if (!project_entity) {
        return 0;
    }

    int visit_rc = bake_env_entity_list_append_unique(
        visited, visited_count, visited_capacity, project_entity);
    if (visit_rc <= 0) {
        return visit_rc;
    }

    for (int32_t i = 0;; i++) {
        ecs_entity_t dep = ecs_get_target(ctx->world, project_entity, BakeDependsOn, i);
        if (!dep) {
            break;
        }

        if (bake_env_resolve_external_dep_entity(ctx, dep, mode) != 0) {
            return -1;
        }

        if (bake_env_resolve_dependency_recursive(
            ctx, dep, mode, visited, visited_count, visited_capacity) != 0)
        {
            return -1;
        }
    }

    return 0;
}

int bake_environment_resolve_external_dependency_binaries(bake_context_t *ctx) {
    if (!ctx || !ctx->world) {
        return -1;
    }

    const char *mode = (ctx->opts.mode && ctx->opts.mode[0]) ? ctx->opts.mode : "debug";
    ecs_entity_t *visited = NULL;
    int32_t visited_count = 0;
    int32_t visited_capacity = 0;

    ecs_iter_t it = ecs_each_id(ctx->world, ecs_id(BakeProject));
    while (ecs_each_next(&it)) {
        const BakeProject *projects = ecs_field(&it, BakeProject, 0);
        for (int32_t i = 0; i < it.count; i++) {
            const BakeProject *project = &projects[i];
            if (!project->cfg || project->external || !project->discovered) {
                continue;
            }

            if (bake_env_resolve_dependency_recursive(
                ctx,
                it.entities[i],
                mode,
                &visited,
                &visited_count,
                &visited_capacity) != 0)
            {
                ecs_os_free(visited);
                return -1;
            }
        }
    }

    ecs_os_free(visited);
    return 0;
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

    char *meta_dir = bake_join3_path(ctx->bake_home, "meta", cfg->id);
    char *include_dst = bake_join3_path(ctx->bake_home, "include", cfg->id);
    char *template_dst = bake_join3_path(ctx->bake_home, "template", cfg->id);
    if (!meta_dir || !include_dst || !template_dst) {
        ecs_os_free(meta_dir);
        ecs_os_free(include_dst);
        ecs_os_free(template_dst);
        return -1;
    }

    if (bake_mkdirs(meta_dir) != 0) {
        ecs_os_free(meta_dir);
        ecs_os_free(include_dst);
        ecs_os_free(template_dst);
        return -1;
    }

    char *src_project_json = bake_join_path(cfg->path, "project.json");
    char *dst_project_json = bake_join_path(meta_dir, "project.json");
    if (!src_project_json || !dst_project_json ||
        !bake_path_exists(src_project_json) || bake_copy_file(src_project_json, dst_project_json) != 0)
    {
        ecs_os_free(src_project_json);
        ecs_os_free(dst_project_json);
        ecs_os_free(meta_dir);
        ecs_os_free(include_dst);
        ecs_os_free(template_dst);
        return -1;
    }
    ecs_os_free(src_project_json);
    ecs_os_free(dst_project_json);

    char *source_txt = bake_join_path(meta_dir, "source.txt");
    char *source_text = bake_asprintf("%s\n", cfg->path);
    if (!source_txt || !source_text || bake_write_file(source_txt, source_text) != 0) {
        ecs_os_free(source_txt);
        ecs_os_free(source_text);
        ecs_os_free(meta_dir);
        ecs_os_free(include_dst);
        ecs_os_free(template_dst);
        return -1;
    }
    ecs_os_free(source_txt);
    ecs_os_free(source_text);

    char *dependee_json = bake_join_path(meta_dir, "dependee.json");
    if (!dependee_json || bake_env_write_dependee_json(dependee_json, cfg) != 0) {
        ecs_os_free(dependee_json);
        ecs_os_free(meta_dir);
        ecs_os_free(include_dst);
        ecs_os_free(template_dst);
        return -1;
    }
    ecs_os_free(dependee_json);

    char *src_license = bake_join_path(cfg->path, "LICENSE");
    char *dst_license = bake_join_path(meta_dir, "LICENSE");
    if (!src_license || !dst_license) {
        ecs_os_free(src_license);
        ecs_os_free(dst_license);
        ecs_os_free(meta_dir);
        ecs_os_free(include_dst);
        ecs_os_free(template_dst);
        return -1;
    }
    if (bake_path_exists(src_license)) {
        if (bake_copy_file(src_license, dst_license) != 0) {
            ecs_os_free(src_license);
            ecs_os_free(dst_license);
            ecs_os_free(meta_dir);
            ecs_os_free(include_dst);
            ecs_os_free(template_dst);
            return -1;
        }
    } else if (bake_env_remove_if_exists(dst_license) != 0) {
        ecs_os_free(src_license);
        ecs_os_free(dst_license);
        ecs_os_free(meta_dir);
        ecs_os_free(include_dst);
        ecs_os_free(template_dst);
        return -1;
    }
    ecs_os_free(src_license);
    ecs_os_free(dst_license);

    char *src_include = bake_join_path(cfg->path, "include");
    if (!src_include) {
        ecs_os_free(meta_dir);
        ecs_os_free(include_dst);
        ecs_os_free(template_dst);
        return -1;
    }
    if (bake_env_copy_tree_exact(src_include, include_dst) != 0) {
        ecs_os_free(src_include);
        ecs_os_free(meta_dir);
        ecs_os_free(include_dst);
        ecs_os_free(template_dst);
        return -1;
    }
    ecs_os_free(src_include);

    char *src_templates = bake_join_path(cfg->path, "templates");
    if (!src_templates) {
        ecs_os_free(meta_dir);
        ecs_os_free(include_dst);
        ecs_os_free(template_dst);
        return -1;
    }
    if (!bake_path_exists(src_templates) || !bake_is_dir(src_templates)) {
        ecs_os_free(src_templates);
        src_templates = bake_join_path(cfg->path, "template");
    }

    if (src_templates && bake_path_exists(src_templates) && bake_is_dir(src_templates)) {
        if (bake_env_copy_tree_exact(src_templates, template_dst) != 0) {
            ecs_os_free(src_templates);
            ecs_os_free(meta_dir);
            ecs_os_free(include_dst);
            ecs_os_free(template_dst);
            return -1;
        }
    } else if (bake_env_remove_if_exists(template_dst) != 0) {
        ecs_os_free(src_templates);
        ecs_os_free(meta_dir);
        ecs_os_free(include_dst);
        ecs_os_free(template_dst);
        return -1;
    }
    ecs_os_free(src_templates);

    if (result && result->artefact && result->artefact[0] &&
        (cfg->kind == BAKE_PROJECT_PACKAGE ||
         cfg->kind == BAKE_PROJECT_APPLICATION ||
         cfg->kind == BAKE_PROJECT_TEST))
    {
        char *legacy_path = bake_env_artefact_path(ctx, cfg, mode);
        char *scoped_path = bake_env_artefact_path_scoped(ctx, cfg, mode);
        if (!legacy_path || !scoped_path ||
            bake_env_copy_artefact_to_path(result->artefact, legacy_path) != 0 ||
            bake_env_copy_artefact_to_path(result->artefact, scoped_path) != 0)
        {
            ecs_os_free(legacy_path);
            ecs_os_free(scoped_path);
            ecs_os_free(meta_dir);
            ecs_os_free(include_dst);
            ecs_os_free(template_dst);
            return -1;
        }
        ecs_os_free(legacy_path);
        ecs_os_free(scoped_path);
    }

    ecs_os_free(meta_dir);
    ecs_os_free(include_dst);
    ecs_os_free(template_dst);
    return 0;
}

static int bake_env_remove_project_artefacts(const bake_context_t *ctx, const bake_project_cfg_t *cfg) {
    char *file_name = bake_env_artefact_file_name(cfg);
    if (!file_name) {
        return 0;
    }

    bake_dir_entry_t *roots = NULL;
    int32_t root_count = 0;
    if (bake_dir_list(ctx->bake_home, &roots, &root_count) != 0) {
        ecs_os_free(file_name);
        return -1;
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
            bake_dir_entries_free(roots, root_count);
            ecs_os_free(file_name);
            return -1;
        }

        for (int32_t c = 0; c < cfg_count; c++) {
            bake_dir_entry_t *cfg_dir = &cfg_dirs[c];
            if (!cfg_dir->is_dir || bake_env_is_dot(cfg_dir->name)) {
                continue;
            }

            const char *subdir = (cfg->kind == BAKE_PROJECT_PACKAGE) ? "lib" : "bin";
            char *base = bake_join_path(cfg_dir->path, subdir);
            char *path = base ? bake_join_path(base, file_name) : NULL;
            char *scoped_dir = (base && cfg->id && cfg->id[0]) ? bake_join_path(base, cfg->id) : NULL;
            char *scoped_path = scoped_dir ? bake_join_path(scoped_dir, file_name) : NULL;
            if (!base || !path || !scoped_dir || !scoped_path) {
                ecs_os_free(base);
                ecs_os_free(path);
                ecs_os_free(scoped_dir);
                ecs_os_free(scoped_path);
                bake_dir_entries_free(cfg_dirs, cfg_count);
                bake_dir_entries_free(roots, root_count);
                ecs_os_free(file_name);
                return -1;
            }

            if (bake_path_exists(path)) {
                if (remove(path) != 0) {
                    ecs_os_free(base);
                    ecs_os_free(path);
                    ecs_os_free(scoped_dir);
                    ecs_os_free(scoped_path);
                    bake_dir_entries_free(cfg_dirs, cfg_count);
                    bake_dir_entries_free(roots, root_count);
                    ecs_os_free(file_name);
                    return -1;
                }
            }

            if (bake_path_exists(scoped_path)) {
                if (remove(scoped_path) != 0) {
                    ecs_os_free(base);
                    ecs_os_free(path);
                    ecs_os_free(scoped_dir);
                    ecs_os_free(scoped_path);
                    bake_dir_entries_free(cfg_dirs, cfg_count);
                    bake_dir_entries_free(roots, root_count);
                    ecs_os_free(file_name);
                    return -1;
                }
            }

            if (bake_path_exists(scoped_dir)) {
                (void)bake_os_rmdir(scoped_dir);
            }

            ecs_os_free(base);
            ecs_os_free(path);
            ecs_os_free(scoped_dir);
            ecs_os_free(scoped_path);
        }

        bake_dir_entries_free(cfg_dirs, cfg_count);
    }

    bake_dir_entries_free(roots, root_count);
    ecs_os_free(file_name);
    return 0;
}

static int bake_env_remove_project_entry(const bake_context_t *ctx, const char *id) {
    char *meta_dir = bake_join3_path(ctx->bake_home, "meta", id);
    char *include_dir = bake_join3_path(ctx->bake_home, "include", id);
    char *template_dir = bake_join3_path(ctx->bake_home, "template", id);
    if (!meta_dir || !include_dir || !template_dir) {
        ecs_os_free(meta_dir);
        ecs_os_free(include_dir);
        ecs_os_free(template_dir);
        return -1;
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
        bake_project_cfg_fini(&cfg);
        ecs_os_free(meta_dir);
        ecs_os_free(include_dir);
        ecs_os_free(template_dir);
        return -1;
    }

    if (has_cfg &&
        (cfg.kind == BAKE_PROJECT_PACKAGE ||
         cfg.kind == BAKE_PROJECT_APPLICATION ||
         cfg.kind == BAKE_PROJECT_TEST))
    {
        if (bake_env_remove_project_artefacts(ctx, &cfg) != 0) {
            bake_project_cfg_fini(&cfg);
            ecs_os_free(meta_dir);
            ecs_os_free(include_dir);
            ecs_os_free(template_dir);
            return -1;
        }
    }

    bake_project_cfg_fini(&cfg);
    ecs_os_free(meta_dir);
    ecs_os_free(include_dir);
    ecs_os_free(template_dir);
    return 0;
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
