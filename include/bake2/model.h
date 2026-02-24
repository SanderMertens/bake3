#ifndef BAKE2_MODEL_H
#define BAKE2_MODEL_H

#include "bake2/context.h"

typedef struct BakeProject {
    ecs_entity_t entity;
    bake_project_cfg_t *cfg;
    bool discovered;
    bool external;
} BakeProject;

typedef struct BakeBuildRequest {
    const char *mode;
    bool recursive;
    bool standalone;
} BakeBuildRequest;

typedef struct BakeBuildResult {
    int32_t status;
    char *artefact;
} BakeBuildResult;

typedef struct BakeDriver {
    char *id;
} BakeDriver;

typedef struct BakeBuildRule {
    char *ext;
    char *command;
} BakeBuildRule;

typedef struct BakeEnvProject {
    char *id;
    char *path;
    char *kind;
} BakeEnvProject;

extern ECS_COMPONENT_DECLARE(BakeProject);
extern ECS_COMPONENT_DECLARE(BakeBuildRequest);
extern ECS_COMPONENT_DECLARE(BakeBuildResult);
extern ECS_COMPONENT_DECLARE(BakeDriver);
extern ECS_COMPONENT_DECLARE(BakeBuildRule);
extern ECS_COMPONENT_DECLARE(BakeEnvProject);

extern ecs_entity_t B2DependsOn;

extern ECS_TAG_DECLARE(B2Discovered);
extern ECS_TAG_DECLARE(B2External);
extern ECS_TAG_DECLARE(B2Built);
extern ECS_TAG_DECLARE(B2BuildFailed);
extern ECS_TAG_DECLARE(B2BuildInProgress);

int bake_model_init(ecs_world_t *world);
void bake_model_fini(ecs_world_t *world);

ecs_entity_t bake_model_add_project(ecs_world_t *world, bake_project_cfg_t *cfg, bool external);
const BakeProject* bake_model_get_project(const ecs_world_t *world, ecs_entity_t entity);
const BakeProject* bake_model_find_project(const ecs_world_t *world, const char *id, ecs_entity_t *entity_out);
const BakeProject* bake_model_find_project_by_path(const ecs_world_t *world, const char *path, ecs_entity_t *entity_out);
void bake_model_link_dependencies(ecs_world_t *world);
void bake_model_mark_build_targets(ecs_world_t *world, const char *target, const char *mode, bool recursive, bool standalone);
int bake_model_build_order(const ecs_world_t *world, ecs_entity_t **out_entities, int32_t *out_count);

#endif
