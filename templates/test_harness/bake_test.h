#ifndef BAKE_TEST_H
#define BAKE_TEST_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bake_test_case {
    const char *id;
    void (*function)(void);
} bake_test_case;

typedef struct bake_test_param {
    const char *name;
    char **values;
    int32_t value_count;
    int32_t value_cur;
} bake_test_param;

typedef struct bake_test_suite {
    const char *id;
    void (*setup)(void);
    void (*teardown)(void);
    uint32_t testcase_count;
    bake_test_case *testcases;
    uint32_t param_count;
    bake_test_param *params;
    uint32_t assert_count;
} bake_test_suite;

int bake_test_run(const char *test_id, int argc, char *argv[], bake_test_suite *suites, uint32_t suite_count);

void _test_assert(bool cond, const char *cond_str, const char *file, int line);
void _test_int(int64_t v1, int64_t v2, const char *str_v1, const char *str_v2, const char *file, int line);
void _test_uint(uint64_t v1, uint64_t v2, const char *str_v1, const char *str_v2, const char *file, int line);
void _test_bool(bool v1, bool v2, const char *str_v1, const char *str_v2, const char *file, int line);
void _test_flt(double v1, double v2, const char *str_v1, const char *str_v2, const char *file, int line);
void _test_str(const char *v1, const char *v2, const char *str_v1, const char *str_v2, const char *file, int line);
void _test_null(void *v, const char *str_v, const char *file, int line);
void _test_not_null(void *v, const char *str_v, const char *file, int line);
void _test_ptr(const void *v1, const void *v2, const char *str_v1, const char *str_v2, const char *file, int line);

bool _if_test_assert(bool cond, const char *cond_str, const char *file, int line);
bool _if_test_int(int64_t v1, int64_t v2, const char *str_v1, const char *str_v2, const char *file, int line);
bool _if_test_uint(uint64_t v1, uint64_t v2, const char *str_v1, const char *str_v2, const char *file, int line);
bool _if_test_bool(bool v1, bool v2, const char *str_v1, const char *str_v2, const char *file, int line);
bool _if_test_flt(double v1, double v2, const char *str_v1, const char *str_v2, const char *file, int line);
bool _if_test_str(const char *v1, const char *v2, const char *str_v1, const char *str_v2, const char *file, int line);
bool _if_test_null(void *v, const char *str_v, const char *file, int line);
bool _if_test_not_null(void *v, const char *str_v, const char *file, int line);
bool _if_test_ptr(const void *v1, const void *v2, const char *str_v1, const char *str_v2, const char *file, int line);

void test_is_flaky(void);
void test_quarantine(const char *date);
void test_expect_abort(void);
void test_abort(void);
const char* test_param(const char *name);

#define test_assert(cond) _test_assert(cond, #cond, __FILE__, __LINE__)
#define test_bool(v1, v2) _test_bool(v1, v2, #v1, #v2, __FILE__, __LINE__)
#define test_true(v) _test_bool(v, true, #v, "true", __FILE__, __LINE__)
#define test_false(v) _test_bool(v, false, #v, "false", __FILE__, __LINE__)
#define test_int(v1, v2) _test_int(v1, v2, #v1, #v2, __FILE__, __LINE__)
#define test_uint(v1, v2) _test_uint(v1, v2, #v1, #v2, __FILE__, __LINE__)
#define test_flt(v1, v2) _test_flt(v1, v2, #v1, #v2, __FILE__, __LINE__)
#define test_str(v1, v2) _test_str(v1, v2, #v1, #v2, __FILE__, __LINE__)
#define test_null(v) _test_null(v, #v, __FILE__, __LINE__)
#define test_not_null(v) _test_not_null(v, #v, __FILE__, __LINE__)
#define test_ptr(v1, v2) _test_ptr(v1, v2, #v1, #v2, __FILE__, __LINE__)

#define if_test_assert(cond) _if_test_assert(cond, #cond, __FILE__, __LINE__)
#define if_test_bool(v1, v2) _if_test_bool(v1, v2, #v1, #v2, __FILE__, __LINE__)
#define if_test_int(v1, v2) _if_test_int(v1, v2, #v1, #v2, __FILE__, __LINE__)
#define if_test_uint(v1, v2) _if_test_uint(v1, v2, #v1, #v2, __FILE__, __LINE__)
#define if_test_flt(v1, v2) _if_test_flt(v1, v2, #v1, #v2, __FILE__, __LINE__)
#define if_test_str(v1, v2) _if_test_str(v1, v2, #v1, #v2, __FILE__, __LINE__)
#define if_test_null(v) _if_test_null(v, #v, __FILE__, __LINE__)
#define if_test_not_null(v) _if_test_not_null(v, #v, __FILE__, __LINE__)
#define if_test_ptr(v1, v2) _if_test_ptr(v1, v2, #v1, #v2, __FILE__, __LINE__)

#ifdef __cplusplus
}
#endif

#endif
