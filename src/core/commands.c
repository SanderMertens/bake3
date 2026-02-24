#include "bake2/commands.h"
#include "bake2/discovery.h"
#include "bake2/environment.h"

void b2_print_help(void) {
    printf("Usage: bake [options] [command] [target]\n");
    printf("\n");
    printf("Commands:\n");
    printf("  build [target]      Build target project and dependencies (default)\n");
    printf("  run [target]        Build and run executable target\n");
    printf("  test [target]       Build and run test target\n");
    printf("  clean [target]      Remove build artifacts\n");
    printf("  rebuild [target]    Clean and build\n");
    printf("  list                List discovered projects in bake environment\n");
    printf("  setup               Install bake executable into bake environment\n");
    printf("\n");
    printf("Options:\n");
    printf("  --cfg <mode>        Build mode: sanitize|debug|profile|release\n");
    printf("  --cc <compiler>     Override C compiler\n");
    printf("  --cxx <compiler>    Override C++ compiler\n");
    printf("  --run-prefix <cmd>  Prefix command when running binaries\n");
    printf("  --standalone        Use amalgamated dependency sources in deps/\n");
    printf("  -r                  Recursive clean/rebuild\n");
    printf("  -h, --help          Show this help\n");
}

static int b2_list_projects(b2_context_t *ctx) {
    if (b2_discover_projects(ctx, ctx->opts.cwd) < 0) {
        return -1;
    }

    ecs_iter_t it = ecs_each_id(ctx->world, ecs_id(b2_project_t));
    while (ecs_each_next(&it)) {
        const b2_project_t *projects = ecs_field(&it, b2_project_t, 0);
        for (int32_t i = 0; i < it.count; i++) {
            if (!projects[i].cfg || !projects[i].cfg->id) {
                continue;
            }
            printf("%-36s %s\n", projects[i].cfg->id,
                projects[i].cfg->path ? projects[i].cfg->path : "<external>");
        }
    }

    return 0;
}

int b2_execute(b2_context_t *ctx, const char *argv0) {
    if (!ctx->opts.command || !strcmp(ctx->opts.command, "build")) {
        return b2_build_run(ctx);
    }

    if (!strcmp(ctx->opts.command, "run") || !strcmp(ctx->opts.command, "test")) {
        return b2_build_run(ctx);
    }

    if (!strcmp(ctx->opts.command, "clean")) {
        return b2_build_clean(ctx);
    }

    if (!strcmp(ctx->opts.command, "rebuild")) {
        return b2_build_rebuild(ctx);
    }

    if (!strcmp(ctx->opts.command, "list")) {
        return b2_list_projects(ctx);
    }

    if (!strcmp(ctx->opts.command, "setup")) {
        return b2_environment_setup(ctx, argv0);
    }

    if (!strcmp(ctx->opts.command, "help") || !strcmp(ctx->opts.command, "--help") || !strcmp(ctx->opts.command, "-h")) {
        b2_print_help();
        return 0;
    }

    B2_ERR("unknown command: %s", ctx->opts.command);
    b2_print_help();
    return -1;
}
