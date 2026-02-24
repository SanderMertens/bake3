#include "bake2/commands.h"
#include "bake2/context.h"

static bool bake_is_command(const char *arg) {
    return !strcmp(arg, "build") ||
        !strcmp(arg, "run") ||
        !strcmp(arg, "test") ||
        !strcmp(arg, "clean") ||
        !strcmp(arg, "rebuild") ||
        !strcmp(arg, "list") ||
        !strcmp(arg, "info") ||
        !strcmp(arg, "reset") ||
        !strcmp(arg, "cleanup") ||
        !strcmp(arg, "setup") ||
        !strcmp(arg, "help");
}

int main(int argc, char *argv[]) {
    ecs_os_init();
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    bake_options_t opts = {
        .command = "build",
        .target = NULL,
        .mode = "debug",
        .cwd = NULL,
        .cc = NULL,
        .cxx = NULL,
        .run_prefix = NULL,
        .recursive = false,
        .standalone = false,
        .strict = false,
        .trace = false,
        .jobs = 0,
        .run_argc = 0,
        .run_argv = NULL
    };

    char *cwd = bake_getcwd();
    if (!cwd) {
        ecs_err("failed to get current directory");
        return 1;
    }
    opts.cwd = cwd;

    bool seen_command = false;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (!strcmp(arg, "--")) {
            opts.run_argc = argc - i - 1;
            opts.run_argv = (const char**)&argv[i + 1];
            break;
        }

        if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
            bake_print_help();
            ecs_os_free(cwd);
            return 0;
        }

        if (!strcmp(arg, "-r")) {
            opts.recursive = true;
            continue;
        }

        if (!strcmp(arg, "--standalone")) {
            opts.standalone = true;
            continue;
        }

        if (!strcmp(arg, "--strict")) {
            opts.strict = true;
            continue;
        }

        if (!strcmp(arg, "--trace")) {
            opts.trace = true;
            continue;
        }

        if (!strcmp(arg, "-j")) {
            if ((i + 1) >= argc) {
                ecs_err("missing value for -j");
                ecs_os_free(cwd);
                return 1;
            }

            int jobs = atoi(argv[++i]);
            if (jobs < 1) {
                ecs_err("invalid value for -j: %s", argv[i]);
                ecs_os_free(cwd);
                return 1;
            }
            opts.jobs = jobs;
            continue;
        }

        if (!strcmp(arg, "--cfg") && i + 1 < argc) {
            opts.mode = argv[++i];
            continue;
        }

        if (!strcmp(arg, "--cc") && i + 1 < argc) {
            opts.cc = argv[++i];
            continue;
        }

        if (!strcmp(arg, "--cxx") && i + 1 < argc) {
            opts.cxx = argv[++i];
            continue;
        }

        if (!strcmp(arg, "--run-prefix") && i + 1 < argc) {
            opts.run_prefix = argv[++i];
            continue;
        }

        if (!seen_command && bake_is_command(arg)) {
            opts.command = arg;
            seen_command = true;
            continue;
        }

        if (!opts.target) {
            opts.target = arg;
            continue;
        }
    }

    bake_context_t ctx;
    if (bake_context_init(&ctx, &opts) != 0) {
        ecs_err("failed to initialize bake context");
        ecs_os_free(cwd);
        return 1;
    }

    int rc = bake_execute(&ctx, argv[0]);
    bake_context_fini(&ctx);
    ecs_os_free(cwd);

    return rc == 0 ? 0 : 1;
}
