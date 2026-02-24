#ifndef BAKE2_CONTEXT_H
#define BAKE2_CONTEXT_H

#include "bake2/config.h"

typedef struct bake_options_t {
    const char *command;
    const char *target;
    const char *mode;
    const char *cwd;
    const char *cc;
    const char *cxx;
    const char *run_prefix;
    bool recursive;
    bool standalone;
    bool strict;
    bool trace;
    int32_t jobs;
    int run_argc;
    const char **run_argv;
} bake_options_t;

typedef struct bake_context_t {
    ecs_world_t *world;
    bake_options_t opts;
    char *bake_home;
    char *env_path;
    bake_compiler_kind_t compiler_kind;
    int32_t thread_count;
} bake_context_t;

int bake_context_init(bake_context_t *ctx, const bake_options_t *opts);
void bake_context_fini(bake_context_t *ctx);

#endif
