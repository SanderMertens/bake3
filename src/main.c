#include "bake/commands.h"
#include "bake/context.h"
#include "bake/os.h"

static bool bake_local_env_name_char_valid(char ch) {
    return
        (ch >= 'a' && ch <= 'z') ||
        (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') ||
        ch == '.' ||
        ch == '_' ||
        ch == '-';
}

static bool bake_local_env_name_valid(const char *name) {
    if (!name || !name[0] || !strcmp(name, ".") || !strcmp(name, "..")) {
        return false;
    }

    for (const char *ptr = name; *ptr; ptr ++) {
        if (!bake_local_env_name_char_valid(*ptr)) {
            return false;
        }
    }

    return true;
}

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
        .setup_local = false,
        .local_env = false,
        .jobs = 0,
        .run_argc = 0,
        .run_argv = NULL
    };
    const char *local_env_name = NULL;

    char *cwd = bake_getcwd();
    if (!cwd) {
        ecs_err("failed to get current directory");
        return 1;
    }
    opts.cwd = cwd;

    if (argv[0] && argv[0][0]) {
        char *exe_path = NULL;
        if (bake_path_is_abs(argv[0])) {
            exe_path = ecs_os_strdup(argv[0]);
        } else {
            exe_path = bake_path_join(cwd, argv[0]);
        }

        if (exe_path && bake_path_exists(exe_path)) {
            bake_setenv("BAKE2_EXEC_PATH", exe_path);
        } else {
            bake_unsetenv("BAKE2_EXEC_PATH");
        }
        ecs_os_free(exe_path);
    }

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

        if (!strcmp(arg, "--local")) {
            opts.setup_local = true;
            continue;
        }

        if (!strcmp(arg, "--local-env")) {
            opts.local_env = true;
            local_env_name = NULL;
            if ((i + 2) < argc &&
                argv[i + 1][0] &&
                argv[i + 1][0] != '-' &&
                !bake_is_command(argv[i + 1]) &&
                bake_is_command(argv[i + 2]))
            {
                const char *candidate = argv[++i];
                if (!bake_local_env_name_valid(candidate)) {
                    ecs_err(
                        "invalid --local-env name '%s' (use letters, digits, '.', '_' or '-')",
                        candidate);
                    ecs_os_free(cwd);
                    return 1;
                }
                local_env_name = candidate;
            }
            continue;
        }

        if (!strncmp(arg, "--local-env=", 12)) {
            const char *candidate = arg + 12;
            if (!candidate[0]) {
                ecs_err("missing value for --local-env");
                ecs_os_free(cwd);
                return 1;
            }
            if (!bake_local_env_name_valid(candidate)) {
                ecs_err(
                    "invalid --local-env name '%s' (use letters, digits, '.', '_' or '-')",
                    candidate);
                ecs_os_free(cwd);
                return 1;
            }
            opts.local_env = true;
            local_env_name = candidate;
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

    char *local_bake_home = NULL;
    if (opts.local_env) {
        const char *existing_bake_home = getenv("BAKE_HOME");
        if (existing_bake_home && existing_bake_home[0]) {
            bake_setenv("BAKE_GLOBAL_HOME", existing_bake_home);
        } else {
            bake_unsetenv("BAKE_GLOBAL_HOME");
        }

        char *local_env_root = bake_path_join3(cwd, ".bake", "local_env");
        local_bake_home = local_env_name ?
            (local_env_root ? bake_path_join(local_env_root, local_env_name) : NULL) :
            local_env_root;
        if (local_env_name) {
            ecs_os_free(local_env_root);
        }
        if (!local_bake_home) {
            ecs_err("failed to resolve local bake environment path");
            ecs_os_free(cwd);
            return 1;
        }
        bake_setenv("BAKE_HOME", local_bake_home);
        bake_setenv("BAKE_LOCAL_ENV", "1");
    } else {
        bake_setenv("BAKE_LOCAL_ENV", "0");
        bake_unsetenv("BAKE_GLOBAL_HOME");
    }

    if (opts.setup_local && strcmp(opts.command, "setup")) {
        ecs_err("--local can only be used with the setup command");
        ecs_os_free(local_bake_home);
        ecs_os_free(cwd);
        return 1;
    }

    bake_context_t ctx;
    if (bake_context_init(&ctx, &opts) != 0) {
        ecs_err("failed to initialize bake context");
        ecs_os_free(local_bake_home);
        ecs_os_free(cwd);
        return 1;
    }

    int rc = bake_execute(&ctx, argv[0]);
    bake_context_fini(&ctx);
    ecs_os_free(local_bake_home);
    ecs_os_free(cwd);

    return rc == 0 ? 0 : 1;
}
