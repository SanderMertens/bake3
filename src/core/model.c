#include "bake2/model.h"

ECS_COMPONENT_DECLARE(BakeProject);
ECS_COMPONENT_DECLARE(BakeBuildRequest);
ECS_COMPONENT_DECLARE(BakeBuildResult);
ECS_COMPONENT_DECLARE(BakeResolvedDeps);
ECS_COMPONENT_DECLARE(BakeDriver);
ECS_COMPONENT_DECLARE(BakeBuildRule);
ECS_COMPONENT_DECLARE(BakeEnvProject);

ECS_TAG_DECLARE(BakeDiscovered);
ECS_TAG_DECLARE(BakeExternal);
ECS_TAG_DECLARE(BakeBuilt);
ECS_TAG_DECLARE(BakeBuildFailed);
ECS_TAG_DECLARE(BakeBuildInProgress);

ecs_entity_t BakeDependsOn = 0;

static int bake_entity_from_id(const ecs_world_t *world, const char *id, ecs_entity_t *entity_out) {
    ecs_iter_t it = ecs_each_id(world, ecs_id(BakeProject));
    while (ecs_each_next(&it)) {
        const BakeProject *projects = ecs_field(&it, BakeProject, 0);
        for (int32_t i = 0; i < it.count; i++) {
            const bake_project_cfg_t *cfg = projects[i].cfg;
            if (!cfg || !cfg->id) {
                continue;
            }
            if (!strcmp(cfg->id, id)) {
                if (entity_out) {
                    *entity_out = it.entities[i];
                }
                return 0;
            }
        }
    }

    return -1;
}

static const char* bake_project_entity_id(const ecs_world_t *world, ecs_entity_t entity) {
    const BakeProject *project = ecs_get(world, entity, BakeProject);
    const char *id = (project && project->cfg && project->cfg->id)
        ? project->cfg->id
        : ecs_get_name(world, entity);
    return id ? id : "";
}

static int bake_entity_cmp_by_project_id(const ecs_world_t *world, ecs_entity_t lhs, ecs_entity_t rhs) {
    return strcmp(bake_project_entity_id(world, lhs), bake_project_entity_id(world, rhs));
}

static void bake_entity_sort_by_project_id(const ecs_world_t *world, ecs_entity_t *entities, int32_t count) {
    for (int32_t i = 0; i < count - 1; i++) {
        for (int32_t j = i + 1; j < count; j++) {
            if (bake_entity_cmp_by_project_id(world, entities[i], entities[j]) > 0) {
                ecs_entity_t tmp = entities[i];
                entities[i] = entities[j];
                entities[j] = tmp;
            }
        }
    }
}

static void bake_model_merge_strlist_unique(bake_strlist_t *dst, const bake_strlist_t *src) {
    for (int32_t i = 0; i < src->count; i++) {
        if (!bake_strlist_contains(dst, src->items[i])) {
            bake_strlist_append(dst, src->items[i]);
        }
    }
}

static void bake_model_resolved_deps_init(BakeResolvedDeps *resolved) {
    memset(resolved, 0, sizeof(*resolved));
    bake_strlist_init(&resolved->include_paths);
    bake_strlist_init(&resolved->libs);
    bake_strlist_init(&resolved->ldflags);
    bake_strlist_init(&resolved->build_libpaths);
}

static void bake_model_resolved_deps_fini(BakeResolvedDeps *resolved) {
    ecs_os_free(resolved->deps);
    bake_strlist_fini(&resolved->include_paths);
    bake_strlist_fini(&resolved->libs);
    bake_strlist_fini(&resolved->ldflags);
    bake_strlist_fini(&resolved->build_libpaths);
    memset(resolved, 0, sizeof(*resolved));
}

static int bake_model_append_dep_entity(BakeResolvedDeps *resolved, ecs_entity_t dep) {
    int32_t next_count = resolved->dep_count + 1;
    ecs_entity_t *next = ecs_os_realloc_n(resolved->deps, ecs_entity_t, next_count);
    if (!next) {
        return -1;
    }
    resolved->deps = next;
    resolved->deps[resolved->dep_count++] = dep;
    return 0;
}

static void bake_model_try_append_include_path(
    const bake_project_cfg_t *cfg,
    const char *bake_home,
    bool external,
    BakeResolvedDeps *resolved)
{
    char *include = NULL;
    if (external && bake_home && cfg && cfg->id) {
        include = bake_join3_path(bake_home, "include", cfg->id);
    }

    if (!include && cfg && cfg->path) {
        include = bake_join_path(cfg->path, "include");
    }

    if (include && bake_path_exists(include) &&
        !bake_strlist_contains(&resolved->include_paths, include))
    {
        bake_strlist_append(&resolved->include_paths, include);
    }
    ecs_os_free(include);
}

static bool bake_cfg_is_external_placeholder(const bake_project_cfg_t *cfg) {
    if (!cfg || !cfg->id) {
        return false;
    }

    return cfg->path == NULL && cfg->output_name == NULL;
}

int bake_model_init(ecs_world_t *world) {
    ECS_COMPONENT_DEFINE(world, BakeProject);
    ECS_COMPONENT_DEFINE(world, BakeBuildRequest);
    ECS_COMPONENT_DEFINE(world, BakeBuildResult);
    ECS_COMPONENT_DEFINE(world, BakeResolvedDeps);
    ECS_COMPONENT_DEFINE(world, BakeDriver);
    ECS_COMPONENT_DEFINE(world, BakeBuildRule);
    ECS_COMPONENT_DEFINE(world, BakeEnvProject);

    ECS_TAG_DEFINE(world, BakeDiscovered);
    ECS_TAG_DEFINE(world, BakeExternal);
    ECS_TAG_DEFINE(world, BakeBuilt);
    ECS_TAG_DEFINE(world, BakeBuildFailed);
    ECS_TAG_DEFINE(world, BakeBuildInProgress);

    BakeDependsOn = ecs_entity(world, {
        .name = "BakeDependsOn"
    });

    ecs_add_id(world, BakeDependsOn, EcsTraversable);

    return 0;
}

ecs_entity_t bake_model_add_project(ecs_world_t *world, bake_project_cfg_t *cfg, bool external) {
    ecs_entity_t entity = 0;
    if (cfg->id) {
        entity = ecs_lookup_path_w_sep(world, 0, cfg->id, "::", NULL, false);
    }

    if (entity) {
        const BakeProject *existing = ecs_get(world, entity, BakeProject);
        if (existing && existing->cfg) {
            const bake_project_cfg_t *existing_cfg = existing->cfg;
            bool paths_conflict = existing_cfg->path && cfg->path &&
                strcmp(existing_cfg->path, cfg->path);

            if (paths_conflict) {
                if (existing_cfg->public_project && cfg->public_project) {
                    ecs_err(
                        "duplicate public project id '%s' for '%s' and '%s'",
                        cfg->id,
                        existing_cfg->path,
                        cfg->path);
                    bake_project_cfg_fini(cfg);
                    ecs_os_free(cfg);
                    return 0;
                }

                /* Ignore duplicate ids when either project is non-public. */
                bake_project_cfg_fini(cfg);
                ecs_os_free(cfg);
                return entity;
            }

            bool allow_replace =
                (existing->external && !external) ||
                (existing->external &&
                 external &&
                 bake_cfg_is_external_placeholder(existing_cfg) &&
                 !bake_cfg_is_external_placeholder(cfg));
            if (!allow_replace) {
                bake_project_cfg_fini(cfg);
                ecs_os_free(cfg);
                return entity;
            }

            if (existing_cfg != cfg) {
                bake_project_cfg_fini(existing->cfg);
                ecs_os_free(existing->cfg);
            }
        }
    }

    if (!entity) {
        ecs_entity_desc_t desc = {0};
        desc.name = cfg->id;
        desc.sep = "::";
        entity = ecs_entity_init(world, &desc);
    }

    BakeProject project = {
        .entity = entity,
        .cfg = cfg,
        .discovered = !external,
        .external = external
    };

    ecs_set_ptr(world, entity, BakeProject, &project);

    if (external) {
        ecs_add(world, entity, BakeExternal);
    } else {
        ecs_remove(world, entity, BakeExternal);
        ecs_add(world, entity, BakeDiscovered);
    }

    for (int32_t i = 0; i < cfg->drivers.count; i++) {
        ecs_entity_t drv = ecs_entity(world, {
            .parent = entity
        });
        ecs_set(world, drv, BakeDriver, { .id = bake_strdup(cfg->drivers.items[i]) });
    }

    for (int32_t i = 0; i < cfg->rules.count; i++) {
        ecs_entity_t rule = ecs_entity(world, {
            .parent = entity
        });
        ecs_set(world, rule, BakeBuildRule, {
            .ext = bake_strdup(cfg->rules.items[i].ext),
            .command = bake_strdup(cfg->rules.items[i].command)
        });
    }

    return entity;
}

const BakeProject* bake_model_get_project(const ecs_world_t *world, ecs_entity_t entity) {
    return ecs_get(world, entity, BakeProject);
}

const BakeProject* bake_model_find_project(const ecs_world_t *world, const char *id, ecs_entity_t *entity_out) {
    ecs_entity_t entity = 0;
    if (bake_entity_from_id(world, id, &entity) != 0) {
        return NULL;
    }

    if (entity_out) {
        *entity_out = entity;
    }

    return ecs_get(world, entity, BakeProject);
}

const BakeProject* bake_model_find_project_by_path(const ecs_world_t *world, const char *path, ecs_entity_t *entity_out) {
    ecs_iter_t it = ecs_each_id(world, ecs_id(BakeProject));
    while (ecs_each_next(&it)) {
        const BakeProject *projects = ecs_field(&it, BakeProject, 0);
        for (int32_t i = 0; i < it.count; i++) {
            const bake_project_cfg_t *cfg = projects[i].cfg;
            if (!cfg || !cfg->path) {
                continue;
            }
            if (!strcmp(cfg->path, path)) {
                if (entity_out) {
                    *entity_out = it.entities[i];
                }
                return ecs_get(world, it.entities[i], BakeProject);
            }
        }
    }

    return NULL;
}

static ecs_entity_t bake_model_ensure_dependency(ecs_world_t *world, const char *id) {
    ecs_entity_t dep = 0;
    if (bake_entity_from_id(world, id, &dep) == 0) {
        return dep;
    }

    bake_project_cfg_t *cfg = ecs_os_calloc_t(bake_project_cfg_t);
    if (!cfg) {
        return 0;
    }
    bake_project_cfg_init(cfg);
    ecs_os_free(cfg->id);
    cfg->id = bake_strdup(id);
    cfg->kind = BAKE_PROJECT_PACKAGE;
    cfg->path = NULL;
    cfg->private_project = false;

    return bake_model_add_project(world, cfg, true);
}

static void bake_model_link_project_list(ecs_world_t *world, ecs_entity_t entity, const bake_strlist_t *deps) {
    for (int32_t i = 0; i < deps->count; i++) {
        ecs_entity_t dep = bake_model_ensure_dependency(world, deps->items[i]);
        if (dep && dep != entity) {
            ecs_add_pair(world, entity, BakeDependsOn, dep);
        }
    }
}

void bake_model_link_dependencies(ecs_world_t *world) {
    ecs_iter_t it = ecs_each_id(world, ecs_id(BakeProject));
    while (ecs_each_next(&it)) {
        for (int32_t i = 0; i < it.count; i++) {
            ecs_entity_t e = it.entities[i];
            const BakeProject *project = ecs_field(&it, BakeProject, 0);
            const bake_project_cfg_t *cfg = project[i].cfg;
            if (!cfg || cfg->private_project) {
                continue;
            }

            bake_model_link_project_list(world, e, &cfg->use);
            bake_model_link_project_list(world, e, &cfg->use_private);
            bake_model_link_project_list(world, e, &cfg->use_build);
            bake_model_link_project_list(world, e, &cfg->use_runtime);
            if (cfg->dependee.cfg) {
                bake_model_link_project_list(world, e, &cfg->dependee.cfg->use);
                bake_model_link_project_list(world, e, &cfg->dependee.cfg->use_private);
                bake_model_link_project_list(world, e, &cfg->dependee.cfg->use_build);
                bake_model_link_project_list(world, e, &cfg->dependee.cfg->use_runtime);
            }
        }
    }
}

static void bake_model_mark_build_recursive(ecs_world_t *world, ecs_entity_t entity, const char *mode, bool recursive, bool standalone) {
    if (ecs_has(world, entity, BakeBuildRequest)) {
        return;
    }

    ecs_set(world, entity, BakeBuildRequest, {
        .mode = mode,
        .recursive = recursive,
        .standalone = standalone
    });

    if (!recursive) {
        return;
    }

    for (int32_t i = 0;; i++) {
        ecs_entity_t dep = ecs_get_target(world, entity, BakeDependsOn, i);
        if (!dep) {
            break;
        }
        bake_model_mark_build_recursive(world, dep, mode, true, standalone);
    }
}

void bake_model_mark_build_targets(ecs_world_t *world, const char *target, const char *mode, bool recursive, bool standalone) {
    ecs_iter_t clear = ecs_each_id(world, ecs_id(BakeBuildRequest));
    while (ecs_each_next(&clear)) {
        for (int32_t i = 0; i < clear.count; i++) {
            ecs_remove(world, clear.entities[i], BakeBuildRequest);
            ecs_remove(world, clear.entities[i], BakeBuilt);
            ecs_remove(world, clear.entities[i], BakeBuildFailed);
            ecs_remove(world, clear.entities[i], BakeBuildInProgress);
        }
    }

    if (!target || !target[0]) {
        ecs_iter_t all = ecs_each_id(world, ecs_id(BakeProject));
        while (ecs_each_next(&all)) {
            const BakeProject *project = ecs_field(&all, BakeProject, 0);
            for (int32_t i = 0; i < all.count; i++) {
                if (project[i].external) {
                    continue;
                }
                bake_model_mark_build_recursive(world, all.entities[i], mode, recursive, standalone);
            }
        }
        return;
    }

    if (bake_path_exists(target)) {
        bool target_is_dir = bake_is_dir(target);
        bool matched_dir = false;
        ecs_iter_t all = ecs_each_id(world, ecs_id(BakeProject));
        while (ecs_each_next(&all)) {
            const BakeProject *project = ecs_field(&all, BakeProject, 0);
            for (int32_t i = 0; i < all.count; i++) {
                const bake_project_cfg_t *cfg = project[i].cfg;
                if (!cfg || !cfg->path) {
                    continue;
                }
                if (bake_path_equal_normalized(cfg->path, target)) {
                    bake_model_mark_build_recursive(world, all.entities[i], mode, true, standalone);
                    return;
                }

                if (target_is_dir && bake_path_has_prefix_normalized(cfg->path, target, NULL)) {
                    bake_model_mark_build_recursive(world, all.entities[i], mode, true, standalone);
                    matched_dir = true;
                }
            }
        }

        if (matched_dir) {
            return;
        }
    }

    ecs_entity_t entity = 0;
    if (bake_entity_from_id(world, target, &entity) == 0) {
        bake_model_mark_build_recursive(world, entity, mode, true, standalone);
    }
}
static int bake_model_collect_resolved_deps(
    const ecs_world_t *world,
    ecs_entity_t project_entity,
    const char *mode,
    const char *bake_home,
    BakeResolvedDeps *resolved,
    ecs_entity_t **visited,
    int32_t *visited_count,
    int32_t *visited_capacity)
{
    for (int32_t i = 0;; i++) {
        ecs_entity_t dep = ecs_get_target(world, project_entity, BakeDependsOn, i);
        if (!dep) {
            break;
        }

        int visit_rc = bake_entity_list_append_unique(
            visited, visited_count, visited_capacity, dep);
        if (visit_rc < 0) {
            return -1;
        }
        if (!visit_rc) {
            continue;
        }

        if (bake_model_append_dep_entity(resolved, dep) != 0) {
            return -1;
        }

        const BakeProject *dep_project = ecs_get(world, dep, BakeProject);
        if (dep_project && dep_project->cfg) {
            const bake_project_cfg_t *cfg = dep_project->cfg;
            bake_model_try_append_include_path(cfg, bake_home, dep_project->external, resolved);

            if (!dep_project->external && cfg->path) {
                char *lib = bake_project_build_root(cfg->path, mode);
                if (lib && bake_path_exists(lib) &&
                    !bake_strlist_contains(&resolved->build_libpaths, lib))
                {
                    bake_strlist_append(&resolved->build_libpaths, lib);
                }
                ecs_os_free(lib);
            }

            bake_model_merge_strlist_unique(&resolved->libs, &cfg->c_lang.libs);
            bake_model_merge_strlist_unique(&resolved->libs, &cfg->cpp_lang.libs);
            bake_model_merge_strlist_unique(&resolved->ldflags, &cfg->c_lang.ldflags);
            bake_model_merge_strlist_unique(&resolved->ldflags, &cfg->cpp_lang.ldflags);

            if (cfg->dependee.cfg) {
                bake_model_merge_strlist_unique(&resolved->libs, &cfg->dependee.cfg->c_lang.libs);
                bake_model_merge_strlist_unique(&resolved->libs, &cfg->dependee.cfg->cpp_lang.libs);
                bake_model_merge_strlist_unique(&resolved->ldflags, &cfg->dependee.cfg->c_lang.ldflags);
                bake_model_merge_strlist_unique(&resolved->ldflags, &cfg->dependee.cfg->cpp_lang.ldflags);
            }
        }

        if (bake_model_collect_resolved_deps(
            world, dep, mode, bake_home, resolved, visited, visited_count, visited_capacity) != 0)
        {
            return -1;
        }
    }

    return 0;
}

int bake_model_refresh_resolved_deps(ecs_world_t *world, const char *mode) {
    const char *resolved_mode = (mode && mode[0]) ? mode : "debug";
    const char *bake_home = getenv("BAKE_HOME");

    ecs_iter_t it = ecs_each_id(world, ecs_id(BakeProject));
    while (ecs_each_next(&it)) {
        for (int32_t i = 0; i < it.count; i++) {
            ecs_entity_t entity = it.entities[i];

            BakeResolvedDeps *existing = (BakeResolvedDeps*)ecs_get(world, entity, BakeResolvedDeps);
            if (existing) {
                bake_model_resolved_deps_fini(existing);
            }

            BakeResolvedDeps resolved;
            bake_model_resolved_deps_init(&resolved);

            ecs_entity_t *visited = NULL;
            int32_t visited_count = 0;
            int32_t visited_capacity = 0;

            if (bake_entity_list_append_unique(
                &visited, &visited_count, &visited_capacity, entity) < 0)
            {
                ecs_os_free(visited);
                bake_model_resolved_deps_fini(&resolved);
                return -1;
            }

            if (bake_model_collect_resolved_deps(
                world,
                entity,
                resolved_mode,
                bake_home,
                &resolved,
                &visited,
                &visited_count,
                &visited_capacity) != 0)
            {
                ecs_os_free(visited);
                bake_model_resolved_deps_fini(&resolved);
                return -1;
            }

            ecs_os_free(visited);
            ecs_set_ptr(world, entity, BakeResolvedDeps, &resolved);
        }
    }

    return 0;
}

int bake_model_build_order(const ecs_world_t *world, ecs_entity_t **out_entities, int32_t *out_count) {
    *out_entities = NULL;
    *out_count = 0;

    int32_t count = 0;
    int32_t capacity = 32;
    ecs_entity_t *entities = ecs_os_malloc_n(ecs_entity_t, capacity);
    if (!entities) {
        return -1;
    }

    ecs_iter_t collect = ecs_each_id(world, ecs_id(BakeBuildRequest));
    while (ecs_each_next(&collect)) {
        for (int32_t i = 0; i < collect.count; i++) {
            if (count == capacity) {
                int32_t next = capacity * 2;
                ecs_entity_t *next_entities = ecs_os_realloc_n(entities, ecs_entity_t, next);
                if (!next_entities) {
                    ecs_os_free(entities);
                    return -1;
                }
                entities = next_entities;
                capacity = next;
            }
            entities[count++] = collect.entities[i];
        }
    }

    if (!count) {
        ecs_os_free(entities);
        return 0;
    }

    bake_entity_sort_by_project_id(world, entities, count);

    int32_t *indegree = ecs_os_calloc_n(int32_t, count);
    bool *scheduled = ecs_os_calloc_n(bool, count);
    ecs_entity_t *order = ecs_os_malloc_n(ecs_entity_t, count);
    if (!indegree || !scheduled || !order) {
        ecs_os_free(indegree);
        ecs_os_free(scheduled);
        ecs_os_free(order);
        ecs_os_free(entities);
        return -1;
    }

    for (int32_t i = 0; i < count; i++) {
        for (int32_t d = 0;; d++) {
            ecs_entity_t dep = ecs_get_target(world, entities[i], BakeDependsOn, d);
            if (!dep) {
                break;
            }

            for (int32_t j = 0; j < count; j++) {
                if (entities[j] == dep) {
                    indegree[i]++;
                    break;
                }
            }
        }
    }

    int32_t out_i = 0;
    while (out_i < count) {
        int32_t next = -1;
        for (int32_t i = 0; i < count; i++) {
            if (!scheduled[i] && indegree[i] == 0) {
                next = i;
                break;
            }
        }

        if (next == -1) {
            for (int32_t i = 0; i < count; i++) {
                if (!scheduled[i]) {
                    next = i;
                    break;
                }
            }
        }

        if (next == -1) {
            break;
        }

        ecs_entity_t resolved = entities[next];
        scheduled[next] = true;
        order[out_i++] = resolved;

        for (int32_t i = 0; i < count; i++) {
            if (scheduled[i]) {
                continue;
            }

            for (int32_t d = 0;; d++) {
                ecs_entity_t dep = ecs_get_target(world, entities[i], BakeDependsOn, d);
                if (!dep) {
                    break;
                }
                if (dep == resolved) {
                    indegree[i]--;
                    break;
                }
            }
        }
    }

    ecs_os_free(entities);
    ecs_os_free(indegree);
    ecs_os_free(scheduled);

    *out_entities = order;
    *out_count = out_i;
    return 0;
}
