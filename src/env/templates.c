#include "bake/environment.h"
#include "bake/os.h"
#include "env_internal.h"

static const char *bake_env_required_test_templates[] = {
    "bake_test.h",
    "bake_test.c",
    "bake_test_runtime.h",
    "bake_test_runtime.c"
};

bool bake_env_has_required_test_templates(const char *dir, const char **missing_out) {
    if (missing_out) {
        *missing_out = NULL;
    }

    if (!dir || !bake_path_exists(dir) || !bake_path_is_dir(dir)) {
        if (missing_out) {
            *missing_out = "<directory>";
        }
        return false;
    }

    size_t template_count =
        sizeof(bake_env_required_test_templates) / sizeof(bake_env_required_test_templates[0]);
    for (size_t i = 0; i < template_count; i++) {
        const char *name = bake_env_required_test_templates[i];
        char *path = bake_path_join(dir, name);
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

int bake_env_copy_tree_recursive(const char *src, const char *dst) {
    bake_dir_entry_t *entries = NULL;
    int32_t count = 0;
    if (bake_dir_list(src, &entries, &count) != 0) {
        return -1;
    }

    for (int32_t i = 0; i < count; i++) {
        bake_dir_entry_t *entry = &entries[i];
        if (bake_is_dot_dir(entry->name)) {
            continue;
        }

        char *dst_path = bake_path_join(dst, entry->name);
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
            rc = bake_file_copy(entry->path, dst_path);
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

int bake_env_copy_tree_exact(const char *src, const char *dst) {
    if (bake_env_remove_if_exists(dst) != 0) {
        return -1;
    }

    if (!src || !bake_path_exists(src) || !bake_path_is_dir(src)) {
        return 0;
    }

    if (bake_mkdirs(dst) != 0) {
        return -1;
    }

    return bake_env_copy_tree_recursive(src, dst);
}

static char* bake_env_test_templates_from_home(const char *home) {
    if (!home || !home[0]) {
        return NULL;
    }

    char *test_dir = bake_path_join(home, "test");
    if (test_dir && bake_env_has_required_test_templates(test_dir, NULL)) {
        return test_dir;
    }

    ecs_os_free(test_dir);
    return NULL;
}

char* bake_env_find_test_template_source(void) {
    char *cwd = bake_getcwd();
    char *cwd_templates = cwd ? bake_path_join(cwd, "templates/test_harness") : NULL;
    ecs_os_free(cwd);
    if (cwd_templates && bake_env_has_required_test_templates(cwd_templates, NULL)) {
        return cwd_templates;
    }
    ecs_os_free(cwd_templates);

    const char *exe_path = getenv("BAKE2_EXEC_PATH");
    if (exe_path && exe_path[0]) {
        char *exe_dir = bake_dirname(exe_path);
        char *exe_test_templates = bake_env_test_templates_from_home(exe_dir);
        if (exe_test_templates) {
            ecs_os_free(exe_dir);
            return exe_test_templates;
        }

        char *root_dir = exe_dir ? bake_dirname(exe_dir) : NULL;
        char *root_templates = root_dir ?
            bake_path_join(root_dir, "templates/test_harness") : NULL;

        ecs_os_free(exe_dir);
        ecs_os_free(root_dir);

        if (root_templates && bake_env_has_required_test_templates(root_templates, NULL)) {
            return root_templates;
        }
        ecs_os_free(root_templates);
    }

    const char *global_home = getenv("BAKE_GLOBAL_HOME");
    char *global_templates = bake_env_test_templates_from_home(global_home);
    if (global_templates) {
        return global_templates;
    }

    char *home = bake_home_path();
    char *default_home = home ? bake_path_join(home, "bake3") : NULL;
    ecs_os_free(home);

    char *default_templates = bake_env_test_templates_from_home(default_home);
    ecs_os_free(default_home);
    if (default_templates) {
        return default_templates;
    }

    return NULL;
}

static bool bake_env_local_mode_enabled(const bake_context_t *ctx) {
    if (ctx && ctx->opts.local_env) {
        return true;
    }

    const char *local_env = getenv("BAKE_LOCAL_ENV");
    return local_env && !strcmp(local_env, "1");
}

int bake_env_ensure_local_test_templates(const bake_context_t *ctx) {
    if (!bake_env_local_mode_enabled(ctx)) {
        return 0;
    }

    char *test_dst = bake_path_join(ctx->bake_home, "test");
    if (!test_dst) {
        return -1;
    }

    if (bake_env_has_required_test_templates(test_dst, NULL)) {
        ecs_os_free(test_dst);
        return 0;
    }

    char *test_src = bake_env_find_test_template_source();
    if (!test_src) {
        const char *missing = NULL;
        bake_env_has_required_test_templates(test_dst, &missing);
        ecs_err(
            "failed to initialize local test harness templates at %s (missing %s); expected templates/test_harness in current working directory, test templates next to bake executable, BAKE_GLOBAL_HOME/test, or ~/bake3/test",
            test_dst,
            missing ? missing : "<unknown>");
        ecs_os_free(test_dst);
        return -1;
    }

    int rc = bake_env_copy_tree_exact(test_src, test_dst);
    if (rc != 0) {
        ecs_err("failed to install local test harness templates from %s to %s", test_src, test_dst);
    }

    ecs_os_free(test_src);
    ecs_os_free(test_dst);
    return rc;
}
