#ifndef BAKE2_TEST_HARNESS_H
#define BAKE2_TEST_HARNESS_H

#include "bake2/discovery.h"

int b2_test_generate_harness(const b2_project_cfg_t *cfg);
int b2_test_run_project(b2_context_t *ctx, const b2_project_cfg_t *cfg, const char *exe_path);

#endif
