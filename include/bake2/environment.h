#ifndef BAKE2_ENVIRONMENT_H
#define BAKE2_ENVIRONMENT_H

#include "bake2/model.h"

int bake_environment_init_paths(bake_context_t *ctx);
int bake_environment_load(bake_context_t *ctx);
int bake_environment_save(bake_context_t *ctx);
int bake_environment_setup(bake_context_t *ctx, const char *argv0);
int bake_environment_reset(bake_context_t *ctx);
int bake_environment_cleanup(bake_context_t *ctx, int32_t *removed_out);
int bake_environment_import_projects(bake_context_t *ctx);
const BakeEnvProject* bake_environment_find_project(const ecs_world_t *world, const char *id, ecs_entity_t *entity_out);

#endif
