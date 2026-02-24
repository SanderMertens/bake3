#include "bake2/test_harness.h"

#include "../config/jsmn.h"

typedef struct b2_suite_spec_t {
    char *id;
    bool setup;
    bool teardown;
    b2_strlist_t testcases;
} b2_suite_spec_t;

typedef struct b2_suite_list_t {
    b2_suite_spec_t *items;
    int32_t count;
    int32_t capacity;
} b2_suite_list_t;

static int b2_json_skip(const jsmntok_t *toks, int count, int index) {
    int next = index + 1;
    if (index < 0 || index >= count) {
        return next;
    }

    if (toks[index].type == JSMN_OBJECT || toks[index].type == JSMN_ARRAY) {
        for (int i = 0; i < toks[index].size; i++) {
            next = b2_json_skip(toks, count, next);
        }
    }

    return next;
}

static int b2_json_eq(const char *json, const jsmntok_t *tok, const char *str) {
    size_t len = strlen(str);
    size_t tok_len = (size_t)(tok->end - tok->start);
    return tok_len == len && strncmp(json + tok->start, str, len) == 0;
}

static char* b2_json_strdup(const char *json, const jsmntok_t *tok) {
    size_t len = (size_t)(tok->end - tok->start);
    char *str = ecs_os_malloc(len + 1);
    if (!str) {
        return NULL;
    }
    memcpy(str, json + tok->start, len);
    str[len] = '\0';
    return str;
}

static int b2_json_find_object_key(const char *json, const jsmntok_t *toks, int count, int object, const char *key) {
    if (object < 0 || object >= count || toks[object].type != JSMN_OBJECT) {
        return -1;
    }

    int i = object + 1;
    int end = b2_json_skip(toks, count, object);
    while (i < end) {
        int key_tok = i;
        int val_tok = i + 1;

        if (toks[key_tok].type == JSMN_STRING && b2_json_eq(json, &toks[key_tok], key)) {
            return val_tok;
        }

        i = b2_json_skip(toks, count, val_tok);
    }

    return -1;
}

static void b2_suite_list_fini(b2_suite_list_t *list) {
    for (int32_t i = 0; i < list->count; i++) {
        b2_suite_spec_t *suite = &list->items[i];
        ecs_os_free(suite->id);
        b2_strlist_fini(&suite->testcases);
    }
    ecs_os_free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int b2_suite_list_append(b2_suite_list_t *list, b2_suite_spec_t *suite) {
    if (list->count == list->capacity) {
        int32_t next = list->capacity ? list->capacity * 2 : 8;
        b2_suite_spec_t *items = ecs_os_realloc_n(list->items, b2_suite_spec_t, next);
        if (!items) {
            return -1;
        }
        list->items = items;
        list->capacity = next;
    }

    list->items[list->count++] = *suite;
    return 0;
}

static int b2_parse_tests_json(const char *path, b2_suite_list_t *out) {
    size_t len = 0;
    char *json = b2_read_file(path, &len);
    if (!json) {
        return -1;
    }

    int cap = 256;
    jsmntok_t *tokens = NULL;
    int parsed = JSMN_ERROR_NOMEM;

    while (parsed == JSMN_ERROR_NOMEM) {
        cap *= 2;
        ecs_os_free(tokens);
        tokens = ecs_os_calloc_n(jsmntok_t, cap);
        if (!tokens) {
            ecs_os_free(json);
            return -1;
        }

        jsmn_parser p;
        jsmn_init(&p);
        parsed = jsmn_parse(&p, json, len, tokens, (unsigned int)cap);
    }

    if (parsed < 1 || tokens[0].type != JSMN_OBJECT) {
        ecs_os_free(tokens);
        ecs_os_free(json);
        return -1;
    }

    int suites_tok = b2_json_find_object_key(json, tokens, parsed, 0, "suites");
    if (suites_tok < 0 || tokens[suites_tok].type != JSMN_ARRAY) {
        ecs_os_free(tokens);
        ecs_os_free(json);
        return -1;
    }

    int i = suites_tok + 1;
    for (int32_t n = 0; n < tokens[suites_tok].size; n++) {
        if (tokens[i].type != JSMN_OBJECT) {
            ecs_os_free(tokens);
            ecs_os_free(json);
            return -1;
        }

        b2_suite_spec_t suite = {0};
        b2_strlist_init(&suite.testcases);

        int id_tok = b2_json_find_object_key(json, tokens, parsed, i, "id");
        int setup_tok = b2_json_find_object_key(json, tokens, parsed, i, "setup");
        int teardown_tok = b2_json_find_object_key(json, tokens, parsed, i, "teardown");
        int tests_tok = b2_json_find_object_key(json, tokens, parsed, i, "testcases");

        if (id_tok < 0 || tests_tok < 0 || tokens[tests_tok].type != JSMN_ARRAY) {
            b2_strlist_fini(&suite.testcases);
            ecs_os_free(tokens);
            ecs_os_free(json);
            return -1;
        }

        suite.id = b2_json_strdup(json, &tokens[id_tok]);
        suite.setup = setup_tok >= 0 && b2_json_eq(json, &tokens[setup_tok], "true");
        suite.teardown = teardown_tok >= 0 && b2_json_eq(json, &tokens[teardown_tok], "true");

        int t = tests_tok + 1;
        for (int32_t tc = 0; tc < tokens[tests_tok].size; tc++) {
            char *name = b2_json_strdup(json, &tokens[t]);
            if (!name || b2_strlist_append_owned(&suite.testcases, name) != 0) {
                ecs_os_free(name);
                b2_strlist_fini(&suite.testcases);
                ecs_os_free(suite.id);
                ecs_os_free(tokens);
                ecs_os_free(json);
                return -1;
            }
            t = b2_json_skip(tokens, parsed, t);
        }

        if (b2_suite_list_append(out, &suite) != 0) {
            b2_strlist_fini(&suite.testcases);
            ecs_os_free(suite.id);
            ecs_os_free(tokens);
            ecs_os_free(json);
            return -1;
        }

        i = b2_json_skip(tokens, parsed, i);
    }

    ecs_os_free(tokens);
    ecs_os_free(json);
    return 0;
}

static int b2_text_contains_function(const char *text, const char *suite, const char *testcase) {
    char signature[512];
    ecs_os_snprintf(signature, sizeof(signature), "void %s_%s(void)", suite, testcase);
    return strstr(text, signature) != NULL;
}

static char* b2_text_replace(const char *input, const char *needle, const char *replacement) {
    size_t needle_len = strlen(needle);
    size_t repl_len = strlen(replacement);
    size_t total = 1;

    const char *cur = input;
    while (true) {
        const char *hit = strstr(cur, needle);
        if (!hit) {
            total += strlen(cur);
            break;
        }
        total += (size_t)(hit - cur) + repl_len;
        cur = hit + needle_len;
    }

    char *out = ecs_os_malloc(total);
    if (!out) {
        return NULL;
    }
    out[0] = '\0';

    cur = input;
    while (true) {
        const char *hit = strstr(cur, needle);
        if (!hit) {
            strcat(out, cur);
            break;
        }
        strncat(out, cur, (size_t)(hit - cur));
        strcat(out, replacement);
        cur = hit + needle_len;
    }

    return out;
}

static int b2_generate_suite_file(const b2_project_cfg_t *cfg, const b2_suite_spec_t *suite) {
    char *test_dir = b2_join_path(cfg->path, "test");
    char *file_name = b2_asprintf("%s_suite.c", suite->id);
    char *suite_file = b2_join_path(test_dir, file_name);
    ecs_os_free(file_name);

    char *existing = NULL;
    if (b2_path_exists(suite_file)) {
        existing = b2_read_file(suite_file, NULL);
    }

    ecs_strbuf_t out = ECS_STRBUF_INIT;
    if (!existing) {
        ecs_strbuf_appendstr(&out, "#include \"generated/bake_test_runtime.h\"\n\n");
        if (suite->setup) {
            ecs_strbuf_append(&out, "void %s_setup(void) { }\n\n", suite->id);
        }
        if (suite->teardown) {
            ecs_strbuf_append(&out, "void %s_teardown(void) { }\n\n", suite->id);
        }
    } else {
        char *normalized = b2_text_replace(
            existing,
            "test/generated/bake_test_runtime.h",
            "generated/bake_test_runtime.h");
        if (!normalized) {
            normalized = b2_strdup(existing);
        }

        ecs_strbuf_appendstr(&out, normalized);
        size_t existing_len = strlen(existing);
        if (existing_len && existing[existing_len - 1] != '\n') {
            ecs_strbuf_appendstr(&out, "\n");
        }
        ecs_strbuf_appendstr(&out, "\n");
        ecs_os_free(normalized);
    }

    for (int32_t i = 0; i < suite->testcases.count; i++) {
        const char *testcase = suite->testcases.items[i];
        if (existing && b2_text_contains_function(existing, suite->id, testcase)) {
            continue;
        }

        ecs_strbuf_append(&out,
            "void %s_%s(void) {\n"
            "    test_assert(true);\n"
            "}\n\n",
            suite->id,
            testcase);
    }

    char *content = ecs_strbuf_get(&out);
    int rc = b2_write_file(suite_file, content);

    ecs_os_free(content);
    ecs_os_free(existing);
    ecs_os_free(suite_file);
    ecs_os_free(test_dir);

    return rc;
}

static int b2_generate_runtime(const b2_project_cfg_t *cfg) {
    char *hdr = b2_join3_path(cfg->path, "test/generated", "bake_test_runtime.h");
    char *src = b2_join3_path(cfg->path, "test/generated", "bake_test_runtime.c");

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Woverlength-strings"
#endif

    const char *header =
        "#ifndef BAKE_TEST_RUNTIME_H\n"
        "#define BAKE_TEST_RUNTIME_H\n"
        "\n"
        "#include <stdbool.h>\n"
        "#include <stdint.h>\n"
        "\n"
        "typedef struct b2_test_case_t {\n"
        "    const char *id;\n"
        "    void (*fn)(void);\n"
        "} b2_test_case_t;\n"
        "\n"
        "typedef struct b2_test_suite_t {\n"
        "    const char *id;\n"
        "    void (*setup)(void);\n"
        "    void (*teardown)(void);\n"
        "    int32_t testcase_count;\n"
        "    b2_test_case_t *testcases;\n"
        "} b2_test_suite_t;\n"
        "\n"
        "int b2_test_run(b2_test_suite_t *suites, int32_t suite_count, int32_t workers);\n"
        "void b2_test_fail(const char *file, int32_t line, const char *expr);\n"
        "\n"
        "#define test_assert(expr) do { if (!(expr)) { b2_test_fail(__FILE__, __LINE__, #expr); return; } } while (0)\n"
        "\n"
        "#endif\n";

    const char *source =
        "#include \"bake_test_runtime.h\"\n"
        "#include <stdio.h>\n"
        "#include <stdlib.h>\n"
        "\n"
        "#if defined(_WIN32)\n"
        "static int g_fail = 0;\n"
        "\n"
        "void b2_test_fail(const char *file, int32_t line, const char *expr) {\n"
        "    printf(\"FAIL %s:%d %s\\n\", file, line, expr);\n"
        "    g_fail ++;\n"
        "}\n"
        "\n"
        "int b2_test_run(b2_test_suite_t *suites, int32_t suite_count, int32_t workers) {\n"
        "    (void)workers;\n"
        "    g_fail = 0;\n"
        "    for (int32_t s = 0; s < suite_count; s++) {\n"
        "        b2_test_suite_t *suite = &suites[s];\n"
        "        for (int32_t t = 0; t < suite->testcase_count; t++) {\n"
        "            if (suite->setup) { suite->setup(); }\n"
        "            suite->testcases[t].fn();\n"
        "            if (suite->teardown) { suite->teardown(); }\n"
        "            printf(\"PASS %s.%s\\n\", suite->id, suite->testcases[t].id);\n"
        "        }\n"
        "    }\n"
        "    return g_fail ? -1 : 0;\n"
        "}\n"
        "#else\n"
        "#include <pthread.h>\n"
        "typedef struct b2_test_ctx_t {\n"
        "    b2_test_suite_t *suites;\n"
        "    int32_t suite_count;\n"
        "    int32_t total;\n"
        "    int32_t cursor;\n"
        "    int32_t fail;\n"
        "    pthread_mutex_t cursor_lock;\n"
        "    pthread_mutex_t lock;\n"
        "} b2_test_ctx_t;\n"
        "\n"
        "static b2_test_ctx_t *g_ctx;\n"
        "\n"
        "static void b2_test_case_from_index(b2_test_ctx_t *ctx, int32_t index, b2_test_suite_t **suite_out, b2_test_case_t **case_out) {\n"
        "    for (int32_t s = 0; s < ctx->suite_count; s++) {\n"
        "        b2_test_suite_t *suite = &ctx->suites[s];\n"
        "        if (index < suite->testcase_count) {\n"
        "            *suite_out = suite;\n"
        "            *case_out = &suite->testcases[index];\n"
        "            return;\n"
        "        }\n"
        "        index -= suite->testcase_count;\n"
        "    }\n"
        "    *suite_out = NULL;\n"
        "    *case_out = NULL;\n"
        "}\n"
        "\n"
        "void b2_test_fail(const char *file, int32_t line, const char *expr) {\n"
        "    printf(\"FAIL %s:%d %s\\n\", file, line, expr);\n"
        "    if (g_ctx) {\n"
        "        pthread_mutex_lock(&g_ctx->lock);\n"
        "        g_ctx->fail ++;\n"
        "        pthread_mutex_unlock(&g_ctx->lock);\n"
        "    }\n"
        "}\n"
        "\n"
        "static void* b2_test_worker(void *arg) {\n"
        "    b2_test_ctx_t *ctx = arg;\n"
        "    for (;;) {\n"
        "        pthread_mutex_lock(&ctx->cursor_lock);\n"
        "        int32_t index = ctx->cursor ++;\n"
        "        pthread_mutex_unlock(&ctx->cursor_lock);\n"
        "        if (index >= ctx->total) {\n"
        "            break;\n"
        "        }\n"
        "\n"
        "        b2_test_suite_t *suite = NULL;\n"
        "        b2_test_case_t *tc = NULL;\n"
        "        b2_test_case_from_index(ctx, index, &suite, &tc);\n"
        "        if (!suite || !tc) {\n"
        "            continue;\n"
        "        }\n"
        "\n"
        "        if (suite->setup) { suite->setup(); }\n"
        "        tc->fn();\n"
        "        if (suite->teardown) { suite->teardown(); }\n"
        "\n"
        "        pthread_mutex_lock(&ctx->lock);\n"
        "        printf(\"PASS %s.%s\\n\", suite->id, tc->id);\n"
        "        pthread_mutex_unlock(&ctx->lock);\n"
        "    }\n"
        "    return NULL;\n"
        "}\n"
        "\n"
        "int b2_test_run(b2_test_suite_t *suites, int32_t suite_count, int32_t workers) {\n"
        "    b2_test_ctx_t ctx = {0};\n"
        "    ctx.suites = suites;\n"
        "    ctx.suite_count = suite_count;\n"
        "    ctx.total = 0;\n"
        "    for (int32_t i = 0; i < suite_count; i++) {\n"
        "        ctx.total += suites[i].testcase_count;\n"
        "    }\n"
        "\n"
        "    if (workers <= 0) { workers = 1; }\n"
        "    if (workers > ctx.total && ctx.total > 0) { workers = ctx.total; }\n"
        "    if (workers == 0) { workers = 1; }\n"
        "\n"
        "    pthread_mutex_init(&ctx.lock, NULL);\n"
        "    pthread_mutex_init(&ctx.cursor_lock, NULL);\n"
        "    pthread_t *threads = malloc((size_t)workers * sizeof(pthread_t));\n"
        "    if (!threads) {\n"
        "        return -1;\n"
        "    }\n"
        "\n"
        "    g_ctx = &ctx;\n"
        "    for (int32_t i = 0; i < workers; i++) {\n"
        "        pthread_create(&threads[i], NULL, b2_test_worker, &ctx);\n"
        "    }\n"
        "\n"
        "    for (int32_t i = 0; i < workers; i++) {\n"
        "        pthread_join(threads[i], NULL);\n"
        "    }\n"
        "    g_ctx = NULL;\n"
        "\n"
        "    free(threads);\n"
        "    pthread_mutex_destroy(&ctx.cursor_lock);\n"
        "    pthread_mutex_destroy(&ctx.lock);\n"
        "\n"
        "    return ctx.fail ? -1 : 0;\n"
        "}\n"
        "#endif\n";

    int rc = b2_write_file(hdr, header);
    if (rc == 0) {
        rc = b2_write_file(src, source);
    }

    ecs_os_free(hdr);
    ecs_os_free(src);
    return rc;
}

static int b2_generate_main(const b2_project_cfg_t *cfg, const b2_suite_list_t *suites) {
    char *main_path = b2_join3_path(cfg->path, "test/generated", "main.c");

    ecs_strbuf_t out = ECS_STRBUF_INIT;
    ecs_strbuf_appendstr(&out, "#include \"bake_test_runtime.h\"\n");
    ecs_strbuf_appendstr(&out, "#include <stdlib.h>\n\n");

    for (int32_t i = 0; i < suites->count; i++) {
        const b2_suite_spec_t *suite = &suites->items[i];
        if (suite->setup) {
            ecs_strbuf_append(&out, "void %s_setup(void);\n", suite->id);
        }
        if (suite->teardown) {
            ecs_strbuf_append(&out, "void %s_teardown(void);\n", suite->id);
        }
        for (int32_t t = 0; t < suite->testcases.count; t++) {
            ecs_strbuf_append(&out, "void %s_%s(void);\n", suite->id, suite->testcases.items[t]);
        }
        ecs_strbuf_appendstr(&out, "\n");
    }

    for (int32_t i = 0; i < suites->count; i++) {
        const b2_suite_spec_t *suite = &suites->items[i];
        ecs_strbuf_append(&out, "static b2_test_case_t %s_cases[] = {\n", suite->id);
        for (int32_t t = 0; t < suite->testcases.count; t++) {
            ecs_strbuf_append(&out, "    {\"%s\", %s_%s},\n", suite->testcases.items[t], suite->id, suite->testcases.items[t]);
        }
        ecs_strbuf_appendstr(&out, "};\n\n");
    }

    ecs_strbuf_appendstr(&out, "static b2_test_suite_t suites[] = {\n");
    for (int32_t i = 0; i < suites->count; i++) {
        const b2_suite_spec_t *suite = &suites->items[i];
        if (suite->setup && suite->teardown) {
            ecs_strbuf_append(&out,
                "    {\"%s\", %s_setup, %s_teardown, %d, %s_cases},\n",
                suite->id, suite->id, suite->testcases.count, suite->id);
        } else if (suite->setup) {
            ecs_strbuf_append(&out,
                "    {\"%s\", %s_setup, NULL, %d, %s_cases},\n",
                suite->id, suite->id, suite->testcases.count, suite->id);
        } else if (suite->teardown) {
            ecs_strbuf_append(&out,
                "    {\"%s\", NULL, %s_teardown, %d, %s_cases},\n",
                suite->id, suite->id, suite->testcases.count, suite->id);
        } else {
            ecs_strbuf_append(&out,
                "    {\"%s\", NULL, NULL, %d, %s_cases},\n",
                suite->id, suite->testcases.count, suite->id);
        }
    }
    ecs_strbuf_appendstr(&out, "};\n\n");

    ecs_strbuf_appendstr(&out,
        "int main(void) {\n"
        "    const char *threads_env = getenv(\"BAKE_TEST_THREADS\");\n"
        "    int workers = threads_env ? atoi(threads_env) : 4;\n"
        "    return b2_test_run(suites, (int)(sizeof(suites) / sizeof(suites[0])), workers);\n"
        "}\n");

    char *content = ecs_strbuf_get(&out);
    int rc = b2_write_file(main_path, content);

    ecs_os_free(content);
    ecs_os_free(main_path);
    return rc;
}

int b2_test_generate_harness(const b2_project_cfg_t *cfg) {
    char *tests_json = b2_join3_path(cfg->path, "test", "tests.json");
    if (!tests_json) {
        return -1;
    }

    if (!b2_path_exists(tests_json)) {
        ecs_os_free(tests_json);
        return 0;
    }

    b2_suite_list_t suites = {0};
    if (b2_parse_tests_json(tests_json, &suites) != 0) {
        ecs_os_free(tests_json);
        b2_suite_list_fini(&suites);
        return -1;
    }

    for (int32_t i = 0; i < suites.count; i++) {
        if (b2_generate_suite_file(cfg, &suites.items[i]) != 0) {
            b2_suite_list_fini(&suites);
            ecs_os_free(tests_json);
            return -1;
        }
    }

    int rc = b2_generate_runtime(cfg);
    if (rc == 0) {
        rc = b2_generate_main(cfg, &suites);
    }

    b2_suite_list_fini(&suites);
    ecs_os_free(tests_json);
    return rc;
}

int b2_test_generate_builtin_api(const b2_project_cfg_t *cfg, const char *gen_dir, char **src_out) {
    B2_UNUSED(cfg);
    if (!gen_dir || !src_out) {
        return -1;
    }

    char *hdr_path = b2_join_path(gen_dir, "bake_test.h");
    char *src_path = b2_join_path(gen_dir, "bake_test.c");
    if (!hdr_path || !src_path) {
        ecs_os_free(hdr_path);
        ecs_os_free(src_path);
        return -1;
    }

    const char *header =
        "#ifndef BAKE_TEST_H\n"
        "#define BAKE_TEST_H\n"
        "\n"
        "#include <stdbool.h>\n"
        "#include <stdint.h>\n"
        "\n"
        "#ifdef __cplusplus\n"
        "extern \"C\" {\n"
        "#endif\n"
        "\n"
        "typedef struct bake_test_case {\n"
        "    const char *id;\n"
        "    void (*function)(void);\n"
        "} bake_test_case;\n"
        "\n"
        "typedef struct bake_test_param {\n"
        "    const char *name;\n"
        "    char **values;\n"
        "    int32_t value_count;\n"
        "    int32_t value_cur;\n"
        "} bake_test_param;\n"
        "\n"
        "typedef struct bake_test_suite {\n"
        "    const char *id;\n"
        "    void (*setup)(void);\n"
        "    void (*teardown)(void);\n"
        "    uint32_t testcase_count;\n"
        "    bake_test_case *testcases;\n"
        "    uint32_t param_count;\n"
        "    bake_test_param *params;\n"
        "    uint32_t assert_count;\n"
        "} bake_test_suite;\n"
        "\n"
        "int bake_test_run(const char *test_id, int argc, char *argv[], bake_test_suite *suites, uint32_t suite_count);\n"
        "\n"
        "void _test_assert(bool cond, const char *cond_str, const char *file, int line);\n"
        "void _test_int(int64_t v1, int64_t v2, const char *str_v1, const char *str_v2, const char *file, int line);\n"
        "void _test_uint(uint64_t v1, uint64_t v2, const char *str_v1, const char *str_v2, const char *file, int line);\n"
        "void _test_bool(bool v1, bool v2, const char *str_v1, const char *str_v2, const char *file, int line);\n"
        "void _test_flt(double v1, double v2, const char *str_v1, const char *str_v2, const char *file, int line);\n"
        "void _test_str(const char *v1, const char *v2, const char *str_v1, const char *str_v2, const char *file, int line);\n"
        "void _test_null(void *v, const char *str_v, const char *file, int line);\n"
        "void _test_not_null(void *v, const char *str_v, const char *file, int line);\n"
        "void _test_ptr(const void *v1, const void *v2, const char *str_v1, const char *str_v2, const char *file, int line);\n"
        "\n"
        "bool _if_test_assert(bool cond, const char *cond_str, const char *file, int line);\n"
        "bool _if_test_int(int64_t v1, int64_t v2, const char *str_v1, const char *str_v2, const char *file, int line);\n"
        "bool _if_test_uint(uint64_t v1, uint64_t v2, const char *str_v1, const char *str_v2, const char *file, int line);\n"
        "bool _if_test_bool(bool v1, bool v2, const char *str_v1, const char *str_v2, const char *file, int line);\n"
        "bool _if_test_flt(double v1, double v2, const char *str_v1, const char *str_v2, const char *file, int line);\n"
        "bool _if_test_str(const char *v1, const char *v2, const char *str_v1, const char *str_v2, const char *file, int line);\n"
        "bool _if_test_null(void *v, const char *str_v, const char *file, int line);\n"
        "bool _if_test_not_null(void *v, const char *str_v, const char *file, int line);\n"
        "bool _if_test_ptr(const void *v1, const void *v2, const char *str_v1, const char *str_v2, const char *file, int line);\n"
        "\n"
        "void test_is_flaky(void);\n"
        "void test_quarantine(const char *date);\n"
        "void test_expect_abort(void);\n"
        "void test_abort(void);\n"
        "const char* test_param(const char *name);\n"
        "\n"
        "#define test_assert(cond) _test_assert(cond, #cond, __FILE__, __LINE__)\n"
        "#define test_bool(v1, v2) _test_bool(v1, v2, #v1, #v2, __FILE__, __LINE__)\n"
        "#define test_true(v) _test_bool(v, true, #v, \"true\", __FILE__, __LINE__)\n"
        "#define test_false(v) _test_bool(v, false, #v, \"false\", __FILE__, __LINE__)\n"
        "#define test_int(v1, v2) _test_int(v1, v2, #v1, #v2, __FILE__, __LINE__)\n"
        "#define test_uint(v1, v2) _test_uint(v1, v2, #v1, #v2, __FILE__, __LINE__)\n"
        "#define test_flt(v1, v2) _test_flt(v1, v2, #v1, #v2, __FILE__, __LINE__)\n"
        "#define test_str(v1, v2) _test_str(v1, v2, #v1, #v2, __FILE__, __LINE__)\n"
        "#define test_null(v) _test_null(v, #v, __FILE__, __LINE__)\n"
        "#define test_not_null(v) _test_not_null(v, #v, __FILE__, __LINE__)\n"
        "#define test_ptr(v1, v2) _test_ptr(v1, v2, #v1, #v2, __FILE__, __LINE__)\n"
        "\n"
        "#define if_test_assert(cond) _if_test_assert(cond, #cond, __FILE__, __LINE__)\n"
        "#define if_test_bool(v1, v2) _if_test_bool(v1, v2, #v1, #v2, __FILE__, __LINE__)\n"
        "#define if_test_int(v1, v2) _if_test_int(v1, v2, #v1, #v2, __FILE__, __LINE__)\n"
        "#define if_test_uint(v1, v2) _if_test_uint(v1, v2, #v1, #v2, __FILE__, __LINE__)\n"
        "#define if_test_flt(v1, v2) _if_test_flt(v1, v2, #v1, #v2, __FILE__, __LINE__)\n"
        "#define if_test_str(v1, v2) _if_test_str(v1, v2, #v1, #v2, __FILE__, __LINE__)\n"
        "#define if_test_null(v) _if_test_null(v, #v, __FILE__, __LINE__)\n"
        "#define if_test_not_null(v) _if_test_not_null(v, #v, __FILE__, __LINE__)\n"
        "#define if_test_ptr(v1, v2) _if_test_ptr(v1, v2, #v1, #v2, __FILE__, __LINE__)\n"
        "\n"
        "#ifdef __cplusplus\n"
        "}\n"
        "#endif\n"
        "\n"
        "#endif\n";

    const char *source =
        "#include \"bake_test.h\"\n"
        "\n"
        "#include <math.h>\n"
        "#include <signal.h>\n"
        "#include <setjmp.h>\n"
        "#include <stdio.h>\n"
        "#include <stdlib.h>\n"
        "#include <string.h>\n"
        "\n"
        "#define B2_TEST_PARAM_MAX (128)\n"
        "#define B2_JMP_FAIL (1)\n"
        "#define B2_JMP_ABORT (2)\n"
        "#define B2_JMP_QUARANTINE (3)\n"
        "\n"
        "static bake_test_suite *g_current_suite = NULL;\n"
        "static bake_test_case *g_current_case = NULL;\n"
        "static bool g_expect_abort = false;\n"
        "static bool g_observed_abort = false;\n"
        "static bool g_failed = false;\n"
        "static bool g_flaky = false;\n"
        "static const char *g_quarantine_date = NULL;\n"
        "static int g_jmp_active = 0;\n"
        "static jmp_buf g_jmp;\n"
        "static const char *g_cli_params[B2_TEST_PARAM_MAX];\n"
        "static int g_cli_param_count = 0;\n"
        "static void b2_abort_handler(int sig);\n"
        "\n"
        "static void b2_set_failure(const char *file, int line, const char *msg, bool jump) {\n"
        "    g_failed = true;\n"
        "    if (g_current_suite && g_current_case) {\n"
        "        printf(\"FAIL %s.%s:%d: %s\\n\", g_current_suite->id, g_current_case->id, line, msg);\n"
        "    } else {\n"
        "        printf(\"FAIL %s:%d: %s\\n\", file, line, msg);\n"
        "    }\n"
        "    if (jump && g_jmp_active) {\n"
        "        longjmp(g_jmp, B2_JMP_FAIL);\n"
        "    }\n"
        "}\n"
        "\n"
        "static const char* b2_lookup_param(const char *name) {\n"
        "    size_t len = strlen(name);\n"
        "    for (int i = 0; i < g_cli_param_count; i++) {\n"
        "        const char *p = g_cli_params[i];\n"
        "        if (!strncmp(p, name, len) && p[len] == '=') {\n"
        "            return p + len + 1;\n"
        "        }\n"
        "    }\n"
        "\n"
        "    if (g_current_suite) {\n"
        "        for (uint32_t i = 0; i < g_current_suite->param_count; i++) {\n"
        "            bake_test_param *param = &g_current_suite->params[i];\n"
        "            if (!strcmp(param->name, name)) {\n"
        "                if (param->value_cur >= 0 && param->value_cur < param->value_count) {\n"
        "                    return param->values[param->value_cur];\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "\n"
        "    return NULL;\n"
        "}\n"
        "\n"
        "const char* test_param(const char *name) {\n"
        "    return b2_lookup_param(name);\n"
        "}\n"
        "\n"
        "void test_is_flaky(void) {\n"
        "    g_flaky = true;\n"
        "}\n"
        "\n"
        "void test_quarantine(const char *date) {\n"
        "    g_quarantine_date = date ? date : \"unknown\";\n"
        "    if (g_jmp_active) {\n"
        "        longjmp(g_jmp, B2_JMP_QUARANTINE);\n"
        "    }\n"
        "}\n"
        "\n"
        "void test_expect_abort(void) {\n"
        "    g_expect_abort = true;\n"
        "    signal(SIGABRT, b2_abort_handler);\n"
        "}\n"
        "\n"
        "static void b2_abort_handler(int sig) {\n"
        "    (void)sig;\n"
        "    if (g_expect_abort) {\n"
        "        g_observed_abort = true;\n"
        "        if (g_jmp_active) {\n"
        "            longjmp(g_jmp, B2_JMP_ABORT);\n"
        "        }\n"
        "        exit(0);\n"
        "    }\n"
        "\n"
        "    b2_set_failure(__FILE__, __LINE__, \"unexpected abort\", true);\n"
        "    exit(-1);\n"
        "}\n"
        "\n"
        "void test_abort(void) {\n"
        "    b2_abort_handler(SIGABRT);\n"
        "}\n"
        "\n"
        "bool _if_test_assert(bool cond, const char *cond_str, const char *file, int line) {\n"
        "    if (g_current_suite) {\n"
        "        g_current_suite->assert_count ++;\n"
        "    }\n"
        "    if (!cond) {\n"
        "        char msg[512];\n"
        "        snprintf(msg, sizeof(msg), \"assert(%s)\", cond_str);\n"
        "        b2_set_failure(file, line, msg, false);\n"
        "        return false;\n"
        "    }\n"
        "    return true;\n"
        "}\n"
        "\n"
        "bool _if_test_int(int64_t v1, int64_t v2, const char *str_v1, const char *str_v2, const char *file, int line) {\n"
        "    if (g_current_suite) { g_current_suite->assert_count ++; }\n"
        "    if (v1 != v2) {\n"
        "        char msg[512];\n"
        "        snprintf(msg, sizeof(msg), \"%s (%lld) != %s (%lld)\", str_v1, (long long)v1, str_v2, (long long)v2);\n"
        "        b2_set_failure(file, line, msg, false);\n"
        "        return false;\n"
        "    }\n"
        "    return true;\n"
        "}\n"
        "\n"
        "bool _if_test_uint(uint64_t v1, uint64_t v2, const char *str_v1, const char *str_v2, const char *file, int line) {\n"
        "    if (g_current_suite) { g_current_suite->assert_count ++; }\n"
        "    if (v1 != v2) {\n"
        "        char msg[512];\n"
        "        snprintf(msg, sizeof(msg), \"%s (%llu) != %s (%llu)\", str_v1, (unsigned long long)v1, str_v2, (unsigned long long)v2);\n"
        "        b2_set_failure(file, line, msg, false);\n"
        "        return false;\n"
        "    }\n"
        "    return true;\n"
        "}\n"
        "\n"
        "bool _if_test_bool(bool v1, bool v2, const char *str_v1, const char *str_v2, const char *file, int line) {\n"
        "    if (g_current_suite) { g_current_suite->assert_count ++; }\n"
        "    if (v1 != v2) {\n"
        "        char msg[512];\n"
        "        snprintf(msg, sizeof(msg), \"%s (%s) != %s (%s)\", str_v1, v1 ? \"true\" : \"false\", str_v2, v2 ? \"true\" : \"false\");\n"
        "        b2_set_failure(file, line, msg, false);\n"
        "        return false;\n"
        "    }\n"
        "    return true;\n"
        "}\n"
        "\n"
        "bool _if_test_flt(double v1, double v2, const char *str_v1, const char *str_v2, const char *file, int line) {\n"
        "    if (g_current_suite) { g_current_suite->assert_count ++; }\n"
        "    double d = fabs(v1 - v2);\n"
        "    if (d > 0.000001) {\n"
        "        char msg[512];\n"
        "        snprintf(msg, sizeof(msg), \"%s (%f) != %s (%f)\", str_v1, v1, str_v2, v2);\n"
        "        b2_set_failure(file, line, msg, false);\n"
        "        return false;\n"
        "    }\n"
        "    return true;\n"
        "}\n"
        "\n"
        "bool _if_test_str(const char *v1, const char *v2, const char *str_v1, const char *str_v2, const char *file, int line) {\n"
        "    if (g_current_suite) { g_current_suite->assert_count ++; }\n"
        "    bool equal = false;\n"
        "    if (!v1 && !v2) {\n"
        "        equal = true;\n"
        "    } else if (v1 && v2 && !strcmp(v1, v2)) {\n"
        "        equal = true;\n"
        "    }\n"
        "\n"
        "    if (!equal) {\n"
        "        char msg[1024];\n"
        "        snprintf(msg, sizeof(msg), \"%s (%s) != %s (%s)\", str_v1, v1 ? v1 : \"NULL\", str_v2, v2 ? v2 : \"NULL\");\n"
        "        b2_set_failure(file, line, msg, false);\n"
        "        return false;\n"
        "    }\n"
        "\n"
        "    return true;\n"
        "}\n"
        "\n"
        "bool _if_test_null(void *v, const char *str_v, const char *file, int line) {\n"
        "    if (g_current_suite) { g_current_suite->assert_count ++; }\n"
        "    if (v != NULL) {\n"
        "        char msg[512];\n"
        "        snprintf(msg, sizeof(msg), \"%s is not NULL\", str_v);\n"
        "        b2_set_failure(file, line, msg, false);\n"
        "        return false;\n"
        "    }\n"
        "    return true;\n"
        "}\n"
        "\n"
        "bool _if_test_not_null(void *v, const char *str_v, const char *file, int line) {\n"
        "    if (g_current_suite) { g_current_suite->assert_count ++; }\n"
        "    if (v == NULL) {\n"
        "        char msg[512];\n"
        "        snprintf(msg, sizeof(msg), \"%s is NULL\", str_v);\n"
        "        b2_set_failure(file, line, msg, false);\n"
        "        return false;\n"
        "    }\n"
        "    return true;\n"
        "}\n"
        "\n"
        "bool _if_test_ptr(const void *v1, const void *v2, const char *str_v1, const char *str_v2, const char *file, int line) {\n"
        "    if (g_current_suite) { g_current_suite->assert_count ++; }\n"
        "    if (v1 != v2) {\n"
        "        char msg[512];\n"
        "        snprintf(msg, sizeof(msg), \"%s (%p) != %s (%p)\", str_v1, v1, str_v2, v2);\n"
        "        b2_set_failure(file, line, msg, false);\n"
        "        return false;\n"
        "    }\n"
        "    return true;\n"
        "}\n"
        "\n"
        "void _test_assert(bool cond, const char *cond_str, const char *file, int line) {\n"
        "    if (!_if_test_assert(cond, cond_str, file, line) && g_jmp_active) { longjmp(g_jmp, B2_JMP_FAIL); }\n"
        "}\n"
        "void _test_int(int64_t v1, int64_t v2, const char *str_v1, const char *str_v2, const char *file, int line) {\n"
        "    if (!_if_test_int(v1, v2, str_v1, str_v2, file, line) && g_jmp_active) { longjmp(g_jmp, B2_JMP_FAIL); }\n"
        "}\n"
        "void _test_uint(uint64_t v1, uint64_t v2, const char *str_v1, const char *str_v2, const char *file, int line) {\n"
        "    if (!_if_test_uint(v1, v2, str_v1, str_v2, file, line) && g_jmp_active) { longjmp(g_jmp, B2_JMP_FAIL); }\n"
        "}\n"
        "void _test_bool(bool v1, bool v2, const char *str_v1, const char *str_v2, const char *file, int line) {\n"
        "    if (!_if_test_bool(v1, v2, str_v1, str_v2, file, line) && g_jmp_active) { longjmp(g_jmp, B2_JMP_FAIL); }\n"
        "}\n"
        "void _test_flt(double v1, double v2, const char *str_v1, const char *str_v2, const char *file, int line) {\n"
        "    if (!_if_test_flt(v1, v2, str_v1, str_v2, file, line) && g_jmp_active) { longjmp(g_jmp, B2_JMP_FAIL); }\n"
        "}\n"
        "void _test_str(const char *v1, const char *v2, const char *str_v1, const char *str_v2, const char *file, int line) {\n"
        "    if (!_if_test_str(v1, v2, str_v1, str_v2, file, line) && g_jmp_active) { longjmp(g_jmp, B2_JMP_FAIL); }\n"
        "}\n"
        "void _test_null(void *v, const char *str_v, const char *file, int line) {\n"
        "    if (!_if_test_null(v, str_v, file, line) && g_jmp_active) { longjmp(g_jmp, B2_JMP_FAIL); }\n"
        "}\n"
        "void _test_not_null(void *v, const char *str_v, const char *file, int line) {\n"
        "    if (!_if_test_not_null(v, str_v, file, line) && g_jmp_active) { longjmp(g_jmp, B2_JMP_FAIL); }\n"
        "}\n"
        "void _test_ptr(const void *v1, const void *v2, const char *str_v1, const char *str_v2, const char *file, int line) {\n"
        "    if (!_if_test_ptr(v1, v2, str_v1, str_v2, file, line) && g_jmp_active) { longjmp(g_jmp, B2_JMP_FAIL); }\n"
        "}\n"
        "\n"
        "static int b2_run_case(bake_test_suite *suite, bake_test_case *tc) {\n"
        "    g_current_suite = suite;\n"
        "    g_current_case = tc;\n"
        "    suite->assert_count = 0;\n"
        "    g_expect_abort = false;\n"
        "    g_observed_abort = false;\n"
        "    g_failed = false;\n"
        "    g_flaky = false;\n"
        "    g_quarantine_date = NULL;\n"
        "    signal(SIGABRT, b2_abort_handler);\n"
        "\n"
        "    if (suite->setup) {\n"
        "        suite->setup();\n"
        "    }\n"
        "\n"
        "    g_jmp_active = 1;\n"
        "    int jmp_rc = setjmp(g_jmp);\n"
        "    if (jmp_rc == 0) {\n"
        "        tc->function();\n"
        "    }\n"
        "    g_jmp_active = 0;\n"
        "    signal(SIGABRT, SIG_DFL);\n"
        "\n"
        "    if (suite->teardown) {\n"
        "        suite->teardown();\n"
        "    }\n"
        "\n"
        "    if (g_quarantine_date) {\n"
        "        printf(\"QUAR %s.%s (%s)\\n\", suite->id, tc->id, g_quarantine_date);\n"
        "        return 0;\n"
        "    }\n"
        "\n"
        "    if (g_expect_abort && !g_observed_abort) {\n"
        "        b2_set_failure(__FILE__, __LINE__, \"expected abort signal\", false);\n"
        "    }\n"
        "\n"
        "    if (g_failed) {\n"
        "        if (g_flaky) {\n"
        "            printf(\"FLAKY %s.%s\\n\", suite->id, tc->id);\n"
        "            return 0;\n"
        "        }\n"
        "        return -1;\n"
        "    }\n"
        "\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "static const char* b2_lookup_cli_param_only(const char *name) {\n"
        "    size_t len = strlen(name);\n"
        "    for (int i = 0; i < g_cli_param_count; i++) {\n"
        "        const char *p = g_cli_params[i];\n"
        "        if (!strncmp(p, name, len) && p[len] == '=') {\n"
        "            return p + len + 1;\n"
        "        }\n"
        "    }\n"
        "    return NULL;\n"
        "}\n"
        "\n"
        "static int b2_run_suite(const char *exec, bake_test_suite *suite, int *pass, int *fail) {\n"
        "    int rc = 0;\n"
        "    for (uint32_t i = 0; i < suite->testcase_count; i++) {\n"
        "        char cmd[4096];\n"
        "        int written = snprintf(cmd, sizeof(cmd), \"\\\"%s\\\" \\\"%s.%s\\\"\", exec, suite->id, suite->testcases[i].id);\n"
        "        if (written < 0 || (size_t)written >= sizeof(cmd)) {\n"
        "            (*fail) ++;\n"
        "            rc = -1;\n"
        "            continue;\n"
        "        }\n"
        "\n"
        "        for (int p = 0; p < g_cli_param_count; p++) {\n"
        "            size_t used = strlen(cmd);\n"
        "            if ((used + strlen(g_cli_params[p]) + 12) >= sizeof(cmd)) {\n"
        "                break;\n"
        "            }\n"
        "            strcat(cmd, \" --param \");\n"
        "            strcat(cmd, g_cli_params[p]);\n"
        "        }\n"
        "\n"
        "        for (uint32_t p = 0; p < suite->param_count; p++) {\n"
        "            bake_test_param *param = &suite->params[p];\n"
        "            if (b2_lookup_cli_param_only(param->name)) {\n"
        "                continue;\n"
        "            }\n"
        "            if (param->value_cur < 0 || param->value_cur >= param->value_count) {\n"
        "                continue;\n"
        "            }\n"
        "            const char *value = param->values[param->value_cur];\n"
        "            size_t used = strlen(cmd);\n"
        "            if ((used + strlen(param->name) + strlen(value) + 16) >= sizeof(cmd)) {\n"
        "                break;\n"
        "            }\n"
        "            strcat(cmd, \" --param \");\n"
        "            strcat(cmd, param->name);\n"
        "            strcat(cmd, \"=\");\n"
        "            strcat(cmd, value);\n"
        "        }\n"
        "\n"
        "        int test_rc = system(cmd);\n"
        "        if (test_rc == 0) {\n"
        "            (*pass) ++;\n"
        "        } else {\n"
        "            (*fail) ++;\n"
        "            rc = -1;\n"
        "        }\n"
        "    }\n"
        "    return rc;\n"
        "}\n"
        "\n"
        "static int b2_run_suite_for_params(const char *exec, bake_test_suite *suite, uint32_t param, int *pass, int *fail) {\n"
        "    if (!suite->param_count || param >= suite->param_count) {\n"
        "        return b2_run_suite(exec, suite, pass, fail);\n"
        "    }\n"
        "\n"
        "    int rc = 0;\n"
        "    bake_test_param *p = &suite->params[param];\n"
        "    for (int32_t i = 0; i < p->value_count; i++) {\n"
        "        p->value_cur = i;\n"
        "        if (b2_run_suite_for_params(exec, suite, param + 1, pass, fail) != 0) {\n"
        "            rc = -1;\n"
        "        }\n"
        "    }\n"
        "    return rc;\n"
        "}\n"
        "\n"
        "static void b2_list_tests(bake_test_suite *suites, uint32_t suite_count) {\n"
        "    for (uint32_t s = 0; s < suite_count; s++) {\n"
        "        for (uint32_t t = 0; t < suites[s].testcase_count; t++) {\n"
        "            printf(\"%s.%s\\n\", suites[s].id, suites[s].testcases[t].id);\n"
        "        }\n"
        "    }\n"
        "}\n"
        "\n"
        "static void b2_list_suites(bake_test_suite *suites, uint32_t suite_count) {\n"
        "    for (uint32_t s = 0; s < suite_count; s++) {\n"
        "        printf(\"%s\\n\", suites[s].id);\n"
        "    }\n"
        "}\n"
        "\n"
        "static void b2_list_commands(const char *exec, bake_test_suite *suites, uint32_t suite_count) {\n"
        "    for (uint32_t s = 0; s < suite_count; s++) {\n"
        "        for (uint32_t t = 0; t < suites[s].testcase_count; t++) {\n"
        "            printf(\"%s %s.%s\\n\", exec, suites[s].id, suites[s].testcases[t].id);\n"
        "        }\n"
        "    }\n"
        "}\n"
        "\n"
        "static bake_test_suite* b2_find_suite(bake_test_suite *suites, uint32_t suite_count, const char *id) {\n"
        "    for (uint32_t i = 0; i < suite_count; i++) {\n"
        "        if (!strcmp(suites[i].id, id)) {\n"
        "            return &suites[i];\n"
        "        }\n"
        "    }\n"
        "    return NULL;\n"
        "}\n"
        "\n"
        "static int b2_run_single_test(bake_test_suite *suites, uint32_t suite_count, const char *id) {\n"
        "    const char *dot = strchr(id, '.');\n"
        "    if (!dot) {\n"
        "        return -1;\n"
        "    }\n"
        "\n"
        "    char suite_id[256];\n"
        "    size_t suite_len = (size_t)(dot - id);\n"
        "    if (suite_len >= sizeof(suite_id)) {\n"
        "        return -1;\n"
        "    }\n"
        "    memcpy(suite_id, id, suite_len);\n"
        "    suite_id[suite_len] = '\\0';\n"
        "\n"
        "    const char *case_id = dot + 1;\n"
        "    bake_test_suite *suite = b2_find_suite(suites, suite_count, suite_id);\n"
        "    if (!suite) {\n"
        "        printf(\"test suite '%s' not found\\n\", suite_id);\n"
        "        return -1;\n"
        "    }\n"
        "\n"
        "    for (uint32_t i = 0; i < suite->testcase_count; i++) {\n"
        "        if (!strcmp(suite->testcases[i].id, case_id)) {\n"
        "            return b2_run_case(suite, &suite->testcases[i]);\n"
        "        }\n"
        "    }\n"
        "\n"
        "    printf(\"testcase '%s' not found\\n\", id);\n"
        "    return -1;\n"
        "}\n"
        "\n"
        "int bake_test_run(const char *test_id, int argc, char *argv[], bake_test_suite *suites, uint32_t suite_count) {\n"
        "    (void)test_id;\n"
        "    const char *single_test = NULL;\n"
        "    const char *suite_filter = NULL;\n"
        "\n"
        "    g_cli_param_count = 0;\n"
        "\n"
        "    for (int i = 1; i < argc; i++) {\n"
        "        const char *arg = argv[i];\n"
        "        if (!strcmp(arg, \"--list-tests\")) {\n"
        "            b2_list_tests(suites, suite_count);\n"
        "            return 0;\n"
        "        }\n"
        "        if (!strcmp(arg, \"--list-suites\")) {\n"
        "            b2_list_suites(suites, suite_count);\n"
        "            return 0;\n"
        "        }\n"
        "        if (!strcmp(arg, \"--list-commands\")) {\n"
        "            b2_list_commands(argv[0], suites, suite_count);\n"
        "            return 0;\n"
        "        }\n"
        "        if (!strcmp(arg, \"--param\")) {\n"
        "            if ((i + 1) < argc && strchr(argv[i + 1], '=')) {\n"
        "                if (g_cli_param_count < B2_TEST_PARAM_MAX) {\n"
        "                    g_cli_params[g_cli_param_count ++] = argv[i + 1];\n"
        "                }\n"
        "                i ++;\n"
        "                continue;\n"
        "            }\n"
        "            printf(\"invalid --param argument\\n\");\n"
        "            return -1;\n"
        "        }\n"
        "        if (!strcmp(arg, \"-j\")) {\n"
        "            if ((i + 1) < argc) {\n"
        "                i ++;\n"
        "                continue;\n"
        "            }\n"
        "            printf(\"missing value for -j\\n\");\n"
        "            return -1;\n"
        "        }\n"
        "\n"
        "        if (strchr(arg, '.')) {\n"
        "            single_test = arg;\n"
        "        } else {\n"
        "            suite_filter = arg;\n"
        "        }\n"
        "    }\n"
        "\n"
        "    if (single_test) {\n"
        "        return b2_run_single_test(suites, suite_count, single_test);\n"
        "    }\n"
        "\n"
        "    int pass = 0;\n"
        "    int fail = 0;\n"
        "    int rc = 0;\n"
        "\n"
        "    if (suite_filter) {\n"
        "        bake_test_suite *suite = b2_find_suite(suites, suite_count, suite_filter);\n"
        "        if (!suite) {\n"
        "            printf(\"test suite '%s' not found\\n\", suite_filter);\n"
        "            return -1;\n"
        "        }\n"
        "        rc = b2_run_suite_for_params(argv[0], suite, 0, &pass, &fail);\n"
        "    } else {\n"
        "        for (uint32_t s = 0; s < suite_count; s++) {\n"
        "            if (b2_run_suite_for_params(argv[0], &suites[s], 0, &pass, &fail) != 0) {\n"
        "                rc = -1;\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "\n"
        "    printf(\"PASS:%d FAIL:%d\\n\", pass, fail);\n"
        "    return rc;\n"
        "}\n";

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

    int rc = b2_write_file(hdr_path, header);
    if (rc == 0) {
        rc = b2_write_file(src_path, source);
    }

    if (rc == 0) {
        *src_out = src_path;
    } else {
        ecs_os_free(src_path);
    }

    ecs_os_free(hdr_path);
    return rc;
}

int b2_test_run_project(b2_context_t *ctx, const b2_project_cfg_t *cfg, const char *exe_path) {
    B2_UNUSED(ctx);
    B2_UNUSED(cfg);
    return b2_run_command(exe_path);
}
