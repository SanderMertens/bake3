#ifndef BAKE2_CONTEXT_H
#define BAKE2_CONTEXT_H

#include "bake2/config.h"

typedef struct b2_options_t {
    const char *command;
    const char *target;
    const char *mode;
    const char *cwd;
    const char *cc;
    const char *cxx;
    const char *run_prefix;
    bool recursive;
    bool standalone;
    int run_argc;
    const char **run_argv;
} b2_options_t;

typedef struct b2_context_t {
    ecs_world_t *world;
    b2_options_t opts;
    char *bake_home;
    char *env_path;
    b2_compiler_kind_t compiler_kind;
    int32_t thread_count;
} b2_context_t;

int b2_context_init(b2_context_t *ctx, const b2_options_t *opts);
void b2_context_fini(b2_context_t *ctx);

#endif
