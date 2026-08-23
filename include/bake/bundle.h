#ifndef BAKE3_BUNDLE_H
#define BAKE3_BUNDLE_H

#include "bake/context.h"

int bake_bundle_prepare_for_project(bake_context_t *ctx, bake_project_cfg_t *cfg);
bool bake_bundle_is_declared(const bake_project_cfg_t *cfg, const char *id);
char* bake_bundle_cargo_command(
    const bake_bundle_t *bundle,
    const char *src_dir,
    const char *build_dir,
    const char *mode);
char* bake_bundle_cargo_profile_dir(const char *mode);

#endif
