#include "bake/environment.h"
#include "bake/os.h"

static bool bake_env_is_dot(const char *name) {
    return !strcmp(name, ".") || !strcmp(name, "..");
}

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

static int bake_env_copy_tree_recursive(const char *src, const char *dst) {
    bake_dir_entry_t *entries = NULL;
    int32_t count = 0;
    if (bake_dir_list(src, &entries, &count) != 0) {
        return -1;
    }

    for (int32_t i = 0; i < count; i++) {
        const bake_dir_entry_t *entry = &entries[i];
        if (bake_env_is_dot(entry->name)) {
            continue;
        }

        char *dst_path = bake_join_path(dst, entry->name);
        if (!dst_path) {
            bake_dir_entries_free(entries, count);
            return -1;
        }

        int rc = 0;
        if (entry->is_dir) {
            rc = bake_mkdirs(dst_path);
            if (rc == 0) {
                rc = bake_env_copy_tree_recursive(entry->path, dst_path);
            }
        } else {
            rc = bake_copy_file(entry->path, dst_path);
        }

        ecs_os_free(dst_path);
        if (rc != 0) {
            bake_dir_entries_free(entries, count);
            return -1;
        }
    }

    bake_dir_entries_free(entries, count);
    return 0;
}

static int bake_env_copy_tree_exact(const char *src, const char *dst) {
    if (!src || !dst || !bake_path_exists(src) || !bake_is_dir(src)) {
        return -1;
    }

    if (bake_path_exists(dst) && bake_remove_tree(dst) != 0) {
        return -1;
    }

    if (bake_mkdirs(dst) != 0) {
        return -1;
    }

    return bake_env_copy_tree_recursive(src, dst);
}

static const char *bake_env_required_test_templates[] = {
    "bake_test.h",
    "bake_test.c",
    "bake_test_runtime.h",
    "bake_test_runtime.c"
};

static bool bake_env_has_required_test_templates(const char *dir, const char **missing_out) {
    if (missing_out) {
        *missing_out = NULL;
    }

    if (!dir || !bake_path_exists(dir) || !bake_is_dir(dir)) {
        if (missing_out) {
            *missing_out = "<directory>";
        }
        return false;
    }

    size_t template_count =
        sizeof(bake_env_required_test_templates) / sizeof(bake_env_required_test_templates[0]);
    for (size_t i = 0; i < template_count; i++) {
        const char *name = bake_env_required_test_templates[i];
        char *path = bake_join_path(dir, name);
        bool exists = path && bake_path_exists(path);
        ecs_os_free(path);

        if (!exists) {
            if (missing_out) {
                *missing_out = name;
            }
            return false;
        }
    }

    return true;
}

static char* bake_env_executable_path(const char *argv0) {
    if (!argv0 || !argv0[0]) {
        return NULL;
    }

    if (bake_path_exists(argv0)) {
        return bake_strdup(argv0);
    }

    char *cwd = bake_getcwd();
    char *candidate = cwd ? bake_join_path(cwd, argv0) : NULL;
    ecs_os_free(cwd);
    if (candidate && bake_path_exists(candidate)) {
        return candidate;
    }

    ecs_os_free(candidate);
    return NULL;
}

static char* bake_env_find_test_template_source(const bake_context_t *ctx, const char *exe_path) {
    const char *missing = NULL;

    char *cwd = bake_getcwd();
    char *cwd_templates = cwd ? bake_join_path(cwd, "templates/test_harness") : NULL;
    ecs_os_free(cwd);
    if (cwd_templates && bake_env_has_required_test_templates(cwd_templates, NULL)) {
        return cwd_templates;
    }
    ecs_os_free(cwd_templates);

    if (exe_path && exe_path[0]) {
        char *exe_dir = bake_dirname(exe_path);
        char *root_dir = exe_dir ? bake_dirname(exe_dir) : NULL;
        char *root_templates = root_dir ? bake_join_path(root_dir, "templates/test_harness") : NULL;

        ecs_os_free(exe_dir);
        ecs_os_free(root_dir);

        if (root_templates && bake_env_has_required_test_templates(root_templates, NULL)) {
            return root_templates;
        }
        ecs_os_free(root_templates);
    }

    char *installed_templates = bake_join_path(ctx->bake_home, "test");
    if (installed_templates && bake_env_has_required_test_templates(installed_templates, NULL)) {
        return installed_templates;
    }
    ecs_os_free(installed_templates);

    char *expected = bake_join_path(ctx->bake_home, "test");
    if (expected) {
        bake_env_has_required_test_templates(expected, &missing);
        ecs_err(
            "failed to find test harness templates; expected templates/test_harness next to source checkout, next to executable, or existing BAKE_HOME templates at %s (missing %s)",
            expected,
            missing ? missing : "<unknown>");
    } else {
        ecs_err("failed to find test harness templates");
    }
    ecs_os_free(expected);
    return NULL;
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

    char *src = bake_env_executable_path(argv0);
    char *test_dst = bake_join_path(ctx->bake_home, "test");

    if (!src || !test_dst) {
        ecs_err("failed to resolve setup paths");
        ecs_os_free(dst);
        ecs_os_free(src);
        ecs_os_free(test_dst);
        return -1;
    }

    int rc = 0;
    if (!bake_path_equal_normalized(src, dst)) {
        rc = bake_copy_file(src, dst);
    }

    if (rc == 0) {
        char *test_src = bake_env_find_test_template_source(ctx, src);
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
