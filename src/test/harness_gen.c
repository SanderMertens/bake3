#include "harness_internal.h"
#include "bake/os.h"

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

static char* bake_strip_strings_and_comments(const char *text) {
    if (!text) {
        return NULL;
    }
    size_t len = strlen(text);
    char *out = ecs_os_malloc(len + 1);
    if (!out) {
        return NULL;
    }
    size_t o = 0;
    const char *p = text;
    while (*p) {
        if (p[0] == '/' && p[1] == '/') {
            p += 2;
            while (*p && *p != '\n') {
                p++;
            }
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) {
                if (*p == '\n') {
                    out[o++] = '\n';
                }
                p++;
            }
            if (*p) {
                p += 2;
            }
            continue;
        }
        if (*p == '"' || *p == '\'') {
            char quote = *p;
            out[o++] = ' ';
            p++;
            while (*p && *p != quote) {
                if (*p == '\\' && p[1]) {
                    p += 2;
                    continue;
                }
                if (*p == '\n') {
                    out[o++] = '\n';
                }
                p++;
            }
            if (*p) {
                p++;
            }
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = '\0';
    return out;
}

static int bake_text_contains_function_definition(const char *text, const char *function_name) {
    if (!text || !function_name || !function_name[0]) {
        return 0;
    }

    char *clean = bake_strip_strings_and_comments(text);
    if (!clean) {
        return 0;
    }

    size_t name_len = strlen(function_name);
    const char *cursor = clean;
    int found = 0;
    while (true) {
        const char *hit = strstr(cursor, function_name);
        if (!hit) {
            break;
        }

        if (hit != clean && bake_char_is_ident(hit[-1])) {
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
            break;
        }

        ptr = bake_skip_ws_and_comments(ptr);
        if (*ptr == '{') {
            found = 1;
            break;
        }

        cursor = hit + 1;
    }

    ecs_os_free(clean);
    return found;
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

static char* bake_harness_try_project_header(const char *include_dir, const char *project_id) {
    if (!include_dir || !project_id || !project_id[0]) {
        return NULL;
    }

    char *header_id = bake_project_id_as_macro(project_id);
    char *header_name = header_id ? flecs_asprintf("%s.h", header_id) : NULL;
    char *header_path = header_name ? bake_path_join(include_dir, header_name) : NULL;

    if (header_path && bake_path_exists(header_path)) {
        ecs_os_free(header_id);
        ecs_os_free(header_path);
        return header_name;
    }

    ecs_os_free(header_id);
    ecs_os_free(header_name);
    ecs_os_free(header_path);
    return NULL;
}

static char* bake_harness_project_header(const bake_project_cfg_t *cfg) {
    if (!cfg || !cfg->path || !cfg->id) {
        return NULL;
    }

    char *include_dir = bake_path_join(cfg->path, "include");
    char *header_name = NULL;
    char *base_id = NULL;

    if (!include_dir || !bake_path_exists(include_dir)) {
        ecs_os_free(include_dir);
        return NULL;
    }

    header_name = bake_harness_try_project_header(include_dir, cfg->id);
    if (!header_name) {
        base_id = bake_project_id_base(cfg->id);
        if (base_id && strcmp(base_id, cfg->id)) {
            header_name = bake_harness_try_project_header(include_dir, base_id);
        }
    }

    ecs_os_free(base_id);
    ecs_os_free(include_dir);
    return header_name;
}

static char* bake_source_path(const bake_project_cfg_t *cfg, const char *base) {
    const char *ext = bake_language_is_cpp(cfg) ? "cpp" : "c";
    char *file_name = flecs_asprintf("%s.%s", base, ext);
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

int bake_generate_suite_file(const bake_project_cfg_t *cfg, const bake_suite_spec_t *suite) {
    char *suite_file = bake_source_path(cfg, suite->id);
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
            "    {\"%s\", (char**)%s, %d}%s\n",
            param->name,
            values_name,
            param->values.count,
            (p + 1) < suite->param_count ? "," : "");

        ecs_os_free(param_symbol);
        ecs_os_free(values_name);
    }
    ecs_strbuf_appendstr(out, "};\n\n");
}

static void bake_generate_main_suite_decls(
    ecs_strbuf_t *out,
    const bake_suite_spec_t *suite)
{
    ecs_strbuf_append(out, "// Testsuite '%s'\n", suite->id);

    if (suite->setup) {
        ecs_strbuf_append(out, "void %s_setup(void);\n", suite->id);
    }
    if (suite->teardown) {
        ecs_strbuf_append(out, "void %s_teardown(void);\n", suite->id);
    }

    for (int32_t t = 0; t < suite->testcases.count; t++) {
        ecs_strbuf_append(out, "void %s_%s(void);\n", suite->id, suite->testcases.items[t]);
    }

    ecs_strbuf_appendstr(out, "\n");
}

int bake_generate_main(const bake_project_cfg_t *cfg, const bake_suite_list_t *suites) {
    char *main_path = bake_source_path(cfg, "main");
    char *project_header = bake_harness_project_header(cfg);
    const char *header_include = project_header ? project_header : "bake_test.h";
    if (!main_path) {
        ecs_os_free(project_header);
        return -1;
    }

    ecs_strbuf_t out = ECS_STRBUF_INIT;

    ecs_strbuf_appendstr(&out,
        "\n"
        "/* A friendly warning from bake.test\n"
        " * ----------------------------------------------------------------------------\n"
        " * This file is generated. To add/remove testcases modify the 'project.json' of\n"
        " * the test project. ANY CHANGE TO THIS FILE IS LOST AFTER (RE)BUILDING!\n"
        " * ----------------------------------------------------------------------------\n"
        " */\n\n");
    ecs_strbuf_append(&out, "#include <%s>\n\n", header_include);

    for (int32_t i = 0; i < suites->count; i++) {
        bake_generate_main_suite_decls(&out, &suites->items[i]);
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
                "    }%s\n",
                testcase,
                suite->id,
                testcase,
                (t + 1) < suite->testcases.count ? "," : "");
        }
        ecs_strbuf_appendstr(&out, "};\n\n");
        ecs_os_free(suite_symbol);
    }

    for (int32_t i = 0; i < suites->count; i++) {
        const bake_suite_spec_t *suite = &suites->items[i];
        char *suite_symbol = bake_symbol_sanitize(suite->id);
        if (!suite_symbol) {
            continue;
        }

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
                "    }%s\n",
                suite->id,
                setup_name ? setup_name : "NULL",
                teardown_name ? teardown_name : "NULL",
                suite->testcases.count,
                suite_symbol,
                suite->param_count,
                suite_symbol,
                (i + 1) < suites->count ? "," : "");
        } else {
            ecs_strbuf_append(&out,
                "    {\n"
                "        \"%s\",\n"
                "        %s,\n"
                "        %s,\n"
                "        %d,\n"
                "        %s_testcases\n"
                "    }%s\n",
                suite->id,
                setup_name ? setup_name : "NULL",
                teardown_name ? teardown_name : "NULL",
                suite->testcases.count,
                suite_symbol,
                (i + 1) < suites->count ? "," : "");
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
    ecs_os_free(project_header);
    ecs_os_free(main_path);
    return rc;
}
