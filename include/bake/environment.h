#ifndef BAKE3_ENVIRONMENT_H
#define BAKE3_ENVIRONMENT_H

#include "bake/model.h"

int bake_env_init_paths(bake_context_t *ctx);
int bake_env_setup(bake_context_t *ctx, const char *argv0);
int bake_env_reset(bake_context_t *ctx);
int bake_env_cleanup(bake_context_t *ctx, int32_t *removed_out);
int bake_env_import_project_by_id(bake_context_t *ctx, const char *id);
int bake_env_import_dependency_closure(bake_context_t *ctx);
int bake_env_resolve_external_dependency_binaries(bake_context_t *ctx);
int bake_env_sync_project(
    bake_context_t *ctx,
    ecs_entity_t project_entity,
    const BakeBuildResult *result,
    const BakeBuildRequest *req,
    bool rebuilt);

bool bake_env_has_required_test_templates(const char *dir, const char **missing_out);
char* bake_env_find_test_template_source(void);
int bake_env_copy_tree_recursive(const char *src, const char *dst);
int bake_env_copy_tree_exact(const char *src, const char *dst);

#endif
