#include "bake2/environment.h"
#include "bake2/os.h"

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

static int bake_env_install_bake3_wrapper(const bake_context_t *ctx) {
    static const char *script_path = "/usr/local/bin/bake3";
    static const char *script_content =
        "#!/usr/bin/env bash\n"
        "\n"
        "exec $HOME/bake3/bake3 \"$@\"\n";

    size_t current_len = 0;
    char *current = bake_read_file(script_path, &current_len);
    if (current && !strcmp(current, script_content)) {
        ecs_os_free(current);
        return 0;
    }
    ecs_os_free(current);

    char *tmp_script = bake_join_path(ctx->bake_home, ".bake3-wrapper.sh");
    if (!tmp_script) {
        return -1;
    }

    if (bake_write_file(tmp_script, script_content) != 0) {
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

int bake_environment_setup(bake_context_t *ctx, const char *argv0) {
    if (bake_mkdirs(ctx->bake_home) != 0) {
        return -1;
    }

    char *dst = bake_join_path(ctx->bake_home, "bake3");
    if (!dst) {
        return -1;
    }

    char *src = NULL;
    if (argv0 && bake_path_exists(argv0)) {
        src = bake_strdup(argv0);
    } else {
        char *cwd = bake_getcwd();
        if (cwd) {
            src = bake_join_path(cwd, argv0);
            ecs_os_free(cwd);
        }
    }

    if (!src) {
        ecs_os_free(dst);
        return -1;
    }

    int rc = 0;
    if (!bake_path_equal_normalized(src, dst)) {
        rc = bake_copy_file(src, dst);
    }

    if (rc == 0) {
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

        printf("\n       Installation complete!\n\n");
    }

    ecs_os_free(src);
    ecs_os_free(dst);
    return rc;
}
