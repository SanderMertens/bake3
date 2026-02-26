#ifndef BAKE2_DISCOVERY_H
#define BAKE2_DISCOVERY_H

#include "bake/model.h"

int bake_discover_projects(
    bake_context_t *ctx,
    const char *start_path,
    bool skip_special_dirs);

#endif
