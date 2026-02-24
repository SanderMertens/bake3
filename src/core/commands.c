#include "bake2/commands.h"
#include "bake2/discovery.h"
#include "bake2/environment.h"

void bake_print_help(void) {
    printf("Usage: bake [options] [command] [target]\n");
    printf("\n");
    printf("Commands:\n");
    printf("  build [target]      Build target project and dependencies (default)\n");
    printf("  run [target]        Build and run executable target\n");
    printf("  test [target]       Build and run test target\n");
    printf("  clean [target]      Remove build artifacts\n");
    printf("  rebuild [target]    Clean and build\n");
    printf("  list                List discovered projects in bake environment\n");
    printf("  info <target>       Show project info\n");
    printf("  cleanup             Remove stale projects from bake environment\n");
    printf("  reset               Reset bake environment metadata\n");
    printf("  setup               Install bake executable into bake environment\n");
    printf("\n");
    printf("Options:\n");
    printf("  --cfg <mode>        Build mode: sanitize|debug|profile|release\n");
    printf("  --cc <compiler>     Override C compiler\n");
    printf("  --cxx <compiler>    Override C++ compiler\n");
    printf("  --run-prefix <cmd>  Prefix command when running binaries\n");
    printf("  --standalone        Use amalgamated dependency sources in deps/\n");
    printf("  --strict            Enable strict compiler warnings and checks\n");
    printf("  -j <count>          Number of parallel jobs for build/test execution\n");
    printf("  -r                  Recursive clean/rebuild\n");
    printf("  -h, --help          Show this help\n");
}

static const char* bake_kind_label(bake_project_kind_t kind) {
    switch (kind) {
    case B2_PROJECT_APPLICATION: return "application";
    case B2_PROJECT_PACKAGE: return "library";
    case B2_PROJECT_CONFIG: return "config";
    case B2_PROJECT_TEST: return "test";
    case B2_PROJECT_TEMPLATE: return "template";
    default: return "application";
    }
}

static int bake_list_projects(bake_context_t *ctx) {
    if (bake_discover_projects(ctx, ctx->opts.cwd) < 0) {
        return -1;
    }

    printf("%-36s %-12s %s\n", "PROJECT", "KIND", "PATH");
    ecs_iter_t it = ecs_each_id(ctx->world, ecs_id(BakeProject));
    while (ecs_each_next(&it)) {
        const BakeProject *projects = ecs_field(&it, BakeProject, 0);
        for (int32_t i = 0; i < it.count; i++) {
            if (!projects[i].cfg || !projects[i].cfg->id) {
                continue;
            }
            printf("%-36s %-12s %s\n",
                projects[i].cfg->id,
                bake_kind_label(projects[i].cfg->kind),
                projects[i].cfg->path ? projects[i].cfg->path : "<external>");
        }
    }

    return 0;
}

static bool bake_is_abs_path(const char *path) {
    if (!path || !path[0]) {
        return false;
    }
#if defined(_WIN32)
    if (path[0] == '\\' || path[0] == '/') {
        return true;
    }
    return path[0] && path[1] == ':';
#else
    return path[0] == '/';
#endif
}

static const BakeProject* bake_find_project_for_target(
    bake_context_t *ctx,
    const char *target,
    ecs_entity_t *entity_out)
{
    const BakeProject *project = bake_model_find_project(ctx->world, target, entity_out);
    if (project) {
        return project;
    }

    if (!bake_path_exists(target)) {
        return NULL;
    }

    char *abs = NULL;
    if (bake_is_abs_path(target)) {
        abs = bake_strdup(target);
    } else {
        abs = bake_join_path(ctx->opts.cwd, target);
    }
    if (!abs) {
        return NULL;
    }

    project = bake_model_find_project_by_path(ctx->world, abs, entity_out);
    ecs_os_free(abs);
    return project;
}

static int bake_info_project(bake_context_t *ctx) {
    if (bake_discover_projects(ctx, ctx->opts.cwd) < 0) {
        return -1;
    }

    if (!ctx->opts.target || !ctx->opts.target[0]) {
        B2_ERR("info command requires a target");
        return -1;
    }

    ecs_entity_t entity = 0;
    const BakeProject *project = bake_find_project_for_target(ctx, ctx->opts.target, &entity);
    if (!project || !project->cfg) {
        B2_ERR("target not found: %s", ctx->opts.target);
        return -1;
    }

    const bake_project_cfg_t *cfg = project->cfg;
    printf("id:          %s\n", cfg->id);
    printf("kind:        %s\n", bake_kind_label(cfg->kind));
    printf("path:        %s\n", cfg->path ? cfg->path : "<external>");
    printf("language:    %s\n", cfg->language ? cfg->language : "<none>");
    printf("output:      %s\n", cfg->output_name ? cfg->output_name : "<none>");
    printf("external:    %s\n", project->external ? "true" : "false");

    if (cfg->use.count) {
        char *joined = bake_strlist_join(&cfg->use, ", ");
        printf("use:         %s\n", joined ? joined : "");
        ecs_os_free(joined);
    }
    if (cfg->use_private.count) {
        char *joined = bake_strlist_join(&cfg->use_private, ", ");
        printf("use-private: %s\n", joined ? joined : "");
        ecs_os_free(joined);
    }
    if (cfg->use_build.count) {
        char *joined = bake_strlist_join(&cfg->use_build, ", ");
        printf("use-build:   %s\n", joined ? joined : "");
        ecs_os_free(joined);
    }
    if (cfg->use_runtime.count) {
        char *joined = bake_strlist_join(&cfg->use_runtime, ", ");
        printf("use-runtime: %s\n", joined ? joined : "");
        ecs_os_free(joined);
    }

    if (cfg->dependee.use.count) {
        char *joined = bake_strlist_join(&cfg->dependee.use, ", ");
        printf("dependee.use: %s\n", joined ? joined : "");
        ecs_os_free(joined);
    }
    if (cfg->dependee.defines.count) {
        char *joined = bake_strlist_join(&cfg->dependee.defines, ", ");
        printf("dependee.defines: %s\n", joined ? joined : "");
        ecs_os_free(joined);
    }
    if (cfg->dependee.include_paths.count) {
        char *joined = bake_strlist_join(&cfg->dependee.include_paths, ", ");
        printf("dependee.include: %s\n", joined ? joined : "");
        ecs_os_free(joined);
    }
    if (cfg->dependee.cflags.count) {
        char *joined = bake_strlist_join(&cfg->dependee.cflags, ", ");
        printf("dependee.cflags: %s\n", joined ? joined : "");
        ecs_os_free(joined);
    }
    if (cfg->dependee.libs.count) {
        char *joined = bake_strlist_join(&cfg->dependee.libs, ", ");
        printf("dependee.lib: %s\n", joined ? joined : "");
        ecs_os_free(joined);
    }

    return 0;
}

int bake_execute(bake_context_t *ctx, const char *argv0) {
    if (!ctx->opts.command || !strcmp(ctx->opts.command, "build")) {
        return bake_build_run(ctx);
    }

    if (!strcmp(ctx->opts.command, "run") || !strcmp(ctx->opts.command, "test")) {
        return bake_build_run(ctx);
    }

    if (!strcmp(ctx->opts.command, "clean")) {
        return bake_build_clean(ctx);
    }

    if (!strcmp(ctx->opts.command, "rebuild")) {
        return bake_build_rebuild(ctx);
    }

    if (!strcmp(ctx->opts.command, "list")) {
        return bake_list_projects(ctx);
    }

    if (!strcmp(ctx->opts.command, "info")) {
        return bake_info_project(ctx);
    }

    if (!strcmp(ctx->opts.command, "reset")) {
        return bake_environment_reset(ctx);
    }

    if (!strcmp(ctx->opts.command, "cleanup")) {
        int32_t removed = 0;
        int rc = bake_environment_cleanup(ctx, &removed);
        if (rc == 0) {
            B2_LOG("removed %d stale project(s) from bake environment", removed);
        }
        return rc;
    }

    if (!strcmp(ctx->opts.command, "setup")) {
        return bake_environment_setup(ctx, argv0);
    }

    if (!strcmp(ctx->opts.command, "help") || !strcmp(ctx->opts.command, "--help") || !strcmp(ctx->opts.command, "-h")) {
        bake_print_help();
        return 0;
    }

    B2_ERR("unknown command: %s", ctx->opts.command);
    bake_print_help();
    return -1;
}
