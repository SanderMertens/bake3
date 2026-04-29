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
    static const char *cmds[] = {
        "build", "run", "test", "clean", "rebuild", "list",
        "info", "reset", "cleanup", "setup", "help"
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        if (!strcmp(arg, cmds[i])) return true;
    }
    return false;
}

int main(int argc, char *argv[]) {
    ecs_os_init();
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    int rc = 1;
    bake_options_t opts = { .command = "build", .mode = "debug" };
    const char *local_env_name = NULL;
    char *local_bake_home = NULL;
    bool ctx_initialized = false;
    bake_context_t ctx;

    char *cwd = bake_os_getcwd();
    if (!cwd) {
        ecs_err("failed to get current directory");
        return 1;
    }
    opts.cwd = cwd;

    if (argv[0] && argv[0][0]) {
        char *exe_path = bake_path_is_abs(argv[0])
            ? ecs_os_strdup(argv[0])
            : bake_path_join(cwd, argv[0]);
        if (exe_path && bake_path_exists(exe_path)) {
            bake_os_setenv("BAKE2_EXEC_PATH", exe_path);
        } else {
            bake_os_unsetenv("BAKE2_EXEC_PATH");
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
            rc = 0;
            goto cleanup;
        }

#define BFLAG(name, field) if (!strcmp(arg, name)) { opts.field = true; continue; }
        BFLAG("-r", recursive)
        BFLAG("--standalone", standalone)
        BFLAG("--strict", strict)
        BFLAG("--trace", trace)
        BFLAG("--local", setup_local)
#undef BFLAG

        if (!strcmp(arg, "--local-env") || !strncmp(arg, "--local-env=", 12)) {
            opts.local_env = true;
            const char *candidate = NULL;
            if (arg[11] == '=') {
                candidate = arg + 12;
                if (!candidate[0]) {
                    ecs_err("missing value for --local-env");
                    goto cleanup;
                }
            } else if ((i + 2) < argc && argv[i + 1][0] && argv[i + 1][0] != '-' &&
                       !bake_is_command(argv[i + 1]) && bake_is_command(argv[i + 2]))
            {
                candidate = argv[++i];
            }
            if (candidate && !bake_local_env_name_valid(candidate)) {
                ecs_err("invalid --local-env name '%s' (use letters, digits, '.', '_' or '-')", candidate);
                goto cleanup;
            }
            local_env_name = candidate;
            continue;
        }

        if (!strcmp(arg, "-j")) {
            if ((i + 1) >= argc) {
                ecs_err("missing value for -j");
                goto cleanup;
            }
            int jobs = atoi(argv[++i]);
            if (jobs < 1) {
                ecs_err("invalid value for -j: %s", argv[i]);
                goto cleanup;
            }
            opts.jobs = jobs;
            continue;
        }

#define VARG(name, field) if (!strcmp(arg, name) && i + 1 < argc) { opts.field = argv[++i]; continue; }
        VARG("--cfg", mode)
        VARG("--cc", cc)
        VARG("--cxx", cxx)
        VARG("--run-prefix", run_prefix)
#undef VARG

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

    if (opts.local_env) {
        const char *existing_bake_home = getenv("BAKE_HOME");
        if (existing_bake_home && existing_bake_home[0]) {
            bake_os_setenv("BAKE_GLOBAL_HOME", existing_bake_home);
        } else {
            bake_os_unsetenv("BAKE_GLOBAL_HOME");
        }

        char *local_env_root = bake_path_join3(cwd, ".bake", "local_env");
        local_bake_home = local_env_name
            ? (local_env_root ? bake_path_join(local_env_root, local_env_name) : NULL)
            : local_env_root;
        if (local_env_name) ecs_os_free(local_env_root);
        if (!local_bake_home) {
            ecs_err("failed to resolve local bake environment path");
            goto cleanup;
        }
        bake_os_setenv("BAKE_HOME", local_bake_home);
        bake_os_setenv("BAKE_LOCAL_ENV", "1");
    } else {
        bake_os_setenv("BAKE_LOCAL_ENV", "0");
        bake_os_unsetenv("BAKE_GLOBAL_HOME");
    }

    if (opts.setup_local && strcmp(opts.command, "setup")) {
        ecs_err("--local can only be used with the setup command");
        goto cleanup;
    }

    if (bake_context_init(&ctx, &opts) != 0) {
        ecs_err("failed to initialize bake context");
        goto cleanup;
    }
    ctx_initialized = true;

    rc = bake_execute(&ctx, argv[0]) == 0 ? 0 : 1;

cleanup:
    if (ctx_initialized) bake_context_fini(&ctx);
    ecs_os_free(local_bake_home);
    ecs_os_free(cwd);
    return rc;
}
