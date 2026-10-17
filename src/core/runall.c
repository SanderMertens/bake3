#include "bake/commands.h"
#include "bake/os.h"

static int bake_runall_cmp_entry(const void *a, const void *b) {
    const bake_dir_entry_t *lhs = a;
    const bake_dir_entry_t *rhs = b;
    return strcmp(lhs->name, rhs->name);
}

static bool bake_runall_is_runnable(const char *project_dir) {
    char *project_json = bake_path_join(project_dir, "project.json");
    bool runnable = false;
    if (bake_path_exists(project_json)) {
        bake_project_cfg_t cfg;
        bake_project_cfg_init(&cfg);
        if (bake_project_cfg_load_file(project_json, &cfg) != 0) {
            runnable = true;
        } else {
            runnable = cfg.kind == BAKE_PROJECT_APPLICATION ||
                bake_project_kind_is_harness(cfg.kind);
        }
        bake_project_cfg_fini(&cfg);
    }
    ecs_os_free(project_json);
    return runnable;
}

static const char** bake_runall_child_argv(
    const bake_options_t *opts,
    int argc,
    char *argv[],
    const char *exe,
    const char *project)
{
    const char **child = ecs_os_calloc_n(const char*, argc + 3);
    int32_t count = 0;
    child[count++] = exe;

    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--")) {
            break;
        }
        if (argv[i] == opts->command) {
            child[count++] = "run";
            if (!opts->target) {
                child[count++] = project;
            }
            continue;
        }
        if (argv[i] == opts->target) {
            child[count++] = project;
            continue;
        }
        child[count++] = argv[i];
    }

    for (; i < argc; i++) {
        child[count++] = argv[i];
    }

    child[count] = NULL;
    return child;
}

static char* bake_runall_exe(char *argv[]) {
    const char *exe = getenv("BAKE3_EXEC_PATH");
    if (exe && exe[0]) {
        return ecs_os_strdup(exe);
    }
    return ecs_os_strdup(argv[0]);
}

int bake_runall(const bake_options_t *opts, int argc, char *argv[]) {
    const char *target = opts->target ? opts->target : ".";
    char *dir = bake_path_is_abs(target)
        ? ecs_os_strdup(target)
        : bake_path_join(opts->cwd, target);

    if (!bake_path_is_dir(dir)) {
        ecs_err("runall target is not a directory: %s", target);
        ecs_os_free(dir);
        return -1;
    }

    bake_dir_entry_t *entries = NULL;
    int32_t entry_count = 0;
    if (bake_dir_list(dir, &entries, &entry_count) != 0) {
        ecs_err("failed to list directory: %s", target);
        ecs_os_free(dir);
        return -1;
    }

    qsort(entries, (size_t)entry_count, sizeof(bake_dir_entry_t),
        bake_runall_cmp_entry);

    char *exe = bake_runall_exe(argv);
    bake_strlist_t failed;
    bake_strlist_init(&failed);
    int32_t run_count = 0;
    bool interrupted = false;

    for (int32_t i = 0; i < entry_count; i++) {
        bake_dir_entry_t *entry = &entries[i];
        if (!entry->is_dir || entry->name[0] == '.') {
            continue;
        }
        if (!bake_runall_is_runnable(entry->path)) {
            continue;
        }

        char *project = opts->target
            ? bake_path_join(opts->target, entry->name)
            : ecs_os_strdup(entry->name);
        const char **child = bake_runall_child_argv(
            opts, argc, argv, exe, project);

        printf("[ runall] %s\n", project);

        bake_process_result_t result = {0};
        int spawn_rc = bake_proc_run_argv(child, &result);
        ecs_os_free(child);
        run_count++;

        if (spawn_rc != 0 || result.exit_code != 0 || result.interrupted) {
            bake_strlist_append_owned(&failed, project);
        } else {
            ecs_os_free(project);
        }

        if (result.interrupted) {
            interrupted = true;
            break;
        }
    }

    if (!run_count) {
        ecs_err("no runnable projects found in %s", target);
    } else if (failed.count) {
        char *joined = bake_strlist_join(&failed, ", ");
        printf("[ runall] %d of %d project%s failed%s: %s\n",
            failed.count, run_count, run_count == 1 ? "" : "s",
            interrupted ? " (interrupted)" : "", joined);
        ecs_os_free(joined);
    } else {
        printf("[ runall] %d project%s passed\n",
            run_count, run_count == 1 ? "" : "s");
    }

    int rc = (run_count && !failed.count) ? 0 : -1;
    bake_strlist_fini(&failed);
    ecs_os_free(exe);
    bake_dir_entries_free(entries, entry_count);
    ecs_os_free(dir);
    return rc;
}
