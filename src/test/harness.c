#include "bake2/test_harness.h"
#include "bake2/os.h"

#include "../config/jsmn.h"

typedef struct bake_suite_spec_t {
    char *id;
    bool setup;
    bool teardown;
    bake_strlist_t testcases;
} bake_suite_spec_t;

typedef struct bake_suite_list_t {
    bake_suite_spec_t *items;
    int32_t count;
    int32_t capacity;
} bake_suite_list_t;

static int bake_json_skip(const jsmntok_t *toks, int count, int index) {
    int next = index + 1;
    if (index < 0 || index >= count) {
        return next;
    }

    if (toks[index].type == JSMN_OBJECT || toks[index].type == JSMN_ARRAY) {
        for (int i = 0; i < toks[index].size; i++) {
            next = bake_json_skip(toks, count, next);
        }
    }

    return next;
}

static int bake_json_eq(const char *json, const jsmntok_t *tok, const char *str) {
    size_t len = strlen(str);
    size_t tok_len = (size_t)(tok->end - tok->start);
    return tok_len == len && strncmp(json + tok->start, str, len) == 0;
}

static char* bake_json_strdup(const char *json, const jsmntok_t *tok) {
    size_t len = (size_t)(tok->end - tok->start);
    char *str = ecs_os_malloc(len + 1);
    if (!str) {
        return NULL;
    }
    memcpy(str, json + tok->start, len);
    str[len] = '\0';
    return str;
}

static int bake_json_find_object_key(const char *json, const jsmntok_t *toks, int count, int object, const char *key) {
    if (object < 0 || object >= count || toks[object].type != JSMN_OBJECT) {
        return -1;
    }

    int i = object + 1;
    int end = bake_json_skip(toks, count, object);
    while (i < end) {
        int key_tok = i;
        int val_tok = i + 1;

        if (toks[key_tok].type == JSMN_STRING && bake_json_eq(json, &toks[key_tok], key)) {
            return val_tok;
        }

        i = bake_json_skip(toks, count, val_tok);
    }

    return -1;
}

static void bake_suite_list_fini(bake_suite_list_t *list) {
    for (int32_t i = 0; i < list->count; i++) {
        bake_suite_spec_t *suite = &list->items[i];
        ecs_os_free(suite->id);
        bake_strlist_fini(&suite->testcases);
    }
    ecs_os_free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int bake_suite_list_append(bake_suite_list_t *list, bake_suite_spec_t *suite) {
    if (list->count == list->capacity) {
        int32_t next = list->capacity ? list->capacity * 2 : 8;
        bake_suite_spec_t *items = ecs_os_realloc_n(list->items, bake_suite_spec_t, next);
        if (!items) {
            return -1;
        }
        list->items = items;
        list->capacity = next;
    }

    list->items[list->count++] = *suite;
    return 0;
}

static int bake_parse_tests_json(const char *path, bake_suite_list_t *out) {
    size_t len = 0;
    char *json = bake_read_file(path, &len);
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

    int suites_tok = bake_json_find_object_key(json, tokens, parsed, 0, "suites");
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

        bake_suite_spec_t suite = {0};
        bake_strlist_init(&suite.testcases);

        int id_tok = bake_json_find_object_key(json, tokens, parsed, i, "id");
        int setup_tok = bake_json_find_object_key(json, tokens, parsed, i, "setup");
        int teardown_tok = bake_json_find_object_key(json, tokens, parsed, i, "teardown");
        int tests_tok = bake_json_find_object_key(json, tokens, parsed, i, "testcases");

        if (id_tok < 0 || tests_tok < 0 || tokens[tests_tok].type != JSMN_ARRAY) {
            bake_strlist_fini(&suite.testcases);
            ecs_os_free(tokens);
            ecs_os_free(json);
            return -1;
        }

        suite.id = bake_json_strdup(json, &tokens[id_tok]);
        suite.setup = setup_tok >= 0 && bake_json_eq(json, &tokens[setup_tok], "true");
        suite.teardown = teardown_tok >= 0 && bake_json_eq(json, &tokens[teardown_tok], "true");

        int t = tests_tok + 1;
        for (int32_t tc = 0; tc < tokens[tests_tok].size; tc++) {
            char *name = bake_json_strdup(json, &tokens[t]);
            if (!name || bake_strlist_append_owned(&suite.testcases, name) != 0) {
                ecs_os_free(name);
                bake_strlist_fini(&suite.testcases);
                ecs_os_free(suite.id);
                ecs_os_free(tokens);
                ecs_os_free(json);
                return -1;
            }
            t = bake_json_skip(tokens, parsed, t);
        }

        if (bake_suite_list_append(out, &suite) != 0) {
            bake_strlist_fini(&suite.testcases);
            ecs_os_free(suite.id);
            ecs_os_free(tokens);
            ecs_os_free(json);
            return -1;
        }

        i = bake_json_skip(tokens, parsed, i);
    }

    ecs_os_free(tokens);
    ecs_os_free(json);
    return 0;
}

static int bake_text_contains_function(const char *text, const char *suite, const char *testcase) {
    char signature[512];
    ecs_os_snprintf(signature, sizeof(signature), "void %s_%s(void)", suite, testcase);
    return strstr(text, signature) != NULL;
}

static char* bake_text_replace(const char *input, const char *needle, const char *replacement) {
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

static char* bake_test_template_file(const char *file) {
    if (!file || !file[0]) {
        return NULL;
    }

    char *candidate = bake_join_path("templates/test_harness", file);
    if (candidate && bake_path_exists(candidate)) {
        return candidate;
    }
    ecs_os_free(candidate);

    const char *exe_path = getenv("BAKE2_EXEC_PATH");
    if (!exe_path || !exe_path[0]) {
        return NULL;
    }

    char *exe_dir = bake_dirname(exe_path);
    if (!exe_dir) {
        return NULL;
    }

    char *root_dir = bake_dirname(exe_dir);
    ecs_os_free(exe_dir);
    if (!root_dir) {
        return NULL;
    }

    char *template_dir = bake_join_path(root_dir, "templates/test_harness");
    ecs_os_free(root_dir);
    if (!template_dir) {
        return NULL;
    }

    candidate = bake_join_path(template_dir, file);
    ecs_os_free(template_dir);
    if (candidate && bake_path_exists(candidate)) {
        return candidate;
    }

    ecs_os_free(candidate);
    return NULL;
}

static int bake_generate_suite_file(const bake_project_cfg_t *cfg, const bake_suite_spec_t *suite) {
    char *test_dir = bake_join_path(cfg->path, "test");
    char *file_name = bake_asprintf("%s_suite.c", suite->id);
    char *suite_file = bake_join_path(test_dir, file_name);
    ecs_os_free(file_name);

    char *existing = NULL;
    if (bake_path_exists(suite_file)) {
        existing = bake_read_file(suite_file, NULL);
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
        char *normalized = bake_text_replace(
            existing,
            "test/generated/bake_test_runtime.h",
            "generated/bake_test_runtime.h");
        if (!normalized) {
            normalized = bake_strdup(existing);
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
        if (existing && bake_text_contains_function(existing, suite->id, testcase)) {
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
    int rc = bake_write_file(suite_file, content);

    ecs_os_free(content);
    ecs_os_free(existing);
    ecs_os_free(suite_file);
    ecs_os_free(test_dir);

    return rc;
}

static int bake_generate_runtime(const bake_project_cfg_t *cfg) {
    char *hdr = bake_join3_path(cfg->path, "test/generated", "bake_test_runtime.h");
    char *src = bake_join3_path(cfg->path, "test/generated", "bake_test_runtime.c");
    char *tmpl_hdr = bake_test_template_file("bake_test_runtime.h");
    char *tmpl_src = bake_test_template_file("bake_test_runtime.c");

    if (!hdr || !src || !tmpl_hdr || !tmpl_src) {
        ecs_os_free(hdr);
        ecs_os_free(src);
        ecs_os_free(tmpl_hdr);
        ecs_os_free(tmpl_src);
        return -1;
    }

    int rc = bake_copy_file(tmpl_hdr, hdr);
    if (rc == 0) {
        rc = bake_copy_file(tmpl_src, src);
    }

    ecs_os_free(hdr);
    ecs_os_free(src);
    ecs_os_free(tmpl_hdr);
    ecs_os_free(tmpl_src);
    return rc;
}

static int bake_generate_main(const bake_project_cfg_t *cfg, const bake_suite_list_t *suites) {
    char *main_path = bake_join3_path(cfg->path, "test/generated", "main.c");

    ecs_strbuf_t out = ECS_STRBUF_INIT;
    ecs_strbuf_appendstr(&out, "#include \"bake_test_runtime.h\"\n");
    ecs_strbuf_appendstr(&out, "#include <stdlib.h>\n\n");

    for (int32_t i = 0; i < suites->count; i++) {
        const bake_suite_spec_t *suite = &suites->items[i];
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
        const bake_suite_spec_t *suite = &suites->items[i];
        ecs_strbuf_append(&out, "static bake_test_case_t %s_cases[] = {\n", suite->id);
        for (int32_t t = 0; t < suite->testcases.count; t++) {
            ecs_strbuf_append(&out, "    {\"%s\", %s_%s},\n", suite->testcases.items[t], suite->id, suite->testcases.items[t]);
        }
        ecs_strbuf_appendstr(&out, "};\n\n");
    }

    ecs_strbuf_appendstr(&out, "static bake_test_suite_t suites[] = {\n");
    for (int32_t i = 0; i < suites->count; i++) {
        const bake_suite_spec_t *suite = &suites->items[i];
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
        "    return bake_test_run(suites, (int)(sizeof(suites) / sizeof(suites[0])), workers);\n"
        "}\n");

    char *content = ecs_strbuf_get(&out);
    int rc = bake_write_file(main_path, content);

    ecs_os_free(content);
    ecs_os_free(main_path);
    return rc;
}

int bake_test_generate_harness(const bake_project_cfg_t *cfg) {
    char *tests_json = bake_join3_path(cfg->path, "test", "tests.json");
    if (!tests_json) {
        return -1;
    }

    if (!bake_path_exists(tests_json)) {
        ecs_os_free(tests_json);
        return 0;
    }

    bake_suite_list_t suites = {0};
    if (bake_parse_tests_json(tests_json, &suites) != 0) {
        ecs_os_free(tests_json);
        bake_suite_list_fini(&suites);
        return -1;
    }

    for (int32_t i = 0; i < suites.count; i++) {
        if (bake_generate_suite_file(cfg, &suites.items[i]) != 0) {
            bake_suite_list_fini(&suites);
            ecs_os_free(tests_json);
            return -1;
        }
    }

    int rc = bake_generate_runtime(cfg);
    if (rc == 0) {
        rc = bake_generate_main(cfg, &suites);
    }

    bake_suite_list_fini(&suites);
    ecs_os_free(tests_json);
    return rc;
}

int bake_test_generate_builtin_api(const bake_project_cfg_t *cfg, const char *gen_dir, char **src_out) {
    BAKE_UNUSED(cfg);
    if (!gen_dir || !src_out) {
        return -1;
    }

    char *hdr_path = bake_join_path(gen_dir, "bake_test.h");
    char *src_path = bake_join_path(gen_dir, "bake_test.c");
    char *tmpl_hdr = bake_test_template_file("bake_test.h");
    char *tmpl_src = bake_test_template_file("bake_test.c");
    if (!hdr_path || !src_path || !tmpl_hdr || !tmpl_src) {
        ecs_os_free(hdr_path);
        ecs_os_free(src_path);
        ecs_os_free(tmpl_hdr);
        ecs_os_free(tmpl_src);
        return -1;
    }

    int rc = bake_copy_file(tmpl_hdr, hdr_path);
    if (rc == 0) {
        rc = bake_copy_file(tmpl_src, src_path);
    }

    if (rc == 0) {
        *src_out = src_path;
    } else {
        ecs_os_free(src_path);
    }

    ecs_os_free(hdr_path);
    ecs_os_free(tmpl_hdr);
    ecs_os_free(tmpl_src);
    return rc;
}

int bake_test_run_project(bake_context_t *ctx, const bake_project_cfg_t *cfg, const char *exe_path) {
    BAKE_UNUSED(cfg);

    char *old_threads = NULL;
    const char *old_env = getenv("BAKE_TEST_THREADS");
    if (old_env) {
        old_threads = bake_strdup(old_env);
    }

    if (ctx && ctx->opts.jobs > 0) {
        char jobs_str[32];
        ecs_os_snprintf(jobs_str, sizeof(jobs_str), "%d", ctx->opts.jobs);
        bake_os_setenv("BAKE_TEST_THREADS", jobs_str);
    }

    ecs_strbuf_t cmd = ECS_STRBUF_INIT;
    if (ctx && ctx->opts.run_prefix) {
        ecs_strbuf_append(&cmd, "%s ", ctx->opts.run_prefix);
    }
    ecs_strbuf_append(&cmd, "\"%s\"", exe_path);
    if (ctx && ctx->opts.jobs > 0) {
        ecs_strbuf_append(&cmd, " -j %d", ctx->opts.jobs);
    }
    for (int i = 0; ctx && i < ctx->opts.run_argc; i++) {
        ecs_strbuf_append(&cmd, " \"%s\"", ctx->opts.run_argv[i]);
    }

    char *cmd_str = ecs_strbuf_get(&cmd);
    int rc = bake_run_command(cmd_str);
    ecs_os_free(cmd_str);

    if (ctx && ctx->opts.jobs > 0) {
        if (old_threads) {
            bake_os_setenv("BAKE_TEST_THREADS", old_threads);
        } else {
            bake_os_unsetenv("BAKE_TEST_THREADS");
        }
    }
    ecs_os_free(old_threads);

    return rc;
}
