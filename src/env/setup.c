#include "bake/environment.h"
#include "bake/os.h"
#include "env_internal.h"

static int bake_env_run_argv_checked(const char *const *argv) {
    bake_process_result_t result = {0};
    if (bake_proc_run_argv(argv, &result) != 0) {
        return -1;
    }

    if (result.interrupted || result.term_signal || result.exit_code != 0) {
        return -1;
    }

    return 0;
}

static char* bake_env_executable_path(const char *argv0) {
    if (!argv0 || !argv0[0]) {
        return NULL;
    }

    if (bake_path_exists(argv0)) {
        return ecs_os_strdup(argv0);
    }

    char *cwd = bake_os_getcwd();
    char *candidate = cwd ? bake_path_join(cwd, argv0) : NULL;
    ecs_os_free(cwd);
    if (candidate && bake_path_exists(candidate)) {
        return candidate;
    }

    ecs_os_free(candidate);
    return NULL;
}


static int bake_env_install_bake3_wrapper(const bake_context_t *ctx) {
    static const char *script_path = "/usr/local/bin/bake3";
    static const char *script_content =
        "#!/usr/bin/env bash\n"
        "\n"
        "exec $HOME/bake3/bake3 \"$@\"\n";

    size_t current_len = 0;
    char *current = bake_file_read(script_path, &current_len);
    if (current && !strcmp(current, script_content)) {
        ecs_os_free(current);
        return 0;
    }
    ecs_os_free(current);

    char *tmp_script = bake_path_join(ctx->bake_home, ".bake3-wrapper.sh");
    if (!tmp_script) {
        return -1;
    }

    if (bake_file_write(tmp_script, script_content) != 0) {
        ecs_os_free(tmp_script);
        return -1;
    }

    const char *chmod_argv[] = {"chmod", "+x", tmp_script, NULL};
    if (bake_env_run_argv_checked(chmod_argv) != 0) {
        remove(tmp_script);
        ecs_os_free(tmp_script);
        return -1;
    }

    const char *copy_argv[] = {"sudo", "cp", tmp_script, script_path, NULL};
    int rc = bake_env_run_argv_checked(copy_argv);
    remove(tmp_script);
    ecs_os_free(tmp_script);
    return rc;
}

int bake_env_setup(bake_context_t *ctx, const char *argv0) {
    if (bake_os_mkdirs(ctx->bake_home) != 0) {
        return -1;
    }

#if defined(_WIN32)
    char *dst = bake_path_join(ctx->bake_home, "bake3.exe");
#else
    char *dst = bake_path_join(ctx->bake_home, "bake3");
#endif
    if (!dst) {
        return -1;
    }

    char *src = bake_env_executable_path(argv0);
    char *test_dst = bake_path_join(ctx->bake_home, "test");

    if (!src || !test_dst) {
        ecs_err("failed to resolve setup paths");
        ecs_os_free(dst);
        ecs_os_free(src);
        ecs_os_free(test_dst);
        return -1;
    }

    int rc = 0;
    if (!bake_path_equal_normalized(src, dst)) {
        rc = bake_os_file_copy(src, dst);
    }

    if (rc == 0) {
        char *test_src = bake_env_find_test_template_source();
        if (!test_src) {
            rc = -1;
        } else {
            rc = bake_env_copy_tree_exact(test_src, test_dst);
            if (rc != 0) {
                ecs_err("failed to install test harness templates from %s to %s", test_src, test_dst);
            }
        }
        ecs_os_free(test_src);
    }

    if (rc == 0 && !ctx->opts.setup_local) {
        rc = bake_env_install_bake3_wrapper(ctx);
    }

    if (rc == 0) {
        /*

        ______   ______   ______   __   __       ______   ______  ______
        /\  __ \ /\  ___\ /\  ___\ /\ \ /\ \     /\  __ \ /\  == \/\__  _\
        \ \  __ \\ \___  \\ \ \____\ \ \\ \ \    \ \  __ \\ \  __<\/_/\ \/
        \ \_\ \_\\/\_____\\ \_____\\ \_\\ \_\    \ \_\ \_\\ \_\ \_\ \ \_\
        \/_/\/_/ \/_____/ \/_____/ \/_/ \/_/     \/_/\/_/ \/_/ /_/  \/_/

        */

        ecs_log(0,
            "#[white]\n"
            "#[normal]    #[cyan]___      ___      ___      ___ \n"
            "#[normal]   /\\#[cyan]  \\    #[normal]/\\#[cyan]  \\    #[normal]/\\#[cyan]__\\    #[normal]/\\  #[cyan]\\ \n"
            "#[normal]  /  \\#[cyan]  \\  #[normal]/  \\#[cyan]  \\  #[normal]/ / #[cyan]_/_  #[normal]/  \\  #[cyan]\\ \n"
            "#[normal] /  \\ \\#[cyan]__\\#[normal]/  \\ \\#[cyan]__\\#[normal]/  -\"\\#[cyan]__\\#[normal]/  \\ \\#[cyan]__\\ \n"
            "#[normal] \\ \\  /#[cyan]  /#[normal]\\/\\  /#[cyan]  /#[normal]\\; ;-\"#[cyan],-\"#[normal]\\ \\ \\/  #[cyan]/ \n"
            "#[normal]  \\  /#[cyan]  /   #[normal]/ /  #[cyan]/  #[normal]| |  #[cyan]|   #[normal]\\ \\/  #[cyan]/ \n"
            "#[normal]   \\/#[cyan]__/    #[normal]\\/#[cyan]__/    #[normal]\\|#[cyan]__|    #[normal]\\/#[cyan]__/ \n\n");

        if (ctx->opts.setup_local) {
            printf("\n       Installation complete (local mode).\n\n");
        } else {
            printf("\n       Installation complete!\n\n");
        }
    }

    ecs_os_free(src);
    ecs_os_free(dst);
    ecs_os_free(test_dst);
    return rc;
}
