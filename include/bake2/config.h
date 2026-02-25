#ifndef BAKE2_CONFIG_H
#define BAKE2_CONFIG_H

#include "bake2/strlist.h"

typedef enum bake_project_kind_t {
    BAKE_PROJECT_APPLICATION = 0,
    BAKE_PROJECT_PACKAGE,
    BAKE_PROJECT_CONFIG,
    BAKE_PROJECT_TEST,
    BAKE_PROJECT_TEMPLATE
} bake_project_kind_t;

typedef enum bake_compiler_kind_t {
    BAKE_COMPILER_GCC = 0,
    BAKE_COMPILER_CLANG,
    BAKE_COMPILER_MSVC,
    BAKE_COMPILER_UNKNOWN
} bake_compiler_kind_t;

typedef struct bake_rule_t {
    char *ext;
    char *command;
} bake_rule_t;

typedef struct bake_rule_list_t {
    bake_rule_t *items;
    int32_t count;
    int32_t capacity;
} bake_rule_list_t;

typedef struct bake_project_cfg_t bake_project_cfg_t;

typedef struct bake_dependee_cfg_t {
    bake_project_cfg_t *cfg;
    char *json;
} bake_dependee_cfg_t;

typedef struct bake_lang_cfg_t {
    bake_strlist_t cflags;
    bake_strlist_t cxxflags;
    bake_strlist_t defines;
    bake_strlist_t ldflags;
    bake_strlist_t libs;
    bake_strlist_t static_libs;
    bake_strlist_t libpaths;
    bake_strlist_t links;
    bake_strlist_t include_paths;
    char *c_standard;
    char *cpp_standard;
    bool static_lib;
    bool export_symbols;
    bool precompile_header;
} bake_lang_cfg_t;

struct bake_project_cfg_t {
    char *id;
    char *path;
    bake_project_kind_t kind;
    bool has_test_spec;
    bool public_project;
    char *language;
    char *output_name;

    bool private_project;
    bool standalone;
    bool amalgamate;
    char *amalgamate_path;

    bake_strlist_t use;
    bake_strlist_t use_private;
    bake_strlist_t use_build;
    bake_strlist_t use_runtime;

    bake_strlist_t drivers;
    bake_strlist_t plugins;

    bake_dependee_cfg_t dependee;
    bake_lang_cfg_t c_lang;
    bake_lang_cfg_t cpp_lang;

    bake_rule_list_t rules;
};

typedef struct bake_build_mode_t {
    const char *id;
    bake_strlist_t cflags;
    bake_strlist_t cxxflags;
    bake_strlist_t ldflags;
} bake_build_mode_t;

const char* bake_project_kind_str(bake_project_kind_t kind);
bake_project_kind_t bake_project_kind_parse(const char *value);

void bake_rule_list_init(bake_rule_list_t *list);
void bake_rule_list_fini(bake_rule_list_t *list);
int bake_rule_list_append(bake_rule_list_t *list, const char *ext, const char *command);

void bake_lang_cfg_init(bake_lang_cfg_t *cfg);
void bake_lang_cfg_fini(bake_lang_cfg_t *cfg);

void bake_dependee_cfg_init(bake_dependee_cfg_t *cfg);
void bake_dependee_cfg_fini(bake_dependee_cfg_t *cfg);

void bake_project_cfg_init(bake_project_cfg_t *cfg);
void bake_project_cfg_fini(bake_project_cfg_t *cfg);
int bake_project_cfg_load_file(const char *project_json_path, bake_project_cfg_t *cfg);
void bake_project_cfg_set_eval_context(const char *mode, const char *target);

void bake_build_mode_init(bake_build_mode_t *mode, const char *id);
void bake_build_mode_fini(bake_build_mode_t *mode);

#endif
