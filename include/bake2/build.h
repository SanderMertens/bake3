#ifndef BAKE2_BUILD_H
#define BAKE2_BUILD_H

#include "bake2/discovery.h"

int b2_build_run(b2_context_t *ctx);
int b2_build_clean(b2_context_t *ctx);
int b2_build_rebuild(b2_context_t *ctx);

#endif
