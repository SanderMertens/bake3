#include "bake/model.h"
#include "bake/build_components.h"
#include "bake/environment.h"
#include "bake/os.h"

ECS_COMPONENT_DECLARE(BakeProject);
ECS_COMPONENT_DECLARE(BakeResolvedDeps);
ECS_COMPONENT_DECLARE(BakeDriver);
ECS_COMPONENT_DECLARE(BakeBuildRule);
ECS_COMPONENT_DECLARE(BakeEnvProject);

ECS_TAG_DECLARE(BakeDiscovered);
ECS_TAG_DECLARE(BakeExternal);

ecs_entity_t BakeDependsOn = 0;

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

static void bake_resolved_deps_ctor(void *ptr, int32_t count, const ecs_type_info_t *type_info) {
    (void)type_info;
    BakeResolvedDeps *items = ptr;
    for (int32_t i = 0; i < count; i++) {
        bake_model_resolved_deps_init(&items[i]);
    }
}

static void bake_resolved_deps_dtor(void *ptr, int32_t count, const ecs_type_info_t *type_info) {
    (void)type_info;
    BakeResolvedDeps *items = ptr;
    for (int32_t i = 0; i < count; i++) {
        bake_model_resolved_deps_fini(&items[i]);
    }
}

static void bake_resolved_deps_move(void *dst_ptr, void *src_ptr, int32_t count, const ecs_type_info_t *type_info) {
    (void)type_info;
    BakeResolvedDeps *dst = dst_ptr;
    BakeResolvedDeps *src = src_ptr;
    for (int32_t i = 0; i < count; i++) {
        bake_model_resolved_deps_fini(&dst[i]);
        dst[i] = src[i];
        memset(&src[i], 0, sizeof(src[i]));
    }
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
        include = bake_path_join3(bake_home, "include", cfg->id);
    }

    if (!include && cfg && cfg->path) {
        include = bake_path_join(cfg->path, "include");
    }

    if (include && bake_path_exists(include) &&
        !bake_strlist_contains(&resolved->include_paths, include))
    {
        bake_strlist_append(&resolved->include_paths, include);
    }
    ecs_os_free(include);
}

bool bake_project_is_placeholder(const BakeProject *project) {
    if (!project || !project->cfg || !project->cfg->id) {
        return false;
    }

    return project->cfg->path == NULL && project->cfg->output_name == NULL;
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
    ecs_set_hooks(world, BakeResolvedDeps, {
        .ctor = bake_resolved_deps_ctor,
        .dtor = bake_resolved_deps_dtor,
        .move = bake_resolved_deps_move
    });
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
        ecs_set(world, drv, BakeDriver, { .id = ecs_os_strdup(cfg->drivers.items[i]) });
    }

    {
        bake_rule_t *rules = ecs_vec_first_t(&cfg->rules.vec, bake_rule_t);
        int32_t rule_count = ecs_vec_count(&cfg->rules.vec);
        for (int32_t i = 0; i < rule_count; i++) {
            ecs_entity_t rule = ecs_entity(world, {
                .parent = entity
            });
            ecs_set(world, rule, BakeBuildRule, {
                .ext = ecs_os_strdup(rules[i].ext),
                .command = ecs_os_strdup(rules[i].command)
            });
        }
    }

    return entity;
}

const BakeProject* bake_model_get_project(const ecs_world_t *world, ecs_entity_t entity) {
    return ecs_get(world, entity, BakeProject);
}

const BakeProject* bake_model_find_project(const ecs_world_t *world, const char *id, ecs_entity_t *entity_out) {
    ecs_entity_t entity = ecs_lookup_path_w_sep(world, 0, id, "::", NULL, false);
    if (!entity) {
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
    ecs_entity_t dep = ecs_lookup_path_w_sep(world, 0, id, "::", NULL, false);
    if (dep) {
        return dep;
    }

    bake_project_cfg_t *cfg = ecs_os_calloc_t(bake_project_cfg_t);
    if (!cfg) {
        return 0;
    }
    bake_project_cfg_init(cfg);
    ecs_os_free(cfg->id);
    cfg->id = ecs_os_strdup(id);
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
    ecs_defer_begin(world);

    ecs_iter_t it = ecs_each_id(world, ecs_id(BakeProject));
    while (ecs_each_next(&it)) {
        const BakeProject *projects = ecs_field(&it, BakeProject, 0);
        for (int32_t i = 0; i < it.count; i++) {
            const bake_project_cfg_t *cfg = projects[i].cfg;
            if (!cfg || cfg->private_project) {
                continue;
            }

            ecs_entity_t entity = it.entities[i];
            bake_model_link_project_list(world, entity, &cfg->use);
            bake_model_link_project_list(world, entity, &cfg->use_private);
            bake_model_link_project_list(world, entity, &cfg->use_build);
            bake_model_link_project_list(world, entity, &cfg->use_runtime);
            if (cfg->dependee.cfg) {
                bake_model_link_project_list(world, entity, &cfg->dependee.cfg->use);
                bake_model_link_project_list(world, entity, &cfg->dependee.cfg->use_private);
                bake_model_link_project_list(world, entity, &cfg->dependee.cfg->use_build);
                bake_model_link_project_list(world, entity, &cfg->dependee.cfg->use_runtime);
            }
        }
    }

    ecs_defer_end(world);
}

static void bake_model_mark_build_recursive_inner(
    ecs_world_t *world,
    ecs_entity_t entity,
    const char *mode,
    bool recursive,
    bool standalone,
    ecs_map_t *visited)
{
    if (ecs_map_get(visited, (ecs_map_key_t)entity)) {
        return;
    }
    ecs_map_insert(visited, (ecs_map_key_t)entity, 0);

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
        bake_model_mark_build_recursive_inner(world, dep, mode, true, standalone, visited);
    }
}

static void bake_model_mark_build_recursive(ecs_world_t *world, ecs_entity_t entity, const char *mode, bool recursive, bool standalone) {
    ecs_map_t visited = {0};
    ecs_map_init(&visited, NULL);
    bake_model_mark_build_recursive_inner(world, entity, mode, recursive, standalone, &visited);
    ecs_map_fini(&visited);
}

void bake_model_mark_build_targets(ecs_world_t *world, const char *target, const char *mode, bool recursive, bool standalone) {
    ecs_defer_begin(world);
    {
        ecs_iter_t clear = ecs_each_id(world, ecs_id(BakeBuildRequest));
        while (ecs_each_next(&clear)) {
            for (int32_t i = 0; i < clear.count; i++) {
                ecs_remove(world, clear.entities[i], BakeBuildRequest);
                ecs_remove(world, clear.entities[i], BakeBuilt);
                ecs_remove(world, clear.entities[i], BakeBuildFailed);
                ecs_remove(world, clear.entities[i], BakeBuildInProgress);
            }
        }
    }
    ecs_defer_end(world);

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
        bool target_is_dir = bake_path_is_dir(target);
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

    ecs_entity_t entity = ecs_lookup_path_w_sep(world, 0, target, "::", NULL, false);
    if (entity) {
        bake_model_mark_build_recursive(world, entity, mode, true, standalone);
    }
}
static int bake_model_collect_resolved_deps(
    const ecs_world_t *world,
    ecs_entity_t project_entity,
    const char *mode,
    const char *bake_home,
    BakeResolvedDeps *resolved,
    ecs_map_t *visited)
{
    for (int32_t i = 0;; i++) {
        ecs_entity_t dep = ecs_get_target(world, project_entity, BakeDependsOn, i);
        if (!dep) {
            break;
        }

        if (ecs_map_get(visited, (ecs_map_key_t)dep)) {
            continue;
        }
        ecs_map_insert(visited, (ecs_map_key_t)dep, 0);

        if (bake_model_append_dep_entity(resolved, dep) != 0) {
            return -1;
        }

        const BakeProject *dep_project = ecs_get(world, dep, BakeProject);
        if (dep_project && dep_project->cfg) {
            const bake_project_cfg_t *cfg = dep_project->cfg;
            bake_model_try_append_include_path(cfg, bake_home, dep_project->external, resolved);

            if (!dep_project->external && cfg->path) {
                char *lib = bake_project_build_root(cfg->path, cfg->id, mode);
                if (lib && bake_path_exists(lib) &&
                    !bake_strlist_contains(&resolved->build_libpaths, lib))
                {
                    bake_strlist_append(&resolved->build_libpaths, lib);
                }
                ecs_os_free(lib);
            }

            bake_strlist_merge_unique(&resolved->libs, &cfg->c_lang.libs);
            bake_strlist_merge_unique(&resolved->libs, &cfg->cpp_lang.libs);
            bake_strlist_merge_unique(&resolved->ldflags, &cfg->c_lang.ldflags);
            bake_strlist_merge_unique(&resolved->ldflags, &cfg->cpp_lang.ldflags);

            if (cfg->dependee.cfg) {
                bake_strlist_merge_unique(&resolved->libs, &cfg->dependee.cfg->c_lang.libs);
                bake_strlist_merge_unique(&resolved->libs, &cfg->dependee.cfg->cpp_lang.libs);
                bake_strlist_merge_unique(&resolved->ldflags, &cfg->dependee.cfg->c_lang.ldflags);
                bake_strlist_merge_unique(&resolved->ldflags, &cfg->dependee.cfg->cpp_lang.ldflags);
            }
        }

        if (bake_model_collect_resolved_deps(
            world, dep, mode, bake_home, resolved, visited) != 0)
        {
            return -1;
        }
    }

    return 0;
}

int bake_model_refresh_resolved_deps(ecs_world_t *world, const char *mode) {
    const char *resolved_mode = (mode && mode[0]) ? mode : "debug";
    const char *bake_home = bake_env_home();

    ecs_iter_t it = ecs_each_id(world, ecs_id(BakeProject));
    while (ecs_each_next(&it)) {
        for (int32_t i = 0; i < it.count; i++) {
            ecs_entity_t entity = it.entities[i];

            BakeResolvedDeps resolved;
            bake_model_resolved_deps_init(&resolved);

            ecs_map_t visited = {0};
            ecs_map_init(&visited, NULL);
            ecs_map_insert(&visited, (ecs_map_key_t)entity, 0);

            if (bake_model_collect_resolved_deps(
                world,
                entity,
                resolved_mode,
                bake_home,
                &resolved,
                &visited) != 0)
            {
                ecs_map_fini(&visited);
                bake_model_resolved_deps_fini(&resolved);
                return -1;
            }

            ecs_map_fini(&visited);
            ecs_set_ptr(world, entity, BakeResolvedDeps, &resolved);
        }
    }

    return 0;
}

static int bake_model_detect_cycle(
    const ecs_world_t *world,
    ecs_entity_t entity,
    ecs_map_t *visited,
    ecs_map_t *on_stack,
    ecs_entity_t *cycle_a,
    ecs_entity_t *cycle_b)
{
    ecs_map_val_t *state = ecs_map_get(visited, (ecs_map_key_t)entity);
    if (state) {
        if (ecs_map_get(on_stack, (ecs_map_key_t)entity)) {
            return -1;
        }
        return 0;
    }
    ecs_map_insert(visited, (ecs_map_key_t)entity, 1);
    ecs_map_insert(on_stack, (ecs_map_key_t)entity, 1);

    for (int32_t i = 0;; i++) {
        ecs_entity_t dep = ecs_get_target(world, entity, BakeDependsOn, i);
        if (!dep) {
            break;
        }
        if (ecs_map_get(on_stack, (ecs_map_key_t)dep)) {
            *cycle_a = entity;
            *cycle_b = dep;
            return -1;
        }
        if (bake_model_detect_cycle(world, dep, visited, on_stack, cycle_a, cycle_b) != 0) {
            if (!*cycle_a && !*cycle_b) {
                *cycle_a = entity;
            }
            return -1;
        }
    }

    ecs_map_remove(on_stack, (ecs_map_key_t)entity);
    return 0;
}

int bake_model_build_order(const ecs_world_t *world, ecs_entity_t **out_entities, int32_t *out_count) {
    *out_entities = NULL;
    *out_count = 0;

    ecs_vec_t vec = {0};
    ecs_vec_init_t(NULL, &vec, ecs_entity_t, 0);

    ecs_iter_t collect = ecs_each_id(world, ecs_id(BakeBuildRequest));
    while (ecs_each_next(&collect)) {
        for (int32_t i = 0; i < collect.count; i++) {
            *ecs_vec_append_t(NULL, &vec, ecs_entity_t) = collect.entities[i];
        }
    }

    int32_t count = ecs_vec_count(&vec);

    if (!count) {
        ecs_vec_fini_t(NULL, &vec, ecs_entity_t);
        return 0;
    }

    ecs_entity_t *vec_entities = ecs_vec_first_t(&vec, ecs_entity_t);

    ecs_map_t visited = {0};
    ecs_map_t on_stack = {0};
    ecs_map_init(&visited, NULL);
    ecs_map_init(&on_stack, NULL);

    ecs_entity_t cycle_a = 0;
    ecs_entity_t cycle_b = 0;
    bool has_cycle = false;
    for (int32_t i = 0; i < count; i++) {
        if (bake_model_detect_cycle(world, vec_entities[i], &visited, &on_stack, &cycle_a, &cycle_b) != 0) {
            has_cycle = true;
            break;
        }
    }

    ecs_map_fini(&visited);
    ecs_map_fini(&on_stack);

    if (has_cycle) {
        const char *id_a = cycle_a ? bake_project_entity_id(world, cycle_a) : "?";
        const char *id_b = cycle_b ? bake_project_entity_id(world, cycle_b) : "?";
        ecs_err("circular dependency detected involving '%s' and '%s'", id_a, id_b);
        ecs_vec_fini_t(NULL, &vec, ecs_entity_t);
        return -1;
    }

    ecs_entity_t *entities = vec_entities;

    int32_t *depths = ecs_os_malloc_n(int32_t, count);
    if (!depths) {
        ecs_vec_fini_t(NULL, &vec, ecs_entity_t);
        return -1;
    }

    bool has_valid_depth = true;
    for (int32_t i = 0; i < count; i++) {
        depths[i] = ecs_get_depth(world, entities[i], BakeDependsOn);
        if (depths[i] < 0) {
            has_valid_depth = false;
            break;
        }
    }

    if (!has_valid_depth) {
        bake_entity_sort_by_project_id(world, entities, count);
    } else {
        for (int32_t i = 0; i < count; i++) {
            for (int32_t j = i + 1; j < count; j++) {
                if (depths[j] < depths[i] ||
                    (depths[j] == depths[i] &&
                     bake_entity_cmp_by_project_id(world, entities[j], entities[i]) < 0))
                {
                    int32_t tmp_depth = depths[i];
                    depths[i] = depths[j];
                    depths[j] = tmp_depth;

                    ecs_entity_t tmp_entity = entities[i];
                    entities[i] = entities[j];
                    entities[j] = tmp_entity;
                }
            }
        }
    }

    ecs_os_free(depths);
    *out_entities = entities;
    *out_count = count;
    return 0;
}
