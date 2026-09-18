#ifndef BAKE3_HARNESS_UTIL_H
#define BAKE3_HARNESS_UTIL_H

#include "bake/config.h"

bool bake_harness_char_is_ident(char ch);

bool bake_harness_symbol_chars_valid(const char *name);

bool bake_harness_symbol_valid(const char *name);

int bake_harness_text_has_function(const char *text, const char *function_name);

int bake_harness_suite_has_function(
    const char *text,
    const char *suite,
    const char *suffix);

char* bake_harness_source_path(const bake_project_cfg_t *cfg, const char *base);

char* bake_harness_project_header(const bake_project_cfg_t *cfg);

void bake_harness_append_separator(
    ecs_strbuf_t *out,
    const char *existing,
    bool *appended);

#endif
