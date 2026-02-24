#ifndef BAKE2_BUILD_H
#define BAKE2_BUILD_H

#include "bake2/discovery.h"

int bake_build_run(bake_context_t *ctx);
int bake_build_clean(bake_context_t *ctx);
int bake_build_rebuild(bake_context_t *ctx);

#endif
