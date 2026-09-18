#include "bench_internal.h"
#include "bake/os.h"
#include "common/harness_util.h"

static void bake_bench_append_stub(
    ecs_strbuf_t *out,
    const char *suite,
    const char *name,
    const char *existing,
    bool *appended)
{
    bake_harness_append_separator(out, existing, appended);
    ecs_strbuf_append(out,
        "void %s_%s(bench_t *b) {\n"
        "    while (bench_iter(b)) {\n"
        "    }\n"
        "}\n\n",
        suite,
        name);
}

static void bake_bench_append_fixture(
    ecs_strbuf_t *out,
    const char *suite,
    const char *suffix,
    const char *existing,
    bool *appended)
{
    bake_harness_append_separator(out, existing, appended);
    ecs_strbuf_append(out,
        "void %s_%s(void) {\n"
        "}\n\n",
        suite,
        suffix);
}

int bake_generate_benchsuite_file(
    const bake_project_cfg_t *cfg,
    const bake_benchsuite_spec_t *suite)
{
    char *suite_file = bake_harness_source_path(cfg, suite->id);

    char *existing = NULL;
    if (bake_path_exists(suite_file)) {
        existing = bake_file_read(suite_file, NULL);
    }

    ecs_strbuf_t out = ECS_STRBUF_INIT;
    bool appended = false;

    if (existing) {
        ecs_strbuf_appendstr(&out, existing);
    } else {
        char *project_header = bake_harness_project_header(cfg);
        ecs_strbuf_append(&out, "#include <%s>\n\n",
            project_header ? project_header : "bake_bench.h");
        ecs_os_free(project_header);
        appended = true;
    }

    if (suite->setup && !bake_harness_suite_has_function(existing, suite->id, "setup")) {
        bake_bench_append_fixture(&out, suite->id, "setup", existing, &appended);
    }

    if (suite->teardown && !bake_harness_suite_has_function(existing, suite->id, "teardown")) {
        bake_bench_append_fixture(&out, suite->id, "teardown", existing, &appended);
    }

    for (int32_t i = 0; i < suite->benchcases.count; i++) {
        const char *benchcase = suite->benchcases.items[i];
        if (bake_harness_suite_has_function(existing, suite->id, benchcase)) {
            continue;
        }

        bake_bench_append_stub(&out, suite->id, benchcase, existing, &appended);
    }

    int rc = 0;
    if (appended) {
        char *content = ecs_strbuf_get(&out);
        rc = bake_file_write(suite_file, content);
        ecs_os_free(content);
    } else {
        ecs_strbuf_reset(&out);
    }

    ecs_os_free(existing);
    ecs_os_free(suite_file);
    return rc;
}

static void bake_generate_bench_suite_decls(
    ecs_strbuf_t *out,
    const bake_benchsuite_spec_t *suite)
{
    ecs_strbuf_append(out, "// Benchsuite '%s'\n", suite->id);

    if (suite->setup) {
        ecs_strbuf_append(out, "void %s_setup(void);\n", suite->id);
    }
    if (suite->teardown) {
        ecs_strbuf_append(out, "void %s_teardown(void);\n", suite->id);
    }

    for (int32_t t = 0; t < suite->benchcases.count; t++) {
        ecs_strbuf_append(out, "void %s_%s(bench_t *b);\n",
            suite->id, suite->benchcases.items[t]);
    }

    ecs_strbuf_appendstr(out, "\n");
}

int bake_generate_bench_main(
    const bake_project_cfg_t *cfg,
    const bake_benchsuite_list_t *suites)
{
    char *main_path = bake_harness_source_path(cfg, "main");
    char *project_header = bake_harness_project_header(cfg);
    const char *header_include = project_header ? project_header : "bake_bench.h";

    ecs_strbuf_t out = ECS_STRBUF_INIT;

    ecs_strbuf_appendstr(&out,
        "\n"
        "/* A friendly warning from bake.bench\n"
        " * ----------------------------------------------------------------------------\n"
        " * This file is generated. To add/remove benchcases modify the 'project.json' of\n"
        " * the bench project. ANY CHANGE TO THIS FILE IS LOST AFTER (RE)BUILDING!\n"
        " * ----------------------------------------------------------------------------\n"
        " */\n\n");
    ecs_strbuf_append(&out, "#include <%s>\n\n", header_include);

    for (int32_t i = 0; i < suites->count; i++) {
        bake_generate_bench_suite_decls(&out, &suites->items[i]);
    }

    for (int32_t i = 0; i < suites->count; i++) {
        const bake_benchsuite_spec_t *suite = &suites->items[i];

        ecs_strbuf_append(&out, "bake_bench_case %s_benchcases[] = {\n", suite->id);
        for (int32_t t = 0; t < suite->benchcases.count; t++) {
            const char *benchcase = suite->benchcases.items[t];
            ecs_strbuf_append(&out,
                "    {\n"
                "        \"%s\",\n"
                "        %s_%s\n"
                "    }%s\n",
                benchcase,
                suite->id,
                benchcase,
                (t + 1) < suite->benchcases.count ? "," : "");
        }
        ecs_strbuf_appendstr(&out, "};\n\n");
    }

    ecs_strbuf_appendstr(&out, "static bake_bench_suite suites[] = {\n");
    for (int32_t i = 0; i < suites->count; i++) {
        const bake_benchsuite_spec_t *suite = &suites->items[i];

        char *setup_name = suite->setup ? flecs_asprintf("%s_setup", suite->id) : NULL;
        char *teardown_name = suite->teardown ?
            flecs_asprintf("%s_teardown", suite->id) : NULL;

        ecs_strbuf_append(&out,
            "    {\n"
            "        \"%s\",\n"
            "        %s,\n"
            "        %s,\n"
            "        %d,\n"
            "        %s_benchcases\n"
            "    }%s\n",
            suite->id,
            setup_name ? setup_name : "NULL",
            teardown_name ? teardown_name : "NULL",
            suite->benchcases.count,
            suite->id,
            (i + 1) < suites->count ? "," : "");

        ecs_os_free(setup_name);
        ecs_os_free(teardown_name);
    }
    ecs_strbuf_appendstr(&out, "};\n\n");

    ecs_strbuf_append(&out,
        "int main(int argc, char *argv[]) {\n"
        "    return bake_bench_run(\"%s\", argc, argv, suites, %d);\n"
        "}\n",
        cfg->id,
        suites->count);

    char *content = ecs_strbuf_get(&out);
    int rc = bake_file_write(main_path, content);

    ecs_os_free(content);
    ecs_os_free(project_header);
    ecs_os_free(main_path);
    return rc;
}
