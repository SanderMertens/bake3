#ifndef BAKE2_ENVIRONMENT_H
#define BAKE2_ENVIRONMENT_H

#include "bake2/model.h"

int bake_environment_init_paths(bake_context_t *ctx);
int bake_environment_setup(bake_context_t *ctx, const char *argv0);
int bake_environment_reset(bake_context_t *ctx);
int bake_environment_cleanup(bake_context_t *ctx, int32_t *removed_out);
int bake_environment_import_project_by_id(bake_context_t *ctx, const char *id);
int bake_environment_import_dependency_closure(bake_context_t *ctx);
int bake_environment_sync_project(
    bake_context_t *ctx,
    ecs_entity_t project_entity,
    const BakeBuildResult *result,
    const BakeBuildRequest *req,
    bool rebuilt);

#endif
