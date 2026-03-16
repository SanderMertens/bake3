#ifndef BAKE3_BUILD_H
#define BAKE3_BUILD_H

#include "bake/discovery.h"

char* bake_project_build_root(const char *project_path, const char *project_id, const char *mode);

int bake_build(bake_context_t *ctx);
int bake_build_clean(bake_context_t *ctx);
int bake_build_rebuild(bake_context_t *ctx);
int bake_build_run(bake_context_t *ctx);

#endif
