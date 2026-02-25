#include "bake2/model.h"

ECS_COMPONENT_DECLARE(BakeProject);
ECS_COMPONENT_DECLARE(BakeBuildRequest);
ECS_COMPONENT_DECLARE(BakeBuildResult);
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

static char* bake_id_from_path(const char *path) {
    char *id = bake_strdup(path);
    if (!id) {
        return NULL;
    }

    for (char *p = id; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ':') {
            *p = '.';
        }
    }

    while (id[0] == '.') {
        memmove(id, id + 1, strlen(id));
    }

    return id;
}

int bake_model_init(ecs_world_t *world) {
    ECS_COMPONENT_DEFINE(world, BakeProject);
    ECS_COMPONENT_DEFINE(world, BakeBuildRequest);
    ECS_COMPONENT_DEFINE(world, BakeBuildResult);
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
        if (existing && existing->cfg && existing->cfg->path && cfg->path &&
            strcmp(existing->cfg->path, cfg->path))
        {
            char *unique_id = bake_id_from_path(cfg->path);
            if (unique_id) {
                ecs_os_free(cfg->id);
                cfg->id = unique_id;
                entity = 0;
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
        ecs_iter_t all = ecs_each_id(world, ecs_id(BakeProject));
        while (ecs_each_next(&all)) {
            const BakeProject *project = ecs_field(&all, BakeProject, 0);
            for (int32_t i = 0; i < all.count; i++) {
                const bake_project_cfg_t *cfg = project[i].cfg;
                if (!cfg || !cfg->path) {
                    continue;
                }
                if (!strcmp(cfg->path, target)) {
                    bake_model_mark_build_recursive(world, all.entities[i], mode, true, standalone);
                    return;
                }
            }
        }
    }

    ecs_entity_t entity = 0;
    if (bake_entity_from_id(world, target, &entity) == 0) {
        bake_model_mark_build_recursive(world, entity, mode, true, standalone);
    }
}

int bake_model_build_order(const ecs_world_t *world, ecs_entity_t **out_entities, int32_t *out_count) {
    ecs_world_t *world_mut = ECS_CONST_CAST(ecs_world_t*, world);

    ecs_query_desc_t qd = {0};
    qd.terms[0].id = ecs_id(BakeBuildRequest);
    qd.terms[1].id = ecs_id(BakeBuildRequest);
    qd.terms[1].src.id = EcsCascade;
    qd.terms[1].trav = BakeDependsOn;
    qd.terms[1].oper = EcsOptional;

    ecs_query_t *q = ecs_query_init(world_mut, &qd);
    if (!q) {
        return -1;
    }

    int32_t count = 0;
    int32_t capacity = 32;
    ecs_entity_t *entities = ecs_os_malloc_n(ecs_entity_t, capacity);
    if (!entities) {
        ecs_query_fini(q);
        return -1;
    }

    ecs_iter_t it = ecs_query_iter(world_mut, q);
    while (ecs_query_next(&it)) {
        for (int32_t i = 0; i < it.count; i++) {
            if (count == capacity) {
                int32_t next = capacity * 2;
                ecs_entity_t *next_entities = ecs_os_realloc_n(entities, ecs_entity_t, next);
                if (!next_entities) {
                    ecs_os_free(entities);
                    ecs_query_fini(q);
                    return -1;
                }
                entities = next_entities;
                capacity = next;
            }
            entities[count++] = it.entities[i];
        }
    }

    ecs_query_fini(q);

    *out_entities = entities;
    *out_count = count;
    return 0;
}
