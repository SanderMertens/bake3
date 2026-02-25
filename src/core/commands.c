#include "bake2/commands.h"
#include "bake2/discovery.h"
#include "bake2/environment.h"
#include "bake2/os.h"

void bake_print_help(void) {
    printf("Usage: bake [options] [command] [target]\n");
    printf("\n");
    printf("Commands:\n");
    printf("  build [target]      Build target project and dependencies (default)\n");
    printf("  run [target]        Build and run executable target\n");
    printf("  test [target]       Build and run test target\n");
    printf("  clean [target]      Remove build artifacts\n");
    printf("  rebuild [target]    Clean and build\n");
    printf("  list                List projects in bake environment\n");
    printf("  info <target>       Show project info\n");
    printf("  cleanup             Remove stale projects from bake environment\n");
    printf("  reset               Reset bake environment metadata\n");
    printf("  setup               Install bake executable into bake environment\n");
    printf("\n");
    printf("Options:\n");
    printf("  --cfg <mode>        Build mode: sanitize|debug|profile|release\n");
    printf("  --cc <compiler>     Override C compiler\n");
    printf("  --cxx <compiler>    Override C++ compiler\n");
    printf("  --run-prefix <cmd>  Prefix command when running binaries\n");
    printf("  --standalone        Use amalgamated dependency sources in deps/\n");
    printf("  --strict            Enable strict compiler warnings and checks\n");
    printf("  --trace             Enable trace logging (Flecs log level 0)\n");
    printf("  -j <count>          Number of parallel jobs for build/test execution\n");
    printf("  -r                  Recursive clean/rebuild\n");
    printf("  -h, --help          Show this help\n");
}

static const char* bake_kind_label(bake_project_kind_t kind) {
    switch (kind) {
    case BAKE_PROJECT_APPLICATION: return "application";
    case BAKE_PROJECT_PACKAGE: return "library";
    case BAKE_PROJECT_CONFIG: return "config";
    case BAKE_PROJECT_TEST: return "test";
    case BAKE_PROJECT_TEMPLATE: return "template";
    default: return "application";
    }
}

typedef struct bake_list_project_t {
    char *id;
    bake_project_kind_t kind;
    bake_strlist_t cfgs;
} bake_list_project_t;

typedef struct bake_list_template_t {
    char *id;
    bake_strlist_t entries;
} bake_list_template_t;

static int bake_list_cmp_string_ptr(const void *a, const void *b) {
    const char *lhs = *(const char* const*)a;
    const char *rhs = *(const char* const*)b;
    return strcmp(lhs, rhs);
}

static int bake_list_cmp_project(const void *a, const void *b) {
    const bake_list_project_t *lhs = a;
    const bake_list_project_t *rhs = b;
    return strcmp(lhs->id, rhs->id);
}

static int bake_list_cmp_template(const void *a, const void *b) {
    const bake_list_template_t *lhs = a;
    const bake_list_template_t *rhs = b;
    return strcmp(lhs->id, rhs->id);
}

static char* bake_list_artefact_name(const bake_project_cfg_t *cfg) {
    if (cfg->kind != BAKE_PROJECT_PACKAGE &&
        cfg->kind != BAKE_PROJECT_APPLICATION &&
        cfg->kind != BAKE_PROJECT_TEST)
    {
        return NULL;
    }

#if defined(_WIN32)
    const char *exe_ext = ".exe";
    const char *lib_ext = ".lib";
    const char *lib_prefix = "";
#else
    const char *exe_ext = "";
    const char *lib_ext = ".a";
    const char *lib_prefix = "lib";
#endif

    if (cfg->kind == BAKE_PROJECT_PACKAGE) {
        return bake_asprintf("%s%s%s", lib_prefix, cfg->output_name, lib_ext);
    }

    return bake_asprintf("%s%s", cfg->output_name, exe_ext);
}

static void bake_list_project_fini(bake_list_project_t *project) {
    ecs_os_free(project->id);
    bake_strlist_fini(&project->cfgs);
}

static void bake_list_template_fini(bake_list_template_t *template_info) {
    ecs_os_free(template_info->id);
    bake_strlist_fini(&template_info->entries);
}

static int bake_list_collect_cfgs(
    const char *platform_dir,
    const bake_project_cfg_t *cfg,
    bake_strlist_t *cfgs_out)
{
    bake_strlist_init(cfgs_out);

    if (cfg->kind == BAKE_PROJECT_CONFIG) {
        return bake_strlist_append(cfgs_out, "all");
    }

    char *artefact_name = bake_list_artefact_name(cfg);
    if (!artefact_name) {
        return 0;
    }

    if (!bake_path_exists(platform_dir) || !bake_is_dir(platform_dir)) {
        ecs_os_free(artefact_name);
        return 0;
    }

    bake_dir_entry_t *cfg_dirs = NULL;
    int32_t cfg_count = 0;
    if (bake_dir_list(platform_dir, &cfg_dirs, &cfg_count) != 0) {
        ecs_os_free(artefact_name);
        return -1;
    }

    const char *subdir = cfg->kind == BAKE_PROJECT_PACKAGE ? "lib" : "bin";
    for (int32_t i = 0; i < cfg_count; i++) {
        const bake_dir_entry_t *cfg_dir = &cfg_dirs[i];
        if (!cfg_dir->is_dir || !strcmp(cfg_dir->name, ".") || !strcmp(cfg_dir->name, "..")) {
            continue;
        }

        char *base = bake_join_path(cfg_dir->path, subdir);
        char *artefact_path = base ? bake_join_path(base, artefact_name) : NULL;
        ecs_os_free(base);
        if (!artefact_path) {
            bake_dir_entries_free(cfg_dirs, cfg_count);
            ecs_os_free(artefact_name);
            bake_strlist_fini(cfgs_out);
            return -1;
        }

        if (bake_path_exists(artefact_path) && !bake_strlist_contains(cfgs_out, cfg_dir->name)) {
            if (bake_strlist_append(cfgs_out, cfg_dir->name) != 0) {
                ecs_os_free(artefact_path);
                bake_dir_entries_free(cfg_dirs, cfg_count);
                ecs_os_free(artefact_name);
                bake_strlist_fini(cfgs_out);
                return -1;
            }
        }

        ecs_os_free(artefact_path);
    }

    bake_dir_entries_free(cfg_dirs, cfg_count);
    ecs_os_free(artefact_name);
    if (cfgs_out->count > 1) {
        qsort(cfgs_out->items, (size_t)cfgs_out->count, sizeof(char*), bake_list_cmp_string_ptr);
    }
    return 0;
}

static int bake_list_collect_projects(
    const bake_context_t *ctx,
    const char *platform_dir,
    bake_list_project_t **projects_out,
    int32_t *count_out)
{
    *projects_out = NULL;
    *count_out = 0;

    char *meta_dir = bake_join_path(ctx->bake_home, "meta");
    if (!meta_dir) {
        return -1;
    }

    if (!bake_path_exists(meta_dir) || !bake_is_dir(meta_dir)) {
        ecs_os_free(meta_dir);
        return 0;
    }

    bake_dir_entry_t *entries = NULL;
    int32_t entry_count = 0;
    if (bake_dir_list(meta_dir, &entries, &entry_count) != 0) {
        ecs_os_free(meta_dir);
        return -1;
    }

    int32_t count = 0;
    int32_t capacity = 16;
    bake_list_project_t *projects = ecs_os_calloc_n(bake_list_project_t, capacity);
    if (!projects) {
        bake_dir_entries_free(entries, entry_count);
        ecs_os_free(meta_dir);
        return -1;
    }

    for (int32_t i = 0; i < entry_count; i++) {
        const bake_dir_entry_t *entry = &entries[i];
        if (!entry->is_dir || !strcmp(entry->name, ".") || !strcmp(entry->name, "..")) {
            continue;
        }

        char *project_json = bake_join_path(entry->path, "project.json");
        if (!project_json) {
            continue;
        }
        if (!bake_path_exists(project_json)) {
            ecs_os_free(project_json);
            continue;
        }

        bake_project_cfg_t cfg;
        bake_project_cfg_init(&cfg);
        if (bake_project_cfg_load_file(project_json, &cfg) != 0) {
            bake_project_cfg_fini(&cfg);
            ecs_os_free(project_json);
            continue;
        }
        ecs_os_free(project_json);

        if (!cfg.public_project) {
            bake_project_cfg_fini(&cfg);
            continue;
        }

        bake_strlist_t cfgs;
        if (bake_list_collect_cfgs(platform_dir, &cfg, &cfgs) != 0) {
            bake_project_cfg_fini(&cfg);
            for (int32_t r = 0; r < count; r++) {
                bake_list_project_fini(&projects[r]);
            }
            ecs_os_free(projects);
            bake_dir_entries_free(entries, entry_count);
            ecs_os_free(meta_dir);
            return -1;
        }

        bool include = cfg.kind == BAKE_PROJECT_CONFIG || cfgs.count > 0;
        if (!include || cfg.kind == BAKE_PROJECT_TEMPLATE) {
            bake_strlist_fini(&cfgs);
            bake_project_cfg_fini(&cfg);
            continue;
        }

        if (count == capacity) {
            int32_t next = capacity * 2;
            bake_list_project_t *next_projects = ecs_os_realloc_n(projects, bake_list_project_t, next);
            if (!next_projects) {
                bake_strlist_fini(&cfgs);
                bake_project_cfg_fini(&cfg);
                for (int32_t r = 0; r < count; r++) {
                    bake_list_project_fini(&projects[r]);
                }
                ecs_os_free(projects);
                bake_dir_entries_free(entries, entry_count);
                ecs_os_free(meta_dir);
                return -1;
            }
            projects = next_projects;
            capacity = next;
        }

        projects[count].id = bake_strdup(entry->name);
        projects[count].kind = cfg.kind;
        projects[count].cfgs = cfgs;
        if (!projects[count].id) {
            bake_strlist_fini(&projects[count].cfgs);
            bake_project_cfg_fini(&cfg);
            for (int32_t r = 0; r < count; r++) {
                bake_list_project_fini(&projects[r]);
            }
            ecs_os_free(projects);
            bake_dir_entries_free(entries, entry_count);
            ecs_os_free(meta_dir);
            return -1;
        }
        count++;
        bake_project_cfg_fini(&cfg);
    }

    bake_dir_entries_free(entries, entry_count);
    ecs_os_free(meta_dir);

    if (count > 1) {
        qsort(projects, (size_t)count, sizeof(bake_list_project_t), bake_list_cmp_project);
    }

    *projects_out = projects;
    *count_out = count;
    return 0;
}

static int bake_list_collect_templates(
    const bake_context_t *ctx,
    bake_list_template_t **templates_out,
    int32_t *count_out)
{
    *templates_out = NULL;
    *count_out = 0;

    char *template_root = bake_join_path(ctx->bake_home, "template");
    if (!template_root) {
        return -1;
    }

    if (!bake_path_exists(template_root) || !bake_is_dir(template_root)) {
        ecs_os_free(template_root);
        return 0;
    }

    bake_dir_entry_t *entries = NULL;
    int32_t entry_count = 0;
    if (bake_dir_list(template_root, &entries, &entry_count) != 0) {
        ecs_os_free(template_root);
        return -1;
    }

    int32_t count = 0;
    int32_t capacity = 8;
    bake_list_template_t *templates = ecs_os_calloc_n(bake_list_template_t, capacity);
    if (!templates) {
        bake_dir_entries_free(entries, entry_count);
        ecs_os_free(template_root);
        return -1;
    }

    for (int32_t i = 0; i < entry_count; i++) {
        const bake_dir_entry_t *entry = &entries[i];
        if (!entry->is_dir || !strcmp(entry->name, ".") || !strcmp(entry->name, "..")) {
            continue;
        }

        bake_strlist_t items;
        bake_strlist_init(&items);

        bake_dir_entry_t *item_entries = NULL;
        int32_t item_count = 0;
        if (bake_dir_list(entry->path, &item_entries, &item_count) != 0) {
            bake_strlist_fini(&items);
            continue;
        }

        for (int32_t j = 0; j < item_count; j++) {
            const bake_dir_entry_t *item = &item_entries[j];
            if (!item->is_dir || !strcmp(item->name, ".") || !strcmp(item->name, "..")) {
                continue;
            }
            if (bake_strlist_append(&items, item->name) != 0) {
                bake_dir_entries_free(item_entries, item_count);
                bake_strlist_fini(&items);
                for (int32_t r = 0; r < count; r++) {
                    bake_list_template_fini(&templates[r]);
                }
                ecs_os_free(templates);
                bake_dir_entries_free(entries, entry_count);
                ecs_os_free(template_root);
                return -1;
            }
        }
        bake_dir_entries_free(item_entries, item_count);

        if (items.count == 0) {
            bake_strlist_fini(&items);
            continue;
        }
        qsort(items.items, (size_t)items.count, sizeof(char*), bake_list_cmp_string_ptr);

        if (count == capacity) {
            int32_t next = capacity * 2;
            bake_list_template_t *next_templates = ecs_os_realloc_n(templates, bake_list_template_t, next);
            if (!next_templates) {
                bake_strlist_fini(&items);
                for (int32_t r = 0; r < count; r++) {
                    bake_list_template_fini(&templates[r]);
                }
                ecs_os_free(templates);
                bake_dir_entries_free(entries, entry_count);
                ecs_os_free(template_root);
                return -1;
            }
            templates = next_templates;
            capacity = next;
        }

        templates[count].id = bake_strdup(entry->name);
        templates[count].entries = items;
        if (!templates[count].id) {
            bake_strlist_fini(&templates[count].entries);
            for (int32_t r = 0; r < count; r++) {
                bake_list_template_fini(&templates[r]);
            }
            ecs_os_free(templates);
            bake_dir_entries_free(entries, entry_count);
            ecs_os_free(template_root);
            return -1;
        }
        count++;
    }

    bake_dir_entries_free(entries, entry_count);
    ecs_os_free(template_root);

    if (count > 1) {
        qsort(templates, (size_t)count, sizeof(bake_list_template_t), bake_list_cmp_template);
    }

    *templates_out = templates;
    *count_out = count;
    return 0;
}

static int bake_list_projects(bake_context_t *ctx) {
    char *platform = bake_host_platform();
    if (!platform) {
        return -1;
    }

    char *platform_dir = bake_join_path(ctx->bake_home, platform);
    if (!platform_dir) {
        ecs_os_free(platform);
        return -1;
    }

    bake_list_project_t *projects = NULL;
    int32_t project_count = 0;
    if (bake_list_collect_projects(ctx, platform_dir, &projects, &project_count) != 0) {
        ecs_os_free(platform_dir);
        ecs_os_free(platform);
        return -1;
    }

    bake_list_template_t *templates = NULL;
    int32_t template_count = 0;
    if (bake_list_collect_templates(ctx, &templates, &template_count) != 0) {
        for (int32_t i = 0; i < project_count; i++) {
            bake_list_project_fini(&projects[i]);
        }
        ecs_os_free(projects);
        ecs_os_free(platform_dir);
        ecs_os_free(platform);
        return -1;
    }

    int32_t package_count = 0;
    int32_t application_count = 0;

    printf("Listing projects for platform:\n");
    printf(" * %s\n\n", platform);
    printf("Packages & Applications:\n");
    for (int32_t i = 0; i < project_count; i++) {
        const bake_list_project_t *project = &projects[i];
        char symbol = 'A';
        if (project->kind == BAKE_PROJECT_PACKAGE) {
            symbol = 'P';
            package_count++;
        } else if (project->kind == BAKE_PROJECT_CONFIG) {
            symbol = 'C';
        } else {
            symbol = 'A';
            application_count++;
        }

        char *cfg_joined = bake_strlist_join(&project->cfgs, ", ");
        printf("%c  %s => [%s]\n", symbol, project->id, cfg_joined ? cfg_joined : "");
        ecs_os_free(cfg_joined);
    }

    printf("\nTemplates:\n");
    for (int32_t i = 0; i < template_count; i++) {
        char *items_joined = bake_strlist_join(&templates[i].entries, ", ");
        printf("T  %s => [%s]\n", templates[i].id, items_joined ? items_joined : "");
        ecs_os_free(items_joined);
    }

    printf("\n\nSummary:\n");
    printf("applications: %d, packages: %d, templates: %d\n",
        application_count, package_count, template_count);

    for (int32_t i = 0; i < project_count; i++) {
        bake_list_project_fini(&projects[i]);
    }
    ecs_os_free(projects);

    for (int32_t i = 0; i < template_count; i++) {
        bake_list_template_fini(&templates[i]);
    }
    ecs_os_free(templates);
    ecs_os_free(platform_dir);
    ecs_os_free(platform);

    return 0;
}

static const BakeProject* bake_find_project_for_target(
    bake_context_t *ctx,
    const char *target,
    ecs_entity_t *entity_out)
{
    const BakeProject *project = bake_model_find_project(ctx->world, target, entity_out);
    if (project) {
        return project;
    }

    if (!bake_path_exists(target)) {
        return NULL;
    }

    char *abs = NULL;
    if (bake_os_path_is_abs(target)) {
        abs = bake_strdup(target);
    } else {
        abs = bake_join_path(ctx->opts.cwd, target);
    }
    if (!abs) {
        return NULL;
    }

    project = bake_model_find_project_by_path(ctx->world, abs, entity_out);
    ecs_os_free(abs);
    return project;
}

static int bake_info_project(bake_context_t *ctx) {
    if (bake_discover_projects(ctx, ctx->opts.cwd) < 0) {
        return -1;
    }

    if (!ctx->opts.target || !ctx->opts.target[0]) {
        ecs_err("info command requires a target");
        return -1;
    }

    ecs_entity_t entity = 0;
    const BakeProject *project = bake_find_project_for_target(ctx, ctx->opts.target, &entity);
    if (!project || !project->cfg) {
        ecs_err("target not found: %s", ctx->opts.target);
        return -1;
    }

    const bake_project_cfg_t *cfg = project->cfg;
    printf("id:          %s\n", cfg->id);
    printf("kind:        %s\n", bake_kind_label(cfg->kind));
    printf("path:        %s\n", cfg->path ? cfg->path : "<external>");
    printf("language:    %s\n", cfg->language ? cfg->language : "<none>");
    printf("output:      %s\n", cfg->output_name ? cfg->output_name : "<none>");
    printf("external:    %s\n", project->external ? "true" : "false");

    if (cfg->use.count) {
        char *joined = bake_strlist_join(&cfg->use, ", ");
        printf("use:         %s\n", joined ? joined : "");
        ecs_os_free(joined);
    }
    if (cfg->use_private.count) {
        char *joined = bake_strlist_join(&cfg->use_private, ", ");
        printf("use-private: %s\n", joined ? joined : "");
        ecs_os_free(joined);
    }
    if (cfg->use_build.count) {
        char *joined = bake_strlist_join(&cfg->use_build, ", ");
        printf("use-build:   %s\n", joined ? joined : "");
        ecs_os_free(joined);
    }
    if (cfg->use_runtime.count) {
        char *joined = bake_strlist_join(&cfg->use_runtime, ", ");
        printf("use-runtime: %s\n", joined ? joined : "");
        ecs_os_free(joined);
    }

    const bake_project_cfg_t *dependee_cfg = cfg->dependee.cfg;
    if (dependee_cfg && dependee_cfg->use.count) {
        char *joined = bake_strlist_join(&dependee_cfg->use, ", ");
        printf("dependee.use: %s\n", joined ? joined : "");
        ecs_os_free(joined);
    }
    if (dependee_cfg && dependee_cfg->c_lang.defines.count) {
        char *joined = bake_strlist_join(&dependee_cfg->c_lang.defines, ", ");
        printf("dependee.defines: %s\n", joined ? joined : "");
        ecs_os_free(joined);
    }
    if (dependee_cfg && dependee_cfg->c_lang.include_paths.count) {
        char *joined = bake_strlist_join(&dependee_cfg->c_lang.include_paths, ", ");
        printf("dependee.include: %s\n", joined ? joined : "");
        ecs_os_free(joined);
    }
    if (dependee_cfg && dependee_cfg->c_lang.cflags.count) {
        char *joined = bake_strlist_join(&dependee_cfg->c_lang.cflags, ", ");
        printf("dependee.cflags: %s\n", joined ? joined : "");
        ecs_os_free(joined);
    }
    if (dependee_cfg && dependee_cfg->c_lang.libs.count) {
        char *joined = bake_strlist_join(&dependee_cfg->c_lang.libs, ", ");
        printf("dependee.lib: %s\n", joined ? joined : "");
        ecs_os_free(joined);
    }

    return 0;
}

int bake_execute(bake_context_t *ctx, const char *argv0) {
    if (!ctx->opts.command || !strcmp(ctx->opts.command, "build")) {
        return bake_build_run(ctx);
    }

    if (!strcmp(ctx->opts.command, "run") || !strcmp(ctx->opts.command, "test")) {
        return bake_build_run(ctx);
    }

    if (!strcmp(ctx->opts.command, "clean")) {
        return bake_build_clean(ctx);
    }

    if (!strcmp(ctx->opts.command, "rebuild")) {
        return bake_build_rebuild(ctx);
    }

    if (!strcmp(ctx->opts.command, "list")) {
        return bake_list_projects(ctx);
    }

    if (!strcmp(ctx->opts.command, "info")) {
        return bake_info_project(ctx);
    }

    if (!strcmp(ctx->opts.command, "reset")) {
        return bake_environment_reset(ctx);
    }

    if (!strcmp(ctx->opts.command, "cleanup")) {
        int32_t removed = 0;
        int rc = bake_environment_cleanup(ctx, &removed);
        if (rc == 0) {
            ecs_trace("removed %d stale project(s) from bake environment", removed);
        }
        return rc;
    }

    if (!strcmp(ctx->opts.command, "setup")) {
        return bake_environment_setup(ctx, argv0);
    }

    if (!strcmp(ctx->opts.command, "help") || !strcmp(ctx->opts.command, "--help") || !strcmp(ctx->opts.command, "-h")) {
        bake_print_help();
        return 0;
    }

    ecs_err("unknown command: %s", ctx->opts.command);
    bake_print_help();
    return -1;
}
