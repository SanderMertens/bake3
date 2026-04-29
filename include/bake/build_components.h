#ifndef BAKE3_BUILD_COMPONENTS_H
#define BAKE3_BUILD_COMPONENTS_H

#include "bake/model.h"

typedef struct BakeBuildRequest {
    const char *mode;
    bool recursive;
    bool standalone;
} BakeBuildRequest;

typedef struct BakeBuildResult {
    int32_t status;
    char *artefact;
} BakeBuildResult;

extern ECS_COMPONENT_DECLARE(BakeBuildRequest);
extern ECS_COMPONENT_DECLARE(BakeBuildResult);

extern ECS_TAG_DECLARE(BakeBuilt);
extern ECS_TAG_DECLARE(BakeBuildFailed);
extern ECS_TAG_DECLARE(BakeBuildInProgress);

int bake_build_components_init(ecs_world_t *world);

char* bake_project_build_root(const char *project_path, const char *project_id, const char *mode);

#endif
