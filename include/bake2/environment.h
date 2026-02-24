#ifndef BAKE2_ENVIRONMENT_H
#define BAKE2_ENVIRONMENT_H

#include "bake2/model.h"

int b2_environment_init_paths(b2_context_t *ctx);
int b2_environment_load(b2_context_t *ctx);
int b2_environment_save(b2_context_t *ctx);
int b2_environment_setup(b2_context_t *ctx, const char *argv0);

#endif
