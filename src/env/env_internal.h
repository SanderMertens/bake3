#ifndef BAKE3_ENV_INTERNAL_H
#define BAKE3_ENV_INTERNAL_H

#include "bake/environment.h"

bool bake_env_has_required_test_templates(const char *dir, const char **missing_out);
char* bake_env_find_test_template_source(void);
int bake_env_copy_tree_recursive(const char *src, const char *dst);
int bake_env_copy_tree_exact(const char *src, const char *dst);
int bake_env_ensure_local_test_templates(const bake_context_t *ctx);

int bake_env_remove_if_exists(const char *path);

char* bake_env_artefact_path(
    const bake_context_t *ctx,
    const bake_project_cfg_t *cfg,
    const char *mode);
char* bake_env_artefact_path_scoped(
    const bake_context_t *ctx,
    const bake_project_cfg_t *cfg,
    const char *mode);
char* bake_env_find_artefact_path_current_mode(
    const bake_context_t *ctx,
    const bake_project_cfg_t *cfg,
    const char *mode);

char* bake_env_resolve_home_path(const char *env_home);

#endif
