#include "bake/test_harness.h"
#include "bake/os.h"

#include "parson.h"

typedef struct bake_param_spec_t {
    char *name;
    bake_strlist_t values;
} bake_param_spec_t;

typedef struct bake_suite_spec_t {
    char *id;
    bool setup;
    bool teardown;
    bake_strlist_t testcases;
    bake_param_spec_t *params;
    int32_t param_count;
    int32_t param_capacity;
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
        return str ? ecs_os_strdup(str) : NULL;
    }

    if (type == JSONBoolean) {
        return ecs_os_strdup(json_value_get_boolean(value) ? "true" : "false");
    }

    if (type == JSONNull) {
        return ecs_os_strdup("null");
    }

    char *serialized = json_serialize_to_string(value);
    if (!serialized) {
        return NULL;
    }

    char *out = ecs_os_strdup(serialized);
    json_free_serialized_string(serialized);
    return out;
}

static void bake_param_spec_fini(bake_param_spec_t *param) {
    if (!param) {
        return;
    }

    ecs_os_free(param->name);
    bake_strlist_fini(&param->values);
    memset(param, 0, sizeof(*param));
}

static void bake_suite_spec_fini(bake_suite_spec_t *suite) {
    if (!suite) {
        return;
    }

    ecs_os_free(suite->id);
    bake_strlist_fini(&suite->testcases);

    for (int32_t i = 0; i < suite->param_count; i++) {
        bake_param_spec_fini(&suite->params[i]);
    }
    ecs_os_free(suite->params);

    memset(suite, 0, sizeof(*suite));
}

static void bake_suite_list_fini(bake_suite_list_t *list) {
    for (int32_t i = 0; i < list->count; i++) {
        bake_suite_spec_fini(&list->items[i]);
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

static int bake_suite_param_append(bake_suite_spec_t *suite, bake_param_spec_t *param) {
    if (suite->param_count == suite->param_capacity) {
        int32_t next = suite->param_capacity ? suite->param_capacity * 2 : 4;
        bake_param_spec_t *items = ecs_os_realloc_n(suite->params, bake_param_spec_t, next);
        if (!items) {
            return -1;
        }

        suite->params = items;
        suite->param_capacity = next;
    }

    suite->params[suite->param_count++] = *param;
    return 0;
}

static int bake_parse_project_tests(const char *path, bake_suite_list_t *out) {
    char *json = bake_file_read(path, NULL);
    if (!json) {
        return -1;
    }

    JSON_Value *root_value = json_parse_string_with_comments(json);
    const JSON_Object *root = root_value ? json_value_get_object(root_value) : NULL;
    if (!root) {
        json_value_free(root_value);
        ecs_os_free(json);
        return -1;
    }

    const JSON_Object *test_obj = json_object_get_object(root, "test");
    if (!test_obj) {
        json_value_free(root_value);
        ecs_os_free(json);
        return 0;
    }

    JSON_Array *suites = json_object_get_array(test_obj, "testsuites");
    if (!suites) {
        json_value_free(root_value);
        ecs_os_free(json);
        return 0;
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
            bake_suite_spec_fini(&suite);
            json_value_free(root_value);
            ecs_os_free(json);
            return -1;
        }

        suite.id = ecs_os_strdup(id);
        if (!suite.id) {
            bake_suite_spec_fini(&suite);
            json_value_free(root_value);
            ecs_os_free(json);
            return -1;
        }

        suite.setup = json_object_get_boolean(suite_obj, "setup") == 1;
        suite.teardown = json_object_get_boolean(suite_obj, "teardown") == 1;

        size_t testcase_count = json_array_get_count(tests);
        for (size_t t = 0; t < testcase_count; t++) {
            JSON_Value *test_value = json_array_get_value(tests, t);
            char *name = bake_json_strdup_value(test_value);
            if (!name || bake_strlist_append_owned(&suite.testcases, name) != 0) {
                ecs_os_free(name);
                bake_suite_spec_fini(&suite);
                json_value_free(root_value);
                ecs_os_free(json);
                return -1;
            }
        }

        const JSON_Object *params_obj = json_object_get_object(suite_obj, "params");
        if (params_obj) {
            size_t param_count = json_object_get_count(params_obj);
            for (size_t p = 0; p < param_count; p++) {
                const char *param_name = json_object_get_name(params_obj, p);
                JSON_Value *param_value = json_object_get_value_at(params_obj, p);
                if (!param_name || !param_value || json_value_get_type(param_value) != JSONArray) {
                    bake_suite_spec_fini(&suite);
                    json_value_free(root_value);
                    ecs_os_free(json);
                    return -1;
                }

                bake_param_spec_t param = {0};
                bake_strlist_init(&param.values);
                param.name = ecs_os_strdup(param_name);
                if (!param.name) {
                    bake_param_spec_fini(&param);
                    bake_suite_spec_fini(&suite);
                    json_value_free(root_value);
                    ecs_os_free(json);
                    return -1;
                }

                JSON_Array *values = json_value_get_array(param_value);
                size_t value_count = values ? json_array_get_count(values) : 0;
                for (size_t v = 0; v < value_count; v++) {
                    JSON_Value *value = json_array_get_value(values, v);
                    char *value_str = bake_json_strdup_value(value);
                    if (!value_str || bake_strlist_append_owned(&param.values, value_str) != 0) {
                        ecs_os_free(value_str);
                        bake_param_spec_fini(&param);
                        bake_suite_spec_fini(&suite);
                        json_value_free(root_value);
                        ecs_os_free(json);
                        return -1;
                    }
                }

                if (bake_suite_param_append(&suite, &param) != 0) {
                    bake_param_spec_fini(&param);
                    bake_suite_spec_fini(&suite);
                    json_value_free(root_value);
                    ecs_os_free(json);
                    return -1;
                }
            }
        }

        if (bake_suite_list_append(out, &suite) != 0) {
            bake_suite_spec_fini(&suite);
            json_value_free(root_value);
            ecs_os_free(json);
            return -1;
        }
    }

    json_value_free(root_value);
    ecs_os_free(json);
    return 0;
}

static bool bake_char_is_ident(char ch) {
    return (ch >= 'a' && ch <= 'z') ||
        (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') ||
        (ch == '_');
}

static const char* bake_skip_ws_and_comments(const char *ptr) {
    for (;;) {
        while (*ptr == ' ' || *ptr == '\t' || *ptr == '\n' || *ptr == '\r' || *ptr == '\f' || *ptr == '\v') {
            ptr++;
        }

        if (ptr[0] == '/' && ptr[1] == '/') {
            ptr += 2;
            while (*ptr && *ptr != '\n') {
                ptr++;
            }
            continue;
        }

        if (ptr[0] == '/' && ptr[1] == '*') {
            ptr += 2;
            while (*ptr && !(ptr[0] == '*' && ptr[1] == '/')) {
                ptr++;
            }
            if (*ptr) {
                ptr += 2;
            }
            continue;
        }

        return ptr;
    }
}

static int bake_text_contains_function_definition(const char *text, const char *function_name) {
    if (!text || !function_name || !function_name[0]) {
        return 0;
    }

    size_t name_len = strlen(function_name);
    const char *cursor = text;
    while (true) {
        const char *hit = strstr(cursor, function_name);
        if (!hit) {
            return 0;
        }

        if (hit != text && bake_char_is_ident(hit[-1])) {
            cursor = hit + 1;
            continue;
        }

        const char *name_end = hit + name_len;
        if (*name_end && bake_char_is_ident(*name_end)) {
            cursor = hit + 1;
            continue;
        }

        const char *ptr = bake_skip_ws_and_comments(name_end);
        if (*ptr != '(') {
            cursor = hit + 1;
            continue;
        }

        int depth = 1;
        ptr++;
        while (*ptr && depth) {
            if (*ptr == '(') {
                depth++;
            } else if (*ptr == ')') {
                depth--;
            }
            ptr++;
        }

        if (depth != 0) {
            return 0;
        }

        ptr = bake_skip_ws_and_comments(ptr);
        if (*ptr == '{') {
            return 1;
        }

        cursor = hit + 1;
    }
}

static int bake_suite_has_function(const char *text, const char *suite, const char *suffix) {
    char name[768];
    ecs_os_snprintf(name, sizeof(name), "%s_%s", suite, suffix);
    return bake_text_contains_function_definition(text, name);
}

static char* bake_symbol_sanitize(const char *name) {
    if (!name || !name[0]) {
        return ecs_os_strdup("_");
    }

    size_t len = strlen(name);
    char *result = ecs_os_malloc(len + 2);
    if (!result) {
        return NULL;
    }

    size_t out = 0;
    if (name[0] >= '0' && name[0] <= '9') {
        result[out++] = '_';
    }

    for (size_t i = 0; i < len; i++) {
        char ch = name[i];
        if (!bake_char_is_ident(ch)) {
            ch = '_';
        }
        result[out++] = ch;
    }

    result[out] = '\0';
    return result;
}

static char* bake_suite_source_path(const bake_project_cfg_t *cfg, const bake_suite_spec_t *suite) {
    const char *ext = "c";
    if (cfg->language && (!strcmp(cfg->language, "cpp") || !strcmp(cfg->language, "c++"))) {
        ext = "cpp";
    }

    char *file_name = flecs_asprintf("%s.%s", suite->id, ext);
    char *source = file_name ? bake_path_join3(cfg->path, "src", file_name) : NULL;
    ecs_os_free(file_name);
    return source;
}

static char* bake_main_source_path(const bake_project_cfg_t *cfg) {
    const char *ext = "c";
    if (cfg->language && (!strcmp(cfg->language, "cpp") || !strcmp(cfg->language, "c++"))) {
        ext = "cpp";
    }

    char *file_name = flecs_asprintf("main.%s", ext);
    char *source = file_name ? bake_path_join3(cfg->path, "src", file_name) : NULL;
    ecs_os_free(file_name);
    return source;
}

static void bake_append_empty_function(
    ecs_strbuf_t *out,
    const char *suite,
    const char *suffix,
    const char *existing,
    bool *appended)
{
    if (!*appended && existing && existing[0]) {
        size_t len = strlen(existing);
        if (existing[len - 1] != '\n') {
            ecs_strbuf_appendstr(out, "\n");
        }
        ecs_strbuf_appendstr(out, "\n");
    }

    *appended = true;
    ecs_strbuf_append(out,
        "void %s_%s(void) {\n"
        "}\n\n",
        suite,
        suffix);
}

static int bake_generate_suite_file(const bake_project_cfg_t *cfg, const bake_suite_spec_t *suite) {
    char *suite_file = bake_suite_source_path(cfg, suite);
    if (!suite_file) {
        return -1;
    }

    char *existing = NULL;
    if (bake_path_exists(suite_file)) {
        existing = bake_file_read(suite_file, NULL);
    }

    ecs_strbuf_t out = ECS_STRBUF_INIT;
    if (existing) {
        ecs_strbuf_appendstr(&out, existing);
    }

    bool appended = false;

    if (suite->setup && !bake_suite_has_function(existing, suite->id, "setup")) {
        bake_append_empty_function(&out, suite->id, "setup", existing, &appended);
    }

    if (suite->teardown && !bake_suite_has_function(existing, suite->id, "teardown")) {
        bake_append_empty_function(&out, suite->id, "teardown", existing, &appended);
    }

    for (int32_t i = 0; i < suite->testcases.count; i++) {
        const char *testcase = suite->testcases.items[i];
        if (bake_suite_has_function(existing, suite->id, testcase)) {
            continue;
        }

        bake_append_empty_function(&out, suite->id, testcase, existing, &appended);
    }

    int rc = 0;
    if (existing || appended) {
        char *content = ecs_strbuf_get(&out);
        rc = content ? bake_file_write(suite_file, content) : -1;
        ecs_os_free(content);
    } else {
        ecs_strbuf_reset(&out);
    }

    ecs_os_free(existing);
    ecs_os_free(suite_file);
    return rc;
}

static void bake_generate_main_suite_params(
    ecs_strbuf_t *out,
    const bake_suite_spec_t *suite,
    const char *suite_symbol)
{
    if (!suite->param_count) {
        return;
    }

    for (int32_t p = 0; p < suite->param_count; p++) {
        const bake_param_spec_t *param = &suite->params[p];
        char *param_symbol = bake_symbol_sanitize(param->name);
        char *values_name = NULL;
        if (param_symbol) {
            values_name = flecs_asprintf("%s_%s_param", suite_symbol, param_symbol);
        }

        if (!param_symbol || !values_name) {
            ecs_os_free(param_symbol);
            ecs_os_free(values_name);
            continue;
        }

        ecs_strbuf_append(out, "const char* %s[] = {", values_name);
        for (int32_t v = 0; v < param->values.count; v++) {
            ecs_strbuf_append(out, "%s\"%s\"", v ? ", " : "", param->values.items[v]);
        }
        ecs_strbuf_appendstr(out, "};\n");

        ecs_os_free(param_symbol);
        ecs_os_free(values_name);
    }

    ecs_strbuf_append(out, "bake_test_param %s_params[] = {\n", suite_symbol);
    for (int32_t p = 0; p < suite->param_count; p++) {
        const bake_param_spec_t *param = &suite->params[p];
        char *param_symbol = bake_symbol_sanitize(param->name);
        char *values_name = NULL;
        if (param_symbol) {
            values_name = flecs_asprintf("%s_%s_param", suite_symbol, param_symbol);
        }

        if (!param_symbol || !values_name) {
            ecs_os_free(param_symbol);
            ecs_os_free(values_name);
            continue;
        }

        ecs_strbuf_append(out,
            "    {\"%s\", (char**)%s, %d},\n",
            param->name,
            values_name,
            param->values.count);

        ecs_os_free(param_symbol);
        ecs_os_free(values_name);
    }
    ecs_strbuf_appendstr(out, "};\n\n");
}

static int bake_generate_main(const bake_project_cfg_t *cfg, const bake_suite_list_t *suites) {
    char *main_path = bake_main_source_path(cfg);
    if (!main_path) {
        return -1;
    }

    ecs_strbuf_t out = ECS_STRBUF_INIT;

    ecs_strbuf_appendstr(&out,
        "/* A friendly warning from bake.test\n"
        " * ----------------------------------------------------------------------------\n"
        " * This file is generated. To add/remove testcases modify the 'project.json' of\n"
        " * the test project. ANY CHANGE TO THIS FILE IS LOST AFTER (RE)BUILDING!\n"
        " * ----------------------------------------------------------------------------\n"
        " */\n\n"
        "#include <bake_test.h>\n\n");

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
        char *suite_symbol = bake_symbol_sanitize(suite->id);
        if (!suite_symbol) {
            continue;
        }

        ecs_strbuf_append(&out, "bake_test_case %s_testcases[] = {\n", suite_symbol);
        for (int32_t t = 0; t < suite->testcases.count; t++) {
            const char *testcase = suite->testcases.items[t];
            ecs_strbuf_append(&out,
                "    {\n"
                "        \"%s\",\n"
                "        %s_%s\n"
                "    },\n",
                testcase,
                suite->id,
                testcase);
        }
        ecs_strbuf_appendstr(&out, "};\n\n");

        bake_generate_main_suite_params(&out, suite, suite_symbol);
        ecs_os_free(suite_symbol);
    }

    ecs_strbuf_appendstr(&out, "static bake_test_suite suites[] = {\n");
    for (int32_t i = 0; i < suites->count; i++) {
        const bake_suite_spec_t *suite = &suites->items[i];
        char *suite_symbol = bake_symbol_sanitize(suite->id);
        if (!suite_symbol) {
            continue;
        }

        char *setup_name = suite->setup ? flecs_asprintf("%s_setup", suite->id) : NULL;
        char *teardown_name = suite->teardown ? flecs_asprintf("%s_teardown", suite->id) : NULL;

        if (suite->param_count) {
            ecs_strbuf_append(&out,
                "    {\n"
                "        \"%s\",\n"
                "        %s,\n"
                "        %s,\n"
                "        %d,\n"
                "        %s_testcases,\n"
                "        %d,\n"
                "        %s_params\n"
                "    },\n",
                suite->id,
                setup_name ? setup_name : "0",
                teardown_name ? teardown_name : "0",
                suite->testcases.count,
                suite_symbol,
                suite->param_count,
                suite_symbol);
        } else {
            ecs_strbuf_append(&out,
                "    {\n"
                "        \"%s\",\n"
                "        %s,\n"
                "        %s,\n"
                "        %d,\n"
                "        %s_testcases\n"
                "    },\n",
                suite->id,
                setup_name ? setup_name : "0",
                teardown_name ? teardown_name : "0",
                suite->testcases.count,
                suite_symbol);
        }

        ecs_os_free(setup_name);
        ecs_os_free(teardown_name);
        ecs_os_free(suite_symbol);
    }
    ecs_strbuf_appendstr(&out, "};\n\n");

    ecs_strbuf_append(&out,
        "int main(int argc, char *argv[]) {\n"
        "    return bake_test_run(\"%s\", argc, argv, suites, %d);\n"
        "}\n",
        cfg->id,
        suites->count);

    char *content = ecs_strbuf_get(&out);
    int rc = content ? bake_file_write(main_path, content) : -1;

    ecs_os_free(content);
    ecs_os_free(main_path);
    return rc;
}

static bool bake_should_generate_harness(const char *project_json, const char *exe_path) {
    if (!exe_path || !exe_path[0]) {
        return true;
    }

    int64_t project_mtime = bake_file_mtime(project_json);
    if (project_mtime < 0) {
        return true;
    }

    int64_t exe_mtime = bake_file_mtime(exe_path);
    if (exe_mtime < 0) {
        return true;
    }

    return project_mtime > exe_mtime;
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

    char *template_root = bake_path_join(ctx->bake_home, "test");
    if (!template_root) {
        return NULL;
    }

    char *candidate = bake_path_join(template_root, file);
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

int bake_test_generate_harness(
    bake_context_t *ctx,
    const bake_project_cfg_t *cfg,
    const char *exe_path)
{
    BAKE_UNUSED(ctx);

    char *project_json = bake_path_join(cfg->path, "project.json");
    if (!project_json) {
        return -1;
    }

    if (!bake_path_exists(project_json)) {
        ecs_os_free(project_json);
        return 0;
    }

    if (!bake_should_generate_harness(project_json, exe_path)) {
        ecs_os_free(project_json);
        return 0;
    }

    bake_suite_list_t suites = {0};
    if (bake_parse_project_tests(project_json, &suites) != 0) {
        ecs_os_free(project_json);
        bake_suite_list_fini(&suites);
        return -1;
    }

    if (!suites.count) {
        ecs_os_free(project_json);
        bake_suite_list_fini(&suites);
        return 0;
    }

    for (int32_t i = 0; i < suites.count; i++) {
        if (bake_generate_suite_file(cfg, &suites.items[i]) != 0) {
            bake_suite_list_fini(&suites);
            ecs_os_free(project_json);
            return -1;
        }
    }

    int rc = bake_generate_main(cfg, &suites);

    bake_suite_list_fini(&suites);
    ecs_os_free(project_json);
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

    char *hdr_path = bake_path_join(gen_dir, "bake_test.h");
    char *src_path = bake_path_join(gen_dir, "bake_test.c");
    char *tmpl_hdr = bake_test_template_file(ctx, "bake_test.h");
    char *tmpl_src = bake_test_template_file(ctx, "bake_test.c");
    if (!hdr_path || !src_path || !tmpl_hdr || !tmpl_src) {
        ecs_os_free(hdr_path);
        ecs_os_free(src_path);
        ecs_os_free(tmpl_hdr);
        ecs_os_free(tmpl_src);
        return -1;
    }

    int rc = bake_file_copy(tmpl_hdr, hdr_path);
    if (rc == 0) {
        rc = bake_file_copy(tmpl_src, src_path);
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
        old_threads = ecs_os_strdup(old_env);
    }

    if (ctx && ctx->opts.jobs > 0) {
        char jobs_str[32];
        ecs_os_snprintf(jobs_str, sizeof(jobs_str), "%d", ctx->opts.jobs);
        bake_setenv("BAKE_TEST_THREADS", jobs_str);
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
    int rc = bake_run_command(cmd_str, false);
    ecs_os_free(cmd_str);

    if (ctx && ctx->opts.jobs > 0) {
        if (old_threads) {
            bake_setenv("BAKE_TEST_THREADS", old_threads);
        } else {
            bake_unsetenv("BAKE_TEST_THREADS");
        }
    }
    ecs_os_free(old_threads);

    return rc;
}
