#ifndef BAKE3_BENCH_HARNESS_H
#define BAKE3_BENCH_HARNESS_H

#include "bake/model.h"

int bake_bench_generate_harness(
    bake_context_t *ctx,
    const bake_project_cfg_t *cfg,
    const char *exe_path);
int bake_bench_generate_builtin_api(
    bake_context_t *ctx,
    const bake_project_cfg_t *cfg,
    const char *gen_dir,
    char **src_out);
int bake_bench_run_project(
    bake_context_t *ctx,
    const bake_project_cfg_t *cfg,
    const char *exe_path);

#endif
