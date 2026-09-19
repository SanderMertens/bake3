#include "bake_bench.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#define BAKE_BENCH_DEFAULT_TIMEOUT (600.0)
#define BAKE_BENCH_DEFAULT_SAMPLES (100)
#define BAKE_BENCH_DEFAULT_TIME (1.0)
#define BAKE_BENCH_DEFAULT_SAMPLE_TIME (0.010)
#define BAKE_BENCH_DEFAULT_THRESHOLD (0.05)
#define BAKE_BENCH_MAX_WARMUP_ROUNDS (30)
#define BAKE_BENCH_WARMUP_SCALE_MAX (100)
#define BAKE_BENCH_Z95 (1.959964)

typedef struct bake_bench_stats_t {
    double mean;
    double median;
    double stddev;
    double min;
    double max;
    double p95;
    double p99;
    double ci_low;
    double ci_high;
    int32_t outliers;
    int32_t outliers_severe;
} bake_bench_stats_t;

typedef struct bake_bench_result_t {
    char *suite;
    char *name;
    uint64_t iterations;
    uint64_t total_iters;
    int32_t samples;
    double *sample_ns;
    bake_bench_stats_t stats;
    int64_t items;
    char *counter_names[BAKE_BENCH_MAX_COUNTERS];
    double counter_values[BAKE_BENCH_MAX_COUNTERS];
    int32_t counter_count;
    double time_sec;
    double baseline_median_ns;
    double change;
    bool has_baseline;
    bool regressed;
    bool improved;
} bake_bench_result_t;

typedef struct bake_bench_baseline_t {
    char *suite;
    char *name;
    double median_ns;
} bake_bench_baseline_t;

static const char *g_json_path = NULL;
static const char *g_baseline_path = NULL;
static const char *g_filter = NULL;
static double g_time = BAKE_BENCH_DEFAULT_TIME;
static double g_sample_time = BAKE_BENCH_DEFAULT_SAMPLE_TIME;
static double g_threshold = BAKE_BENCH_DEFAULT_THRESHOLD;
static int32_t g_samples = BAKE_BENCH_DEFAULT_SAMPLES;
static double g_timeout = BAKE_BENCH_DEFAULT_TIMEOUT;
static bool g_fail_on_regression = false;

static bake_bench_result_t *g_results = NULL;
static int32_t g_result_count = 0;
static int32_t g_result_cap = 0;

static bake_bench_baseline_t *g_baseline = NULL;
static int32_t g_baseline_count = 0;

static volatile uint64_t g_bench_sink = 0;

void bake_bench_keep_bytes(const void *ptr, size_t size) {
    const unsigned char *bytes = (const unsigned char*)ptr;
    uint64_t acc = g_bench_sink;
    for (size_t i = 0; i < size; i ++) {
        acc += bytes[i];
    }
    g_bench_sink = acc;
}

void bake_bench_clobber_memory(void) {
    g_bench_sink = g_bench_sink + 1;
}

static uint64_t bake_bench_now_ns(void) {
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&now);
    return (uint64_t)((double)now.QuadPart * 1e9 / (double)freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static char* bake_bench_strdup(const char *str) {
    size_t len = strlen(str ? str : "") + 1;
    char *copy = (char*)malloc(len);
    if (copy) {
        memcpy(copy, str ? str : "", len);
    }
    return copy;
}

static const char* bake_bench_host_os(void) {
#if defined(_WIN32)
    return "Windows";
#elif defined(__EMSCRIPTEN__)
    return "Emscripten";
#elif defined(__APPLE__)
    return "Darwin";
#elif defined(__linux__)
    return "Linux";
#else
    return "unknown";
#endif
}

static const char* bake_bench_host_arch(void) {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#elif defined(__wasm32__)
    return "wasm32";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#else
    return "unknown";
#endif
}

static int bake_bench_cpu_count(void) {
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return (int)info.dwNumberOfProcessors;
#elif defined(_SC_NPROCESSORS_ONLN)
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? (int)count : 1;
#else
    return 1;
#endif
}

static void bake_bench_round_start(bench_t *b, uint64_t iters, uint64_t now) {
    b->round_iters = iters;
    b->iters_left = iters - 1;
    b->paused_ns = 0;
    b->paused = false;
    b->pause_start_ns = 0;
    b->round_start_ns = now;
}

static double bake_bench_round_elapsed(const bench_t *b, uint64_t now) {
    uint64_t paused = b->paused_ns;
    if (b->paused) {
        paused += now - b->pause_start_ns;
    }
    uint64_t total = now - b->round_start_ns;
    if (paused > total) {
        paused = total;
    }
    return (double)(total - paused);
}

static bool bake_bench_sample_append(bench_t *b, double ns_per_iter) {
    if (b->sample_count == b->sample_capacity) {
        int32_t cap = b->sample_capacity ? (b->sample_capacity * 2) : 64;
        double *tmp = (double*)realloc(b->samples, (size_t)cap * sizeof(double));
        if (!tmp) {
            b->out_of_memory = true;
            return false;
        }
        b->samples = tmp;
        b->sample_capacity = cap;
    }
    b->samples[b->sample_count ++] = ns_per_iter;
    return true;
}

static void bake_bench_warmup_done(bench_t *b, double per_iter, uint64_t now) {
    double per = per_iter > 0.0 ? per_iter : 1.0;
    double want = b->sample_target_ns / per;
    uint64_t iters = 1;
    if (want >= 1.0) {
        iters = (uint64_t)(want + 0.5);
    }
    b->iters_per_sample = iters;
    b->phase = BAKE_BENCH_PHASE_SAMPLE;
    b->measuring = true;
    bake_bench_round_start(b, iters, now);
}

static bool bake_bench_warmup_next(bench_t *b, uint64_t now) {
    double elapsed = bake_bench_round_elapsed(b, now);
    double per_iter = elapsed / (double)b->round_iters;
    double previous = b->warmup_estimate_ns;
    bool stable = previous > 0.0 &&
        fabs(per_iter - previous) <= (0.1 * previous);
    bool long_enough = elapsed >= b->sample_target_ns;
    bool out_of_time = (double)(now - b->run_start_ns) >= b->warmup_budget_ns;

    b->warmup_estimate_ns = per_iter;
    b->warmup_rounds ++;

    if ((long_enough && stable) || out_of_time ||
        b->warmup_rounds >= BAKE_BENCH_MAX_WARMUP_ROUNDS)
    {
        bake_bench_warmup_done(b, per_iter, now);
        return true;
    }

    uint64_t next = b->round_iters;
    if (!long_enough) {
        double scale = elapsed > 0.0 ? (b->sample_target_ns / elapsed) : 0.0;
        if (scale < 2.0) {
            scale = 2.0;
        }
        if (scale > BAKE_BENCH_WARMUP_SCALE_MAX) {
            scale = BAKE_BENCH_WARMUP_SCALE_MAX;
        }
        next = (uint64_t)((double)b->round_iters * scale);
        if (next <= b->round_iters) {
            next = b->round_iters + 1;
        }
    }

    bake_bench_round_start(b, next, bake_bench_now_ns());
    return true;
}

static bool bake_bench_sample_next(bench_t *b, uint64_t now) {
    double elapsed = bake_bench_round_elapsed(b, now);
    b->total_iters += b->round_iters;

    if (!bake_bench_sample_append(b, elapsed / (double)b->round_iters)) {
        b->phase = BAKE_BENCH_PHASE_DONE;
        return false;
    }

    bool budget_spent = (double)(now - b->run_start_ns) >= b->budget_ns;
    if (b->sample_count >= b->target_samples || budget_spent) {
        b->phase = BAKE_BENCH_PHASE_DONE;
        return false;
    }

    bake_bench_round_start(b, b->iters_per_sample, bake_bench_now_ns());
    return true;
}

bool bake_bench_next(bench_t *b) {
    uint64_t now = bake_bench_now_ns();

    if (b->phase == BAKE_BENCH_PHASE_INIT) {
        b->run_start_ns = now;
        b->phase = BAKE_BENCH_PHASE_WARMUP;
        bake_bench_round_start(b, 1, bake_bench_now_ns());
        return true;
    }

    if (b->phase == BAKE_BENCH_PHASE_WARMUP) {
        return bake_bench_warmup_next(b, now);
    }

    if (b->phase == BAKE_BENCH_PHASE_SAMPLE) {
        return bake_bench_sample_next(b, now);
    }

    return false;
}

void bench_pause(bench_t *b) {
    if (b->paused) {
        return;
    }
    b->paused = true;
    b->pause_start_ns = bake_bench_now_ns();
}

void bench_resume(bench_t *b) {
    if (!b->paused) {
        return;
    }
    b->paused_ns += bake_bench_now_ns() - b->pause_start_ns;
    b->paused = false;
}

void bench_counter(bench_t *b, const char *name, double value) {
    if (!b->measuring || !name) {
        return;
    }

    for (int32_t i = 0; i < b->counter_count; i ++) {
        if (!strcmp(b->counters[i].name, name)) {
            b->counters[i].value += value;
            return;
        }
    }

    if (b->counter_count == BAKE_BENCH_MAX_COUNTERS) {
        return;
    }

    b->counters[b->counter_count].name = name;
    b->counters[b->counter_count].value = value;
    b->counter_count ++;
}

void bench_set_items(bench_t *b, int64_t items) {
    b->items = items;
}

uint64_t bench_iterations(const bench_t *b) {
    return b->total_iters;
}

static int bake_bench_cmp_double(const void *a, const void *b) {
    double lhs = *(const double*)a;
    double rhs = *(const double*)b;
    if (lhs < rhs) {
        return -1;
    }
    return lhs > rhs ? 1 : 0;
}

static double bake_bench_percentile(const double *sorted, int32_t count, double p) {
    if (count <= 0) {
        return 0.0;
    }
    if (count == 1) {
        return sorted[0];
    }

    double rank = p * (double)(count - 1);
    int32_t lo = (int32_t)rank;
    if (lo < 0) {
        lo = 0;
    }
    if (lo > (count - 1)) {
        lo = count - 1;
    }
    int32_t hi = (lo + 1) < count ? (lo + 1) : (count - 1);
    double frac = rank - (double)lo;
    return sorted[lo] + ((sorted[hi] - sorted[lo]) * frac);
}

static void bake_bench_stats(
    const double *samples,
    int32_t count,
    bake_bench_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    if (count <= 0) {
        return;
    }

    double *sorted = (double*)malloc((size_t)count * sizeof(double));
    if (!sorted) {
        return;
    }
    memcpy(sorted, samples, (size_t)count * sizeof(double));
    qsort(sorted, (size_t)count, sizeof(double), bake_bench_cmp_double);

    double sum = 0.0;
    for (int32_t i = 0; i < count; i ++) {
        sum += sorted[i];
    }
    out->mean = sum / (double)count;

    double variance = 0.0;
    for (int32_t i = 0; i < count; i ++) {
        double delta = sorted[i] - out->mean;
        variance += delta * delta;
    }
    if (count > 1) {
        out->stddev = sqrt(variance / (double)(count - 1));
    }

    out->min = sorted[0];
    out->max = sorted[count - 1];
    out->median = bake_bench_percentile(sorted, count, 0.5);
    out->p95 = bake_bench_percentile(sorted, count, 0.95);
    out->p99 = bake_bench_percentile(sorted, count, 0.99);

    double error = BAKE_BENCH_Z95 * out->stddev / sqrt((double)count);
    out->ci_low = out->mean - error;
    out->ci_high = out->mean + error;
    if (out->ci_low < 0.0) {
        out->ci_low = 0.0;
    }

    double q1 = bake_bench_percentile(sorted, count, 0.25);
    double q3 = bake_bench_percentile(sorted, count, 0.75);
    double iqr = q3 - q1;
    double mild_low = q1 - (1.5 * iqr);
    double mild_high = q3 + (1.5 * iqr);
    double severe_low = q1 - (3.0 * iqr);
    double severe_high = q3 + (3.0 * iqr);

    for (int32_t i = 0; i < count; i ++) {
        if (sorted[i] < mild_low || sorted[i] > mild_high) {
            out->outliers ++;
        }
        if (sorted[i] < severe_low || sorted[i] > severe_high) {
            out->outliers_severe ++;
        }
    }

    free(sorted);
}

static void bake_bench_result_fini(bake_bench_result_t *result) {
    free(result->suite);
    free(result->name);
    free(result->sample_ns);
    for (int32_t i = 0; i < result->counter_count; i ++) {
        free(result->counter_names[i]);
    }
    memset(result, 0, sizeof(*result));
}

static void bake_bench_results_fini(void) {
    for (int32_t i = 0; i < g_result_count; i ++) {
        bake_bench_result_fini(&g_results[i]);
    }
    free(g_results);
    g_results = NULL;
    g_result_count = 0;
    g_result_cap = 0;
}

static bake_bench_result_t* bake_bench_result_append(void) {
    if (g_result_count == g_result_cap) {
        int32_t cap = g_result_cap ? (g_result_cap * 2) : 16;
        bake_bench_result_t *tmp = (bake_bench_result_t*)realloc(
            g_results, (size_t)cap * sizeof(bake_bench_result_t));
        if (!tmp) {
            return NULL;
        }
        g_results = tmp;
        g_result_cap = cap;
    }

    bake_bench_result_t *result = &g_results[g_result_count ++];
    memset(result, 0, sizeof(*result));
    return result;
}

static const char* bake_bench_scan_value(const char *ptr) {
    const char *colon = strchr(ptr, ':');
    if (!colon) {
        return NULL;
    }
    colon ++;
    while (*colon == ' ' || *colon == '\t' || *colon == '\n' || *colon == '\r') {
        colon ++;
    }
    return colon;
}

static char* bake_bench_scan_string(const char *ptr) {
    const char *value = bake_bench_scan_value(ptr);
    if (!value || *value != '"') {
        return NULL;
    }
    value ++;

    const char *end = value;
    while (*end && *end != '"') {
        if (*end == '\\' && end[1]) {
            end += 2;
            continue;
        }
        end ++;
    }

    size_t len = (size_t)(end - value);
    char *out = (char*)malloc(len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, value, len);
    out[len] = '\0';
    return out;
}

static double bake_bench_scan_number(const char *ptr) {
    const char *value = bake_bench_scan_value(ptr);
    if (!value) {
        return -1.0;
    }
    return strtod(value, NULL);
}

static char* bake_bench_file_read(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }

    char *text = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long size = ftell(f);
        if (size >= 0) {
            rewind(f);
            text = (char*)malloc((size_t)size + 1);
            if (text) {
                size_t read = fread(text, 1, (size_t)size, f);
                text[read] = '\0';
            }
        }
    }

    fclose(f);
    return text;
}

static int bake_bench_baseline_load(const char *path) {
    char *text = bake_bench_file_read(path);
    if (!text) {
        printf("failed to read baseline '%s': %s\n", path, strerror(errno));
        return -1;
    }

    int32_t capacity = 16;
    g_baseline = (bake_bench_baseline_t*)malloc(
        (size_t)capacity * sizeof(bake_bench_baseline_t));
    if (!g_baseline) {
        free(text);
        return -1;
    }

    const char *ptr = text;
    while ((ptr = strstr(ptr, "\"suite\"")) != NULL) {
        const char *case_key = strstr(ptr, "\"case\"");
        const char *median_key = strstr(ptr, "\"median_ns\"");
        if (!case_key || !median_key) {
            break;
        }

        char *suite = bake_bench_scan_string(ptr);
        char *name = bake_bench_scan_string(case_key);
        double median = bake_bench_scan_number(median_key);
        ptr = median_key + 1;

        if (!suite || !name || median < 0.0) {
            free(suite);
            free(name);
            continue;
        }

        if (g_baseline_count == capacity) {
            capacity *= 2;
            bake_bench_baseline_t *tmp = (bake_bench_baseline_t*)realloc(
                g_baseline, (size_t)capacity * sizeof(bake_bench_baseline_t));
            if (!tmp) {
                free(suite);
                free(name);
                break;
            }
            g_baseline = tmp;
        }

        g_baseline[g_baseline_count].suite = suite;
        g_baseline[g_baseline_count].name = name;
        g_baseline[g_baseline_count].median_ns = median;
        g_baseline_count ++;
    }

    free(text);
    return 0;
}

static void bake_bench_baseline_fini(void) {
    for (int32_t i = 0; i < g_baseline_count; i ++) {
        free(g_baseline[i].suite);
        free(g_baseline[i].name);
    }
    free(g_baseline);
    g_baseline = NULL;
    g_baseline_count = 0;
}

static const bake_bench_baseline_t* bake_bench_baseline_find(
    const char *suite,
    const char *name)
{
    for (int32_t i = 0; i < g_baseline_count; i ++) {
        if (!strcmp(g_baseline[i].suite, suite) &&
            !strcmp(g_baseline[i].name, name))
        {
            return &g_baseline[i];
        }
    }
    return NULL;
}

static void bake_bench_apply_baseline(bake_bench_result_t *result) {
    const bake_bench_baseline_t *entry =
        bake_bench_baseline_find(result->suite, result->name);
    if (!entry || entry->median_ns <= 0.0) {
        return;
    }

    result->has_baseline = true;
    result->baseline_median_ns = entry->median_ns;
    result->change = (result->stats.median - entry->median_ns) / entry->median_ns;
    if (result->change > g_threshold) {
        result->regressed = true;
    } else if (result->change < -g_threshold) {
        result->improved = true;
    }
}

static void bake_bench_print_ns(double value) {
    if (value < 1000.0) {
        printf("%.3f", value);
    } else if (value < 1000000.0) {
        printf("%.1f", value);
    } else {
        printf("%.0f", value);
    }
}

static void bake_bench_print_result(const bake_bench_result_t *result) {
    char label[256];
    snprintf(label, sizeof(label), "%s.%s", result->suite, result->name);
    printf("%-38s ", label);
    bake_bench_print_ns(result->stats.median);
    printf(" ns/iter  ci95 [");
    bake_bench_print_ns(result->stats.ci_low);
    printf(", ");
    bake_bench_print_ns(result->stats.ci_high);
    printf("]  iters %llu  samples %d  outliers %d",
        (unsigned long long)result->iterations,
        result->samples,
        result->stats.outliers);

    if (result->items > 0 && result->stats.mean > 0.0) {
        double per_sec = (double)result->items * 1e9 / result->stats.mean;
        printf("  %.3f M items/s", per_sec / 1e6);
    }

    if (result->has_baseline) {
        printf("  %+.1f%% vs baseline", result->change * 100.0);
        if (result->regressed) {
            printf(" (regression)");
        } else if (result->improved) {
            printf(" (improvement)");
        }
    }

    printf("\n");

    for (int32_t i = 0; i < result->counter_count; i ++) {
        double total = result->counter_values[i];
        double per_iter = result->total_iters ?
            (total / (double)result->total_iters) : 0.0;
        printf("%-38s   %s: %.6g total, %.6g per iter\n",
            "", result->counter_names[i], total, per_iter);
    }
}

static void bake_bench_json_string(FILE *f, const char *str) {
    fputc('"', f);
    for (const char *p = str ? str : ""; *p; p++) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', f);
            fputc(*p, f);
        } else if ((unsigned char)*p < 0x20) {
            fprintf(f, "\\u%04x", (unsigned char)*p);
        } else {
            fputc(*p, f);
        }
    }
    fputc('"', f);
}

static void bake_bench_write_counters(FILE *f, const bake_bench_result_t *r) {
    fputs("      \"counters\": [", f);
    for (int32_t i = 0; i < r->counter_count; i ++) {
        double per_iter = r->total_iters ?
            (r->counter_values[i] / (double)r->total_iters) : 0.0;
        fputs(i ? ", {\"name\": " : "{\"name\": ", f);
        bake_bench_json_string(f, r->counter_names[i]);
        fprintf(f, ", \"total\": %.6f, \"per_iter\": %.6f}",
            r->counter_values[i], per_iter);
    }
    fputs("],\n", f);
}

static void bake_bench_write_samples(FILE *f, const bake_bench_result_t *r) {
    fputs("      \"sample_ns\": [", f);
    for (int32_t i = 0; i < r->samples; i ++) {
        fprintf(f, "%s%.6f", i ? ", " : "", r->sample_ns[i]);
    }
    fputs("]\n", f);
}

static void bake_bench_write_result(FILE *f, const bake_bench_result_t *r, bool last) {
    const bake_bench_stats_t *s = &r->stats;

    fputs("    {\n", f);
    fputs("      \"suite\": ", f); bake_bench_json_string(f, r->suite); fputs(",\n", f);
    fputs("      \"case\": ", f); bake_bench_json_string(f, r->name); fputs(",\n", f);
    fprintf(f, "      \"iterations\": %llu,\n", (unsigned long long)r->iterations);
    fprintf(f, "      \"samples\": %d,\n", r->samples);
    fprintf(f, "      \"total_iterations\": %llu,\n", (unsigned long long)r->total_iters);
    fprintf(f, "      \"mean_ns\": %.6f,\n", s->mean);
    fprintf(f, "      \"median_ns\": %.6f,\n", s->median);
    fprintf(f, "      \"stddev_ns\": %.6f,\n", s->stddev);
    fprintf(f, "      \"min_ns\": %.6f,\n", s->min);
    fprintf(f, "      \"max_ns\": %.6f,\n", s->max);
    fprintf(f, "      \"p95_ns\": %.6f,\n", s->p95);
    fprintf(f, "      \"p99_ns\": %.6f,\n", s->p99);
    fprintf(f, "      \"ci_low_ns\": %.6f,\n", s->ci_low);
    fprintf(f, "      \"ci_high_ns\": %.6f,\n", s->ci_high);
    fputs("      \"ci_level\": 0.95,\n", f);
    fputs("      \"ci_method\": \"normal-approx-stddev\",\n", f);
    fprintf(f, "      \"outliers\": %d,\n", s->outliers);
    fprintf(f, "      \"outliers_severe\": %d,\n", s->outliers_severe);
    fprintf(f, "      \"items_per_iter\": %lld,\n", (long long)r->items);
    if (r->items > 0 && s->mean > 0.0) {
        fprintf(f, "      \"items_per_sec\": %.6f,\n",
            (double)r->items * 1e9 / s->mean);
    }
    if (r->has_baseline) {
        fprintf(f, "      \"baseline_median_ns\": %.6f,\n", r->baseline_median_ns);
        fprintf(f, "      \"change\": %.6f,\n", r->change);
    }
    fprintf(f, "      \"time_sec\": %.6f,\n", r->time_sec);
    bake_bench_write_counters(f, r);
    bake_bench_write_samples(f, r);
    fprintf(f, "    }%s\n", last ? "" : ",");
}

static int bake_bench_write_json(const char *bench_id, double elapsed) {
    if (!g_json_path) {
        return 0;
    }

    FILE *f = fopen(g_json_path, "w");
    if (!f) {
        printf("failed to open json report '%s': %s\n", g_json_path, strerror(errno));
        return -1;
    }

    time_t now = time(NULL);
    char stamp[64] = {0};
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));

    fputs("{\n", f);
    fputs("  \"project\": ", f); bake_bench_json_string(f, bench_id); fputs(",\n", f);
    fputs("  \"tool\": \"bake3\",\n", f);
    fputs("  \"tool_version\": ", f);
    bake_bench_json_string(f, BAKE_BENCH_VERSION);
    fputs(",\n", f);
    fputs("  \"timestamp\": ", f); bake_bench_json_string(f, stamp); fputs(",\n", f);
    fputs("  \"host\": {\"os\": ", f);
    bake_bench_json_string(f, bake_bench_host_os());
    fputs(", \"arch\": ", f);
    bake_bench_json_string(f, bake_bench_host_arch());
    fprintf(f, ", \"cpu_count\": %d},\n", bake_bench_cpu_count());
    fprintf(f, "  \"samples\": %d,\n", g_samples);
    fprintf(f, "  \"time_budget_sec\": %.6f,\n", g_time);
    fprintf(f, "  \"sample_target_sec\": %.6f,\n", g_sample_time);
    fprintf(f, "  \"cases\": %d,\n", g_result_count);
    fprintf(f, "  \"time_sec\": %.6f,\n", elapsed);
    fputs("  \"benchmarks\": [\n", f);
    for (int32_t i = 0; i < g_result_count; i ++) {
        bake_bench_write_result(f, &g_results[i], (i + 1) == g_result_count);
    }
    fputs("  ]\n}\n", f);
    fclose(f);
    return 0;
}

static bool bake_bench_case_selected(
    const char *suite,
    const char *name,
    const char *suite_filter,
    const char *single)
{
    if (suite_filter && strcmp(suite_filter, suite)) {
        return false;
    }

    char label[256];
    snprintf(label, sizeof(label), "%s.%s", suite, name);

    if (single && strcmp(single, label)) {
        return false;
    }

    if (g_filter && !strstr(label, g_filter)) {
        return false;
    }

    return true;
}

static char g_timeout_msg[256];
static size_t g_timeout_msg_len = 0;

#if defined(_WIN32)
static HANDLE g_timeout_timer = NULL;

static VOID CALLBACK bake_bench_timeout_fired(PVOID arg, BOOLEAN fired) {
    (void)arg;
    (void)fired;
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), g_timeout_msg,
        (DWORD)g_timeout_msg_len, &written, NULL);
    ExitProcess(1);
}
#else
static void bake_bench_timeout_fired(int sig) {
    (void)sig;
    ssize_t written = write(STDOUT_FILENO, g_timeout_msg, g_timeout_msg_len);
    (void)written;
    _exit(1);
}
#endif

/* A benchcase runs in the process that measures it, so the timeout cannot be a
 * thread: an extra thread changes what the case measures. It is a kernel timer
 * that writes its message and ends the run when it fires. */
static void bake_bench_timeout_arm(const char *suite, const char *name) {
    if (g_timeout <= 0) {
        return;
    }

    int len = snprintf(g_timeout_msg, sizeof(g_timeout_msg),
        "TIMEOUT %s.%s (exceeded %g seconds)\n", suite, name, g_timeout);
    g_timeout_msg_len = (len > 0) ? (size_t)len : 0;
    fflush(stdout);

#if defined(_WIN32)
    CreateTimerQueueTimer(&g_timeout_timer, NULL, bake_bench_timeout_fired,
        NULL, (DWORD)(g_timeout * 1000.0), 0, WT_EXECUTEDEFAULT);
#else
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = bake_bench_timeout_fired;
    sigemptyset(&action.sa_mask);
    sigaction(SIGALRM, &action, NULL);

    struct itimerval timer;
    memset(&timer, 0, sizeof(timer));
    timer.it_value.tv_sec = (time_t)g_timeout;
    timer.it_value.tv_usec = (suseconds_t)(
        (g_timeout - (double)(time_t)g_timeout) * 1e6);
    setitimer(ITIMER_REAL, &timer, NULL);
#endif
}

static void bake_bench_timeout_disarm(void) {
    if (g_timeout <= 0) {
        return;
    }

#if defined(_WIN32)
    if (g_timeout_timer) {
        DeleteTimerQueueTimer(NULL, g_timeout_timer, NULL);
        g_timeout_timer = NULL;
    }
#else
    struct itimerval timer;
    memset(&timer, 0, sizeof(timer));
    setitimer(ITIMER_REAL, &timer, NULL);
#endif
}

static int bake_bench_run_case(
    bake_bench_suite *suite,
    bake_bench_case *benchcase)
{
    bench_t b;
    memset(&b, 0, sizeof(b));
    b.phase = BAKE_BENCH_PHASE_INIT;
    b.target_samples = g_samples;
    b.sample_target_ns = g_sample_time * 1e9;
    b.budget_ns = g_time * 1e9;
    b.warmup_budget_ns = b.budget_ns * 0.25;
    if (b.warmup_budget_ns > 1e8) {
        b.warmup_budget_ns = 1e8;
    }
    if (b.warmup_budget_ns < (b.sample_target_ns * 3.0)) {
        b.warmup_budget_ns = b.sample_target_ns * 3.0;
    }

    uint64_t start = bake_bench_now_ns();
    bake_bench_timeout_arm(suite->id, benchcase->id);
    if (suite->setup) {
        suite->setup();
    }
    benchcase->function(&b);
    if (suite->teardown) {
        suite->teardown();
    }
    bake_bench_timeout_disarm();
    double elapsed = (double)(bake_bench_now_ns() - start) / 1e9;

    int rc = 0;
    if (b.out_of_memory) {
        printf("%s.%s: out of memory while collecting samples\n",
            suite->id, benchcase->id);
        rc = -1;
    } else if (!b.sample_count) {
        printf("%s.%s: no samples collected (add a 'while (bench_iter(b))' loop)\n",
            suite->id, benchcase->id);
        rc = -1;
    } else {
        bake_bench_result_t *result = bake_bench_result_append();
        if (!result) {
            rc = -1;
        } else {
            result->suite = bake_bench_strdup(suite->id);
            result->name = bake_bench_strdup(benchcase->id);
            result->iterations = b.iters_per_sample;
            result->total_iters = b.total_iters;
            result->samples = b.sample_count;
            result->sample_ns = b.samples;
            result->items = b.items;
            result->time_sec = elapsed;
            b.samples = NULL;
            for (int32_t i = 0; i < b.counter_count; i ++) {
                result->counter_names[i] = bake_bench_strdup(b.counters[i].name);
                result->counter_values[i] = b.counters[i].value;
            }
            result->counter_count = b.counter_count;
            bake_bench_stats(result->sample_ns, result->samples, &result->stats);
            bake_bench_apply_baseline(result);
            bake_bench_print_result(result);
        }
    }

    free(b.samples);
    return rc;
}

static bake_bench_suite* bake_bench_find_suite(
    bake_bench_suite *suites,
    uint32_t suite_count,
    const char *id)
{
    for (uint32_t i = 0; i < suite_count; i ++) {
        if (!strcmp(suites[i].id, id)) {
            return &suites[i];
        }
    }
    return NULL;
}

static void bake_bench_list_cases(bake_bench_suite *suites, uint32_t suite_count) {
    for (uint32_t s = 0; s < suite_count; s ++) {
        for (uint32_t c = 0; c < suites[s].benchcase_count; c ++) {
            printf("%s.%s\n", suites[s].id, suites[s].benchcases[c].id);
        }
    }
}

static void bake_bench_list_suites(bake_bench_suite *suites, uint32_t suite_count) {
    for (uint32_t s = 0; s < suite_count; s ++) {
        printf("%s\n", suites[s].id);
    }
}

static int bake_bench_print_summary(const char *bench_id, double elapsed) {
    int regressions = 0;
    int improvements = 0;
    for (int32_t i = 0; i < g_result_count; i ++) {
        regressions += g_results[i].regressed ? 1 : 0;
        improvements += g_results[i].improved ? 1 : 0;
    }

    printf("-----------------------------\n");
    printf("%s: %d benchmark(s) in %.3fs\n", bench_id, g_result_count, elapsed);

    if (g_baseline_path) {
        printf("baseline %s: %d regression(s), %d improvement(s) beyond %.1f%%\n",
            g_baseline_path, regressions, improvements, g_threshold * 100.0);
        for (int32_t i = 0; i < g_result_count; i ++) {
            if (g_results[i].regressed) {
                printf("REGRESSION %s.%s %+.1f%%\n",
                    g_results[i].suite, g_results[i].name,
                    g_results[i].change * 100.0);
            }
        }
    }

    return regressions;
}

static int bake_bench_parse_args(int argc, char *argv[], const char **single, const char **suite_filter) {
    for (int i = 1; i < argc; i ++) {
        const char *arg = argv[i];
        const char *value = (i + 1) < argc ? argv[i + 1] : NULL;

        if (!strcmp(arg, "--json") || !strcmp(arg, "--baseline") ||
            !strcmp(arg, "--filter") || !strcmp(arg, "--time") ||
            !strcmp(arg, "--sample-time") || !strcmp(arg, "--samples") ||
            !strcmp(arg, "--threshold") || !strcmp(arg, "--timeout") ||
            !strcmp(arg, "-j"))
        {
            if (!value) {
                printf("missing value for %s\n", arg);
                return -1;
            }

            if (!strcmp(arg, "--json")) {
                g_json_path = value;
            } else if (!strcmp(arg, "--baseline")) {
                g_baseline_path = value;
            } else if (!strcmp(arg, "--filter")) {
                g_filter = value;
            } else if (!strcmp(arg, "--time")) {
                g_time = atof(value);
            } else if (!strcmp(arg, "--sample-time")) {
                g_sample_time = atof(value);
            } else if (!strcmp(arg, "--samples")) {
                g_samples = atoi(value);
            } else if (!strcmp(arg, "--threshold")) {
                g_threshold = atof(value);
            } else if (!strcmp(arg, "--timeout")) {
                g_timeout = atof(value);
            }

            i ++;
            continue;
        }

        if (!strcmp(arg, "--fail-on-regression")) {
            g_fail_on_regression = true;
            continue;
        }

        if (arg[0] == '-') {
            printf("unknown option '%s'\n", arg);
            return -1;
        }

        if (strchr(arg, '.')) {
            *single = arg;
        } else {
            *suite_filter = arg;
        }
    }

    if (g_samples < 1) {
        g_samples = 1;
    }
    if (g_time <= 0.0) {
        g_time = BAKE_BENCH_DEFAULT_TIME;
    }
    if (g_sample_time <= 0.0) {
        g_sample_time = BAKE_BENCH_DEFAULT_SAMPLE_TIME;
    }
    if (g_threshold < 0.0) {
        g_threshold = BAKE_BENCH_DEFAULT_THRESHOLD;
    }
    if (g_timeout < 0.0) {
        g_timeout = BAKE_BENCH_DEFAULT_TIMEOUT;
    }

    return 0;
}

int bake_bench_run(
    const char *bench_id,
    int argc,
    char *argv[],
    bake_bench_suite *suites,
    uint32_t suite_count)
{
    if (!bench_id || !bench_id[0]) {
        bench_id = "bench";
    }

    for (int i = 1; i < argc; i ++) {
        if (!strcmp(argv[i], "--list-benches")) {
            bake_bench_list_cases(suites, suite_count);
            return 0;
        }
        if (!strcmp(argv[i], "--list-suites")) {
            bake_bench_list_suites(suites, suite_count);
            return 0;
        }
    }

    const char *single = NULL;
    const char *suite_filter = NULL;
    if (bake_bench_parse_args(argc, argv, &single, &suite_filter) != 0) {
        return -1;
    }

    if (suite_filter && !bake_bench_find_suite(suites, suite_count, suite_filter)) {
        printf("bench suite '%s' not found\n", suite_filter);
        return -1;
    }

    if (g_baseline_path && bake_bench_baseline_load(g_baseline_path) != 0) {
        return -1;
    }

    int rc = 0;
    uint64_t start = bake_bench_now_ns();

    for (uint32_t s = 0; s < suite_count; s ++) {
        bake_bench_suite *suite = &suites[s];
        for (uint32_t c = 0; c < suite->benchcase_count; c ++) {
            bake_bench_case *benchcase = &suite->benchcases[c];
            if (!bake_bench_case_selected(
                suite->id, benchcase->id, suite_filter, single))
            {
                continue;
            }
            if (bake_bench_run_case(suite, benchcase) != 0) {
                rc = -1;
            }
        }
    }

    double elapsed = (double)(bake_bench_now_ns() - start) / 1e9;

    if (!g_result_count) {
        printf("no benchmarks matched\n");
        rc = -1;
    }

    int regressions = bake_bench_print_summary(bench_id, elapsed);

    if (bake_bench_write_json(bench_id, elapsed) != 0) {
        rc = -1;
    }

    if (regressions && g_fail_on_regression) {
        rc = -1;
    }

    bake_bench_results_fini();
    bake_bench_baseline_fini();

    return rc;
}
