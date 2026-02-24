#ifndef BAKE2_MODEL_H
#define BAKE2_MODEL_H

#include "bake2/context.h"

typedef struct b2_project_t {
    ecs_entity_t entity;
    b2_project_cfg_t *cfg;
    bool discovered;
    bool external;
} b2_project_t;

typedef struct b2_build_request_t {
    const char *mode;
    bool recursive;
    bool standalone;
} b2_build_request_t;

typedef struct b2_build_result_t {
    int32_t status;
    char *artefact;
} b2_build_result_t;

typedef struct b2_driver_t {
    char *id;
} b2_driver_t;

typedef struct b2_build_rule_t {
    char *ext;
    char *command;
} b2_build_rule_t;

typedef struct b2_env_project_t {
    char *id;
    char *path;
    char *kind;
} b2_env_project_t;

extern ECS_COMPONENT_DECLARE(b2_project_t);
extern ECS_COMPONENT_DECLARE(b2_build_request_t);
extern ECS_COMPONENT_DECLARE(b2_build_result_t);
extern ECS_COMPONENT_DECLARE(b2_driver_t);
extern ECS_COMPONENT_DECLARE(b2_build_rule_t);
extern ECS_COMPONENT_DECLARE(b2_env_project_t);

extern ecs_entity_t B2DependsOn;

extern ECS_TAG_DECLARE(B2Discovered);
extern ECS_TAG_DECLARE(B2External);
extern ECS_TAG_DECLARE(B2Built);
extern ECS_TAG_DECLARE(B2BuildFailed);
extern ECS_TAG_DECLARE(B2BuildInProgress);

int b2_model_init(ecs_world_t *world);
void b2_model_fini(ecs_world_t *world);

ecs_entity_t b2_model_add_project(ecs_world_t *world, b2_project_cfg_t *cfg, bool external);
const b2_project_t* b2_model_get_project(const ecs_world_t *world, ecs_entity_t entity);
const b2_project_t* b2_model_find_project(const ecs_world_t *world, const char *id, ecs_entity_t *entity_out);
void b2_model_link_dependencies(ecs_world_t *world);
void b2_model_mark_build_targets(ecs_world_t *world, const char *target, const char *mode, bool recursive, bool standalone);
int b2_model_build_order(const ecs_world_t *world, ecs_entity_t **out_entities, int32_t *out_count);

#endif
