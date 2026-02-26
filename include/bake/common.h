#ifndef BAKE2_COMMON_H
#define BAKE2_COMMON_H

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <flecs.h>

#define BAKE_UNUSED(x) (void)(x)

int bake_run_command(const char *cmd, bool log_command);
char* bake_project_build_root(const char *project_path, const char *mode);
char* bake_text_replace(const char *input, const char *needle, const char *replacement);

int bake_entity_list_append_unique(
    ecs_entity_t **entities,
    int32_t *count,
    int32_t *capacity,
    ecs_entity_t entity);

#endif
