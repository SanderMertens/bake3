#include "bake2/model.h"

ECS_COMPONENT_DECLARE(b2_project_t);
ECS_COMPONENT_DECLARE(b2_build_request_t);
ECS_COMPONENT_DECLARE(b2_build_result_t);
ECS_COMPONENT_DECLARE(b2_driver_t);
ECS_COMPONENT_DECLARE(b2_build_rule_t);
ECS_COMPONENT_DECLARE(b2_env_project_t);

ECS_TAG_DECLARE(B2Discovered);
ECS_TAG_DECLARE(B2External);
ECS_TAG_DECLARE(B2Built);
ECS_TAG_DECLARE(B2BuildFailed);
ECS_TAG_DECLARE(B2BuildInProgress);

ecs_entity_t B2DependsOn = 0;

static int b2_entity_from_id(const ecs_world_t *world, const char *id, ecs_entity_t *entity_out) {
    ecs_iter_t it = ecs_each_id(world, ecs_id(b2_project_t));
    while (ecs_each_next(&it)) {
        const b2_project_t *projects = ecs_field(&it, b2_project_t, 0);
        for (int32_t i = 0; i < it.count; i++) {
            const b2_project_cfg_t *cfg = projects[i].cfg;
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

static char* b2_id_from_path(const char *path) {
    char *id = b2_strdup(path);
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

int b2_model_init(ecs_world_t *world) {
    ECS_COMPONENT_DEFINE(world, b2_project_t);
    ECS_COMPONENT_DEFINE(world, b2_build_request_t);
    ECS_COMPONENT_DEFINE(world, b2_build_result_t);
    ECS_COMPONENT_DEFINE(world, b2_driver_t);
    ECS_COMPONENT_DEFINE(world, b2_build_rule_t);
    ECS_COMPONENT_DEFINE(world, b2_env_project_t);

    ECS_TAG_DEFINE(world, B2Discovered);
    ECS_TAG_DEFINE(world, B2External);
    ECS_TAG_DEFINE(world, B2Built);
    ECS_TAG_DEFINE(world, B2BuildFailed);
    ECS_TAG_DEFINE(world, B2BuildInProgress);

    B2DependsOn = ecs_entity(world, {
        .name = "B2DependsOn"
    });
    ecs_add_id(world, B2DependsOn, EcsTraversable);

    return 0;
}

void b2_model_fini(ecs_world_t *world) {
    B2_UNUSED(world);
}

ecs_entity_t b2_model_add_project(ecs_world_t *world, b2_project_cfg_t *cfg, bool external) {
    ecs_entity_t entity = 0;
    if (cfg->id) {
        entity = ecs_lookup_path_w_sep(world, 0, cfg->id, "::", NULL, false);
    }

    if (entity) {
        const b2_project_t *existing = ecs_get(world, entity, b2_project_t);
        if (existing && existing->cfg && existing->cfg->path && cfg->path &&
            strcmp(existing->cfg->path, cfg->path))
        {
            char *unique_id = b2_id_from_path(cfg->path);
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

    b2_project_t project = {
        .entity = entity,
        .cfg = cfg,
        .discovered = !external,
        .external = external
    };

    ecs_set_ptr(world, entity, b2_project_t, &project);

    if (external) {
        ecs_add(world, entity, B2External);
    } else {
        ecs_remove(world, entity, B2External);
        ecs_add(world, entity, B2Discovered);
    }

    for (int32_t i = 0; i < cfg->drivers.count; i++) {
        ecs_entity_t drv = ecs_entity(world, {
            .parent = entity
        });
        ecs_set(world, drv, b2_driver_t, { .id = b2_strdup(cfg->drivers.items[i]) });
    }

    for (int32_t i = 0; i < cfg->rules.count; i++) {
        ecs_entity_t rule = ecs_entity(world, {
            .parent = entity
        });
        ecs_set(world, rule, b2_build_rule_t, {
            .ext = b2_strdup(cfg->rules.items[i].ext),
            .command = b2_strdup(cfg->rules.items[i].command)
        });
    }

    return entity;
}

const b2_project_t* b2_model_get_project(const ecs_world_t *world, ecs_entity_t entity) {
    return ecs_get(world, entity, b2_project_t);
}

const b2_project_t* b2_model_find_project(const ecs_world_t *world, const char *id, ecs_entity_t *entity_out) {
    ecs_entity_t entity = 0;
    if (b2_entity_from_id(world, id, &entity) != 0) {
        return NULL;
    }

    if (entity_out) {
        *entity_out = entity;
    }

    return ecs_get(world, entity, b2_project_t);
}

const b2_project_t* b2_model_find_project_by_path(const ecs_world_t *world, const char *path, ecs_entity_t *entity_out) {
    ecs_iter_t it = ecs_each_id(world, ecs_id(b2_project_t));
    while (ecs_each_next(&it)) {
        const b2_project_t *projects = ecs_field(&it, b2_project_t, 0);
        for (int32_t i = 0; i < it.count; i++) {
            const b2_project_cfg_t *cfg = projects[i].cfg;
            if (!cfg || !cfg->path) {
                continue;
            }
            if (!strcmp(cfg->path, path)) {
                if (entity_out) {
                    *entity_out = it.entities[i];
                }
                return ecs_get(world, it.entities[i], b2_project_t);
            }
        }
    }

    return NULL;
}

static ecs_entity_t b2_model_ensure_dependency(ecs_world_t *world, const char *id) {
    ecs_entity_t dep = 0;
    if (b2_entity_from_id(world, id, &dep) == 0) {
        return dep;
    }

    b2_project_cfg_t *cfg = ecs_os_calloc_t(b2_project_cfg_t);
    if (!cfg) {
        return 0;
    }
    b2_project_cfg_init(cfg);
    ecs_os_free(cfg->id);
    cfg->id = b2_strdup(id);
    cfg->kind = B2_PROJECT_PACKAGE;
    cfg->path = NULL;
    cfg->private_project = false;

    return b2_model_add_project(world, cfg, true);
}

static void b2_model_link_project_list(ecs_world_t *world, ecs_entity_t entity, const b2_strlist_t *deps) {
    for (int32_t i = 0; i < deps->count; i++) {
        ecs_entity_t dep = b2_model_ensure_dependency(world, deps->items[i]);
        if (dep && dep != entity) {
            ecs_add_pair(world, entity, B2DependsOn, dep);
        }
    }
}

void b2_model_link_dependencies(ecs_world_t *world) {
    ecs_iter_t it = ecs_each_id(world, ecs_id(b2_project_t));
    while (ecs_each_next(&it)) {
        for (int32_t i = 0; i < it.count; i++) {
            ecs_entity_t e = it.entities[i];
            const b2_project_t *project = ecs_field(&it, b2_project_t, 0);
            const b2_project_cfg_t *cfg = project[i].cfg;
            if (!cfg || cfg->private_project) {
                continue;
            }

            b2_model_link_project_list(world, e, &cfg->use);
            b2_model_link_project_list(world, e, &cfg->use_private);
            b2_model_link_project_list(world, e, &cfg->use_build);
            b2_model_link_project_list(world, e, &cfg->use_runtime);
        }
    }
}

static void b2_model_mark_build_recursive(ecs_world_t *world, ecs_entity_t entity, const char *mode, bool recursive, bool standalone) {
    if (ecs_has(world, entity, b2_build_request_t)) {
        return;
    }

    ecs_set(world, entity, b2_build_request_t, {
        .mode = mode,
        .recursive = recursive,
        .standalone = standalone
    });

    if (!recursive) {
        return;
    }

    for (int32_t i = 0;; i++) {
        ecs_entity_t dep = ecs_get_target(world, entity, B2DependsOn, i);
        if (!dep) {
            break;
        }
        b2_model_mark_build_recursive(world, dep, mode, true, standalone);
    }
}

void b2_model_mark_build_targets(ecs_world_t *world, const char *target, const char *mode, bool recursive, bool standalone) {
    ecs_iter_t clear = ecs_each_id(world, ecs_id(b2_build_request_t));
    while (ecs_each_next(&clear)) {
        for (int32_t i = 0; i < clear.count; i++) {
            ecs_remove(world, clear.entities[i], b2_build_request_t);
            ecs_remove(world, clear.entities[i], B2Built);
            ecs_remove(world, clear.entities[i], B2BuildFailed);
            ecs_remove(world, clear.entities[i], B2BuildInProgress);
        }
    }

    if (!target || !target[0]) {
        ecs_iter_t all = ecs_each_id(world, ecs_id(b2_project_t));
        while (ecs_each_next(&all)) {
            const b2_project_t *project = ecs_field(&all, b2_project_t, 0);
            for (int32_t i = 0; i < all.count; i++) {
                if (project[i].external) {
                    continue;
                }
                b2_model_mark_build_recursive(world, all.entities[i], mode, recursive, standalone);
            }
        }
        return;
    }

    if (b2_path_exists(target)) {
        ecs_iter_t all = ecs_each_id(world, ecs_id(b2_project_t));
        while (ecs_each_next(&all)) {
            const b2_project_t *project = ecs_field(&all, b2_project_t, 0);
            for (int32_t i = 0; i < all.count; i++) {
                const b2_project_cfg_t *cfg = project[i].cfg;
                if (!cfg || !cfg->path) {
                    continue;
                }
                if (!strcmp(cfg->path, target)) {
                    b2_model_mark_build_recursive(world, all.entities[i], mode, true, standalone);
                    return;
                }
            }
        }
    }

    ecs_entity_t entity = 0;
    if (b2_entity_from_id(world, target, &entity) == 0) {
        b2_model_mark_build_recursive(world, entity, mode, true, standalone);
    }
}

int b2_model_build_order(const ecs_world_t *world, ecs_entity_t **out_entities, int32_t *out_count) {
    ecs_world_t *world_mut = ECS_CONST_CAST(ecs_world_t*, world);

    ecs_query_desc_t qd = {0};
    qd.terms[0].id = ecs_id(b2_build_request_t);
    qd.terms[1].id = ecs_id(b2_build_request_t);
    qd.terms[1].src.id = EcsCascade;
    qd.terms[1].trav = B2DependsOn;
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
