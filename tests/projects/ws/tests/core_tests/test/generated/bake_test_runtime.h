#ifndef BAKE_TEST_RUNTIME_H
#define BAKE_TEST_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

typedef struct b2_test_case_t {
    const char *id;
    void (*fn)(void);
} b2_test_case_t;

typedef struct b2_test_suite_t {
    const char *id;
    void (*setup)(void);
    void (*teardown)(void);
    int32_t testcase_count;
    b2_test_case_t *testcases;
} b2_test_suite_t;

int b2_test_run(b2_test_suite_t *suites, int32_t suite_count, int32_t workers);
void b2_test_fail(const char *file, int32_t line, const char *expr);

#define test_assert(expr) do { if (!(expr)) { b2_test_fail(__FILE__, __LINE__, #expr); return; } } while (0)

#endif
