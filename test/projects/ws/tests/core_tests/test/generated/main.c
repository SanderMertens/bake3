#include "bake_test_runtime.h"
#include <stdlib.h>

void math_add(void);
void math_add_again(void);

static bake_test_case_t math_cases[] = {
    {"add", math_add},
    {"add_again", math_add_again},
};

static bake_test_suite_t suites[] = {
    {"math", NULL, NULL, 2, math_cases},
};

int main(void) {
    const char *threads_env = getenv("BAKE_TEST_THREADS");
    int workers = threads_env ? atoi(threads_env) : 4;
    return bake_test_run(suites, (int)(sizeof(suites) / sizeof(suites[0])), workers);
}
