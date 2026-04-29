#ifndef BAKE3_BUILD_H
#define BAKE3_BUILD_H

#include "bake/discovery.h"
#include "bake/build_components.h"

int bake_build(bake_context_t *ctx);
int bake_build_clean(bake_context_t *ctx);
int bake_build_rebuild(bake_context_t *ctx);
int bake_build_run(bake_context_t *ctx);

#endif
