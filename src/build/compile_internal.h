#ifndef BAKE_COMPILE_INTERNAL_H
#define BAKE_COMPILE_INTERNAL_H

#include "build_internal.h"

typedef struct bake_compile_cmd_ctx_t {
    bake_context_t *ctx;
    const bake_project_cfg_t *cfg;
    const bake_compile_unit_t *unit;
    const bake_lang_cfg_t *lang;
    const bake_strlist_t *mode_flags;
    const bake_strlist_t *dep_includes;
} bake_compile_cmd_ctx_t;

typedef struct bake_link_cmd_ctx_t {
    bake_context_t *ctx;
    const bake_project_cfg_t *cfg;
    const bake_build_paths_t *paths;
    const bake_compile_list_t *units;
    const bake_lang_cfg_t *lang;
    const bake_strlist_t *mode_ldflags;
    const bake_strlist_t *dep_artefacts;
    const bake_strlist_t *dep_libpaths;
    const bake_strlist_t *dep_libs;
    const bake_strlist_t *dep_ldflags;
    const char *artefact;
    bool use_cpp;
} bake_link_cmd_ctx_t;

int bake_compose_compile_command_posix(const bake_compile_cmd_ctx_t *ctx, ecs_strbuf_t *cmd);
int bake_compose_compile_command_msvc(const bake_compile_cmd_ctx_t *ctx, ecs_strbuf_t *cmd);
int bake_compose_link_command_posix(const bake_link_cmd_ctx_t *ctx, ecs_strbuf_t *cmd);
int bake_compose_link_command_msvc(const bake_link_cmd_ctx_t *ctx, ecs_strbuf_t *cmd);

#endif
