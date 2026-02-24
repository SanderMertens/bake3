#ifndef BAKE2_CONFIG_H
#define BAKE2_CONFIG_H

#include "bake2/strlist.h"

typedef enum b2_project_kind_t {
    B2_PROJECT_APPLICATION = 0,
    B2_PROJECT_PACKAGE,
    B2_PROJECT_CONFIG,
    B2_PROJECT_TEST,
    B2_PROJECT_TEMPLATE
} b2_project_kind_t;

typedef enum b2_compiler_kind_t {
    B2_COMPILER_GCC = 0,
    B2_COMPILER_CLANG,
    B2_COMPILER_MSVC,
    B2_COMPILER_UNKNOWN
} b2_compiler_kind_t;

typedef struct b2_rule_t {
    char *ext;
    char *command;
} b2_rule_t;

typedef struct b2_rule_list_t {
    b2_rule_t *items;
    int32_t count;
    int32_t capacity;
} b2_rule_list_t;

typedef struct b2_dependee_cfg_t {
    b2_strlist_t defines;
    b2_strlist_t include_paths;
    b2_strlist_t cflags;
    b2_strlist_t ldflags;
    b2_strlist_t libs;
} b2_dependee_cfg_t;

typedef struct b2_lang_cfg_t {
    b2_strlist_t cflags;
    b2_strlist_t cxxflags;
    b2_strlist_t defines;
    b2_strlist_t ldflags;
    b2_strlist_t libs;
    b2_strlist_t static_libs;
    b2_strlist_t libpaths;
    b2_strlist_t links;
    b2_strlist_t include_paths;
    char *c_standard;
    char *cpp_standard;
    bool static_lib;
    bool export_symbols;
    bool precompile_header;
} b2_lang_cfg_t;

typedef struct b2_project_cfg_t {
    char *id;
    char *path;
    b2_project_kind_t kind;
    bool has_test_spec;
    char *language;
    char *output_name;

    bool private_project;
    bool standalone;

    b2_strlist_t use;
    b2_strlist_t use_private;
    b2_strlist_t use_build;
    b2_strlist_t use_runtime;

    b2_strlist_t drivers;
    b2_strlist_t plugins;

    b2_dependee_cfg_t dependee;
    b2_lang_cfg_t c_lang;
    b2_lang_cfg_t cpp_lang;

    b2_rule_list_t rules;
} b2_project_cfg_t;

typedef struct b2_build_mode_t {
    const char *id;
    b2_strlist_t cflags;
    b2_strlist_t cxxflags;
    b2_strlist_t ldflags;
} b2_build_mode_t;

const char* b2_project_kind_str(b2_project_kind_t kind);
b2_project_kind_t b2_project_kind_parse(const char *value);

void b2_rule_list_init(b2_rule_list_t *list);
void b2_rule_list_fini(b2_rule_list_t *list);
int b2_rule_list_append(b2_rule_list_t *list, const char *ext, const char *command);

void b2_lang_cfg_init(b2_lang_cfg_t *cfg);
void b2_lang_cfg_fini(b2_lang_cfg_t *cfg);

void b2_dependee_cfg_init(b2_dependee_cfg_t *cfg);
void b2_dependee_cfg_fini(b2_dependee_cfg_t *cfg);

void b2_project_cfg_init(b2_project_cfg_t *cfg);
void b2_project_cfg_fini(b2_project_cfg_t *cfg);
int b2_project_cfg_load_file(const char *project_json_path, b2_project_cfg_t *cfg);

void b2_build_mode_init(b2_build_mode_t *mode, const char *id);
void b2_build_mode_fini(b2_build_mode_t *mode);

#endif
