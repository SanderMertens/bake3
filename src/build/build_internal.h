#ifndef B2_BUILD_INTERNAL_H
#define B2_BUILD_INTERNAL_H

#include "bake2/build.h"
#include "bake2/plugin.h"
#include "bake2/test_harness.h"

typedef struct bake_compile_unit_t {
    char *src;
    char *obj;
    char *dep;
    bool cpp;
} bake_compile_unit_t;

typedef struct bake_compile_list_t {
    bake_compile_unit_t *items;
    int32_t count;
    int32_t capacity;
} bake_compile_list_t;

typedef struct bake_build_paths_t {
    char *build_root;
    char *obj_dir;
    char *bin_dir;
    char *lib_dir;
    char *gen_dir;
} bake_build_paths_t;

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
    const char *artefact;
    bool use_cpp;
} bake_link_cmd_ctx_t;

void bake_compile_list_init(bake_compile_list_t *list);
void bake_compile_list_fini(bake_compile_list_t *list);
int bake_compile_list_append(
    bake_compile_list_t *list,
    const char *src,
    const char *obj,
    const char *dep,
    bool cpp);

void bake_build_paths_fini(bake_build_paths_t *paths);
int bake_build_paths_init(const bake_project_cfg_t *cfg, const char *mode, bake_build_paths_t *paths);

int bake_collect_compile_units(const bake_project_cfg_t *cfg, const bake_build_paths_t *paths, bool include_tests, bake_compile_list_t *units);
int bake_execute_rules(const bake_project_cfg_t *cfg, const bake_build_paths_t *paths);
int bake_generate_dep_header(ecs_world_t *world, const bake_project_cfg_t *cfg, const bake_build_paths_t *paths);
int bake_apply_dependee_config(ecs_world_t *world, ecs_entity_t project_entity, bake_lang_cfg_t *dst);

bake_compiler_kind_t bake_detect_compiler_kind(const char *cc, const char *cxx);
void bake_add_mode_flags(const char *mode, bake_compiler_kind_t kind, bake_strlist_t *cflags, bake_strlist_t *cxxflags, bake_strlist_t *ldflags);
void bake_add_strict_flags(bool strict, bake_compiler_kind_t kind, bake_strlist_t *cflags, bake_strlist_t *cxxflags, bake_strlist_t *ldflags);
int bake_list_append_fmt(ecs_strbuf_t *buf, const bake_strlist_t *list, const char *prefix, bool quote);
char* bake_project_id_as_macro(const char *id);

int bake_compile_units_parallel(bake_context_t *ctx, const bake_project_cfg_t *cfg, const bake_build_paths_t *paths, const bake_compile_list_t *units, const bake_lang_cfg_t *lang, const bake_lang_cfg_t *cpp_lang, const bake_strlist_t *mode_cflags, const bake_strlist_t *mode_cxxflags);

int bake_link_project_binary(bake_context_t *ctx, ecs_entity_t project_entity, const bake_project_cfg_t *cfg, const bake_build_paths_t *paths, const bake_compile_list_t *units, const bake_lang_cfg_t *lang, const bake_strlist_t *mode_ldflags, char **artefact_out);

int bake_compose_compile_command_posix(const bake_compile_cmd_ctx_t *ctx, ecs_strbuf_t *cmd);
int bake_compose_compile_command_msvc(const bake_compile_cmd_ctx_t *ctx, ecs_strbuf_t *cmd);
int bake_compose_link_command_posix(const bake_link_cmd_ctx_t *ctx, ecs_strbuf_t *cmd);
int bake_compose_link_command_msvc(const bake_link_cmd_ctx_t *ctx, ecs_strbuf_t *cmd);

int bake_amalgamate_project(const bake_project_cfg_t *cfg, const char *dst_dir, char **out_c, char **out_h);
int bake_generate_project_amalgamation(const bake_project_cfg_t *cfg);

#endif
