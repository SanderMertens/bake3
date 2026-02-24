#include "bake_test_runtime.h"
#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
static int g_fail = 0;

void b2_test_fail(const char *file, int32_t line, const char *expr) {
    printf("FAIL %s:%d %s\n", file, line, expr);
    g_fail ++;
}

int b2_test_run(b2_test_suite_t *suites, int32_t suite_count, int32_t workers) {
    (void)workers;
    g_fail = 0;
    for (int32_t s = 0; s < suite_count; s++) {
        b2_test_suite_t *suite = &suites[s];
        for (int32_t t = 0; t < suite->testcase_count; t++) {
            if (suite->setup) { suite->setup(); }
            suite->testcases[t].fn();
            if (suite->teardown) { suite->teardown(); }
            printf("PASS %s.%s\n", suite->id, suite->testcases[t].id);
        }
    }
    return g_fail ? -1 : 0;
}
#else
#include <pthread.h>
typedef struct b2_test_ctx_t {
    b2_test_suite_t *suites;
    int32_t suite_count;
    int32_t total;
    int32_t cursor;
    int32_t fail;
    pthread_mutex_t cursor_lock;
    pthread_mutex_t lock;
} b2_test_ctx_t;

static b2_test_ctx_t *g_ctx;

static void b2_test_case_from_index(b2_test_ctx_t *ctx, int32_t index, b2_test_suite_t **suite_out, b2_test_case_t **case_out) {
    for (int32_t s = 0; s < ctx->suite_count; s++) {
        b2_test_suite_t *suite = &ctx->suites[s];
        if (index < suite->testcase_count) {
            *suite_out = suite;
            *case_out = &suite->testcases[index];
            return;
        }
        index -= suite->testcase_count;
    }
    *suite_out = NULL;
    *case_out = NULL;
}

void b2_test_fail(const char *file, int32_t line, const char *expr) {
    printf("FAIL %s:%d %s\n", file, line, expr);
    if (g_ctx) {
        pthread_mutex_lock(&g_ctx->lock);
        g_ctx->fail ++;
        pthread_mutex_unlock(&g_ctx->lock);
    }
}

static void* b2_test_worker(void *arg) {
    b2_test_ctx_t *ctx = arg;
    for (;;) {
        pthread_mutex_lock(&ctx->cursor_lock);
        int32_t index = ctx->cursor ++;
        pthread_mutex_unlock(&ctx->cursor_lock);
        if (index >= ctx->total) {
            break;
        }

        b2_test_suite_t *suite = NULL;
        b2_test_case_t *tc = NULL;
        b2_test_case_from_index(ctx, index, &suite, &tc);
        if (!suite || !tc) {
            continue;
        }

        if (suite->setup) { suite->setup(); }
        tc->fn();
        if (suite->teardown) { suite->teardown(); }

        pthread_mutex_lock(&ctx->lock);
        printf("PASS %s.%s\n", suite->id, tc->id);
        pthread_mutex_unlock(&ctx->lock);
    }
    return NULL;
}

int b2_test_run(b2_test_suite_t *suites, int32_t suite_count, int32_t workers) {
    b2_test_ctx_t ctx = {0};
    ctx.suites = suites;
    ctx.suite_count = suite_count;
    ctx.total = 0;
    for (int32_t i = 0; i < suite_count; i++) {
        ctx.total += suites[i].testcase_count;
    }

    if (workers <= 0) { workers = 1; }
    if (workers > ctx.total && ctx.total > 0) { workers = ctx.total; }
    if (workers == 0) { workers = 1; }

    pthread_mutex_init(&ctx.lock, NULL);
    pthread_mutex_init(&ctx.cursor_lock, NULL);
    pthread_t *threads = malloc((size_t)workers * sizeof(pthread_t));
    if (!threads) {
        return -1;
    }

    g_ctx = &ctx;
    for (int32_t i = 0; i < workers; i++) {
        pthread_create(&threads[i], NULL, b2_test_worker, &ctx);
    }

    for (int32_t i = 0; i < workers; i++) {
        pthread_join(threads[i], NULL);
    }
    g_ctx = NULL;

    free(threads);
    pthread_mutex_destroy(&ctx.cursor_lock);
    pthread_mutex_destroy(&ctx.lock);

    return ctx.fail ? -1 : 0;
}
#endif
