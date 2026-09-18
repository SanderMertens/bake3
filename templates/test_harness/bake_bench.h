#ifndef BAKE_BENCH_H
#define BAKE_BENCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define BAKE_BENCH_VERSION "1.0.0"
#define BAKE_BENCH_MAX_COUNTERS (16)

#define BAKE_BENCH_PHASE_INIT (0)
#define BAKE_BENCH_PHASE_WARMUP (1)
#define BAKE_BENCH_PHASE_SAMPLE (2)
#define BAKE_BENCH_PHASE_DONE (3)

typedef struct bake_bench_counter {
    const char *name;
    double value;
} bake_bench_counter;

typedef struct bench_t {
    uint64_t iters_left;
    uint64_t round_iters;
    uint64_t iters_per_sample;
    uint64_t total_iters;
    uint64_t round_start_ns;
    uint64_t run_start_ns;
    uint64_t paused_ns;
    uint64_t pause_start_ns;
    double warmup_estimate_ns;
    double sample_target_ns;
    double warmup_budget_ns;
    double budget_ns;
    double *samples;
    int32_t sample_count;
    int32_t sample_capacity;
    int32_t target_samples;
    int32_t warmup_rounds;
    int32_t phase;
    bool paused;
    bool measuring;
    bool out_of_memory;
    int64_t items;
    bake_bench_counter counters[BAKE_BENCH_MAX_COUNTERS];
    int32_t counter_count;
} bench_t;

typedef struct bake_bench_case {
    const char *id;
    void (*function)(bench_t *b);
} bake_bench_case;

typedef struct bake_bench_suite {
    const char *id;
    void (*setup)(void);
    void (*teardown)(void);
    uint32_t benchcase_count;
    bake_bench_case *benchcases;
} bake_bench_suite;

int bake_bench_run(
    const char *bench_id,
    int argc,
    char *argv[],
    bake_bench_suite *suites,
    uint32_t suite_count);

bool bake_bench_next(bench_t *b);
void bench_pause(bench_t *b);
void bench_resume(bench_t *b);
void bench_counter(bench_t *b, const char *name, double value);
void bench_set_items(bench_t *b, int64_t items);
uint64_t bench_iterations(const bench_t *b);

void bake_bench_keep_bytes(const void *ptr, size_t size);
void bake_bench_clobber_memory(void);

#if defined(_MSC_VER)
#define BAKE_BENCH_INLINE static __forceinline
#else
#define BAKE_BENCH_INLINE static inline
#endif

BAKE_BENCH_INLINE bool bench_iter(bench_t *b) {
    if (b->iters_left) {
        b->iters_left --;
        return true;
    }
    return bake_bench_next(b);
}

#if defined(__GNUC__) || defined(__clang__)
#define bench_keep(value) __asm__ volatile("" : : "r,m"(value) : "memory")
#define bench_clobber() __asm__ volatile("" : : : "memory")
#elif defined(_MSC_VER)
#define bench_keep(value) bake_bench_keep_bytes(&(value), sizeof(value))
#define bench_clobber() _ReadWriteBarrier()
#else
#define bench_keep(value) bake_bench_keep_bytes(&(value), sizeof(value))
#define bench_clobber() bake_bench_clobber_memory()
#endif

#define bench_do_not_optimize(value) bench_keep(value)

#ifdef __cplusplus
}
#endif

#endif
