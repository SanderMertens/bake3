#ifndef BAKE_TEST_RUNTIME_H
#define BAKE_TEST_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

typedef struct bake_test_case_t {
    const char *id;
    void (*fn)(void);
} bake_test_case_t;

typedef struct bake_test_suite_t {
    const char *id;
    void (*setup)(void);
    void (*teardown)(void);
    int32_t testcase_count;
    bake_test_case_t *testcases;
} bake_test_suite_t;

int bake_test_run(bake_test_suite_t *suites, int32_t suite_count, int32_t workers);
void bake_test_fail(const char *file, int32_t line, const char *expr);

#define test_assert(expr) do { if (!(expr)) { bake_test_fail(__FILE__, __LINE__, #expr); return; } } while (0)

#endif
