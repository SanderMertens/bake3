#ifndef BAKE2_COMMANDS_H
#define BAKE2_COMMANDS_H

#include "bake2/build.h"

void bake_print_help(void);
int bake_execute(bake_context_t *ctx, const char *argv0);

#endif
