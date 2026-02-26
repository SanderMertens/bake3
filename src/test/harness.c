#include "bake2/test_harness.h"
#include "bake2/os.h"

#include "parson.h"

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

static char* bake_json_strdup_value(const JSON_Value *value) {
    if (!value) {
        return NULL;
    }

    JSON_Value_Type type = json_value_get_type(value);
    if (type == JSONString) {
        const char *str = json_value_get_string(value);
        return str ? bake_strdup(str) : NULL;
    }

    if (type == JSONBoolean) {
        return bake_strdup(json_value_get_boolean(value) ? "true" : "false");
    }

    if (type == JSONNull) {
        return bake_strdup("null");
    }

    char *serialized = json_serialize_to_string(value);
    if (!serialized) {
        return NULL;
    }

    char *out = bake_strdup(serialized);
    json_free_serialized_string(serialized);
    return out;
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

    JSON_Value *root_value = json_parse_string(json);
    const JSON_Object *root = root_value ? json_value_get_object(root_value) : NULL;
    if (!root) {
        json_value_free(root_value);
        ecs_os_free(json);
        return -1;
    }

    JSON_Array *suites = json_object_get_array(root, "suites");
    if (!suites) {
        json_value_free(root_value);
        ecs_os_free(json);
        return -1;
    }

    size_t suite_count = json_array_get_count(suites);
    for (size_t i = 0; i < suite_count; i++) {
        const JSON_Object *suite_obj = json_array_get_object(suites, i);
        if (!suite_obj) {
            json_value_free(root_value);
            ecs_os_free(json);
            return -1;
        }

        bake_suite_spec_t suite = {0};
        bake_strlist_init(&suite.testcases);

        const char *id = json_object_get_string(suite_obj, "id");
        JSON_Array *tests = json_object_get_array(suite_obj, "testcases");
        if (!id || !tests) {
            bake_strlist_fini(&suite.testcases);
            json_value_free(root_value);
            ecs_os_free(json);
            return -1;
        }

        suite.id = bake_strdup(id);
        if (!suite.id) {
            bake_strlist_fini(&suite.testcases);
            json_value_free(root_value);
            ecs_os_free(json);
            return -1;
        }

        suite.setup = json_object_get_boolean(suite_obj, "setup") == 1;
        suite.teardown = json_object_get_boolean(suite_obj, "teardown") == 1;

        size_t test_count = json_array_get_count(tests);
        for (size_t t = 0; t < test_count; t++) {
            JSON_Value *test_value = json_array_get_value(tests, t);
            char *name = bake_json_strdup_value(test_value);
            if (!name || bake_strlist_append_owned(&suite.testcases, name) != 0) {
                ecs_os_free(name);
                bake_strlist_fini(&suite.testcases);
                ecs_os_free(suite.id);
                json_value_free(root_value);
                ecs_os_free(json);
                return -1;
            }
        }

        if (bake_suite_list_append(out, &suite) != 0) {
            bake_strlist_fini(&suite.testcases);
            ecs_os_free(suite.id);
            json_value_free(root_value);
            ecs_os_free(json);
            return -1;
        }
    }

    json_value_free(root_value);
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

static char* bake_test_template_file(const bake_context_t *ctx, const char *file) {
    if (!ctx || !ctx->bake_home || !ctx->bake_home[0]) {
        ecs_err("cannot resolve test harness template '%s': BAKE_HOME is not initialized", file ? file : "<null>");
        return NULL;
    }

    if (!file || !file[0]) {
        ecs_err("cannot resolve test harness template: invalid file name");
        return NULL;
    }

    char *template_root = bake_join_path(ctx->bake_home, "test");
    if (!template_root) {
        return NULL;
    }

    char *candidate = bake_join_path(template_root, file);
    if (!candidate) {
        ecs_os_free(template_root);
        return NULL;
    }

    if (!bake_path_exists(candidate)) {
        ecs_err(
            "missing test harness template '%s' at '%s'; run 'bake setup' (or 'bake setup --local') to install templates into BAKE_HOME",
            file,
            candidate);
        ecs_os_free(candidate);
        candidate = NULL;
    }

    ecs_os_free(template_root);
    return candidate;
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

static int bake_generate_runtime(const bake_context_t *ctx, const bake_project_cfg_t *cfg) {
    char *hdr = bake_join3_path(cfg->path, "test/generated", "bake_test_runtime.h");
    char *src = bake_join3_path(cfg->path, "test/generated", "bake_test_runtime.c");
    char *tmpl_hdr = bake_test_template_file(ctx, "bake_test_runtime.h");
    char *tmpl_src = bake_test_template_file(ctx, "bake_test_runtime.c");

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

int bake_test_generate_harness(bake_context_t *ctx, const bake_project_cfg_t *cfg) {
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

    int rc = bake_generate_runtime(ctx, cfg);
    if (rc == 0) {
        rc = bake_generate_main(cfg, &suites);
    }

    bake_suite_list_fini(&suites);
    ecs_os_free(tests_json);
    return rc;
}

int bake_test_generate_builtin_api(
    bake_context_t *ctx,
    const bake_project_cfg_t *cfg,
    const char *gen_dir,
    char **src_out)
{
    BAKE_UNUSED(cfg);
    if (!gen_dir || !src_out) {
        return -1;
    }

    char *hdr_path = bake_join_path(gen_dir, "bake_test.h");
    char *src_path = bake_join_path(gen_dir, "bake_test.c");
    char *tmpl_hdr = bake_test_template_file(ctx, "bake_test.h");
    char *tmpl_src = bake_test_template_file(ctx, "bake_test.c");
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
