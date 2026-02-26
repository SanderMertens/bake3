#ifndef BAKE2_TEST_HARNESS_H
#define BAKE2_TEST_HARNESS_H

#include "bake2/discovery.h"

int bake_test_generate_harness(bake_context_t *ctx, const bake_project_cfg_t *cfg);
int bake_test_generate_builtin_api(
    bake_context_t *ctx,
    const bake_project_cfg_t *cfg,
    const char *gen_dir,
    char **src_out);
int bake_test_run_project(bake_context_t *ctx, const bake_project_cfg_t *cfg, const char *exe_path);

#endif
