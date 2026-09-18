#ifndef BAKE3_BENCH_INTERNAL_H
#define BAKE3_BENCH_INTERNAL_H

#include "bake/bench_harness.h"

typedef struct bake_benchsuite_spec_t {
    char *id;
    bool setup;
    bool teardown;
    bake_strlist_t benchcases;
} bake_benchsuite_spec_t;

typedef struct bake_benchsuite_list_t {
    bake_benchsuite_spec_t *items;
    int32_t count;
    int32_t capacity;
} bake_benchsuite_list_t;

void bake_benchsuite_list_fini(bake_benchsuite_list_t *list);

int bake_parse_project_benches(const char *path, bake_benchsuite_list_t *out);

int bake_generate_benchsuite_file(
    const bake_project_cfg_t *cfg,
    const bake_benchsuite_spec_t *suite);

int bake_generate_bench_main(
    const bake_project_cfg_t *cfg,
    const bake_benchsuite_list_t *suites);

#endif
