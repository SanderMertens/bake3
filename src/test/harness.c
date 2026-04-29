#include "bake/test_harness.h"
#include "bake/os.h"

#include "harness_internal.h"

static bool bake_should_generate_harness(const char *project_json, const char *exe_path) {
    if (!exe_path || !exe_path[0]) {
        return true;
    }

    int64_t project_mtime = bake_os_file_mtime(project_json);
    if (project_mtime < 0) {
        return true;
    }

    int64_t exe_mtime = bake_os_file_mtime(exe_path);
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

    int rc = 0;
    bake_suite_list_t suites = {0};
    char *project_json = bake_path_join(cfg->path, "project.json");
    if (!project_json) return -1;

    if (!bake_path_exists(project_json) ||
        !bake_should_generate_harness(project_json, exe_path))
    {
        goto cleanup;
    }

    if (bake_parse_project_tests(project_json, &suites) != 0) {
        rc = -1;
        goto cleanup;
    }

    for (int32_t i = 0; i < suites.count; i++) {
        if (bake_generate_suite_file(cfg, &suites.items[i]) != 0) {
            rc = -1;
            goto cleanup;
        }
    }

    if (suites.count) {
        rc = bake_generate_main(cfg, &suites);
    }

cleanup:
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
    if (!gen_dir || !src_out) return -1;

    int rc = -1;
    char *hdr_path = bake_path_join(gen_dir, "bake_test.h");
    char *src_path = bake_path_join(gen_dir, "bake_test.c");
    char *tmpl_hdr = bake_test_template_file(ctx, "bake_test.h");
    char *tmpl_src = bake_test_template_file(ctx, "bake_test.c");

    if (hdr_path && src_path && tmpl_hdr && tmpl_src &&
        bake_os_file_copy(tmpl_hdr, hdr_path) == 0 &&
        bake_os_file_copy(tmpl_src, src_path) == 0)
    {
        *src_out = src_path;
        src_path = NULL;
        rc = 0;
    }

    ecs_os_free(hdr_path);
    ecs_os_free(src_path);
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
    int rc = bake_run_command(cmd_str, false);
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
