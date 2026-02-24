#include "bake2/commands.h"
#include "bake2/context.h"

static bool b2_is_command(const char *arg) {
    return !strcmp(arg, "build") ||
        !strcmp(arg, "run") ||
        !strcmp(arg, "test") ||
        !strcmp(arg, "clean") ||
        !strcmp(arg, "rebuild") ||
        !strcmp(arg, "list") ||
        !strcmp(arg, "setup") ||
        !strcmp(arg, "help");
}

int main(int argc, char *argv[]) {
    ecs_os_init();
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    b2_options_t opts = {
        .command = "build",
        .target = NULL,
        .mode = "debug",
        .cwd = NULL,
        .cc = NULL,
        .cxx = NULL,
        .run_prefix = NULL,
        .recursive = false,
        .standalone = false,
        .run_argc = 0,
        .run_argv = NULL
    };

    char *cwd = b2_getcwd();
    if (!cwd) {
        B2_ERR("failed to get current directory");
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
            b2_print_help();
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

        if (!seen_command && b2_is_command(arg)) {
            opts.command = arg;
            seen_command = true;
            continue;
        }

        if (!opts.target) {
            opts.target = arg;
            continue;
        }
    }

    b2_context_t ctx;
    if (b2_context_init(&ctx, &opts) != 0) {
        B2_ERR("failed to initialize bake context");
        ecs_os_free(cwd);
        return 1;
    }

    int rc = b2_execute(&ctx, argv[0]);
    b2_context_fini(&ctx);
    ecs_os_free(cwd);

    return rc == 0 ? 0 : 1;
}
