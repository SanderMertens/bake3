#include "bake/commands.h"
#include "bake/discovery.h"
#include "bake/environment.h"
#include "bake/os.h"

static const char *bake_help_text =
    "Usage: bake [options] [command] [target]\n"
    "\n"
    "Commands:\n"
    "  build [target]      Build target project and dependencies (default)\n"
    "  run [target]        Build and run executable target\n"
    "  test [target]       Build and run test target\n"
    "  clean [target]      Remove build artifacts\n"
    "  rebuild [target]    Clean and build\n"
    "  list                List projects in bake environment\n"
    "  info <target>       Show project info\n"
    "  cleanup             Remove stale projects from bake environment\n"
    "  reset               Reset bake environment metadata\n"
    "  setup               Install bake executable into bake environment\n"
    "\n"
    "Options:\n"
    "  --cfg <mode>        Build mode: sanitize|debug|profile|release\n"
    "  --cc <compiler>     Override C compiler\n"
    "  --cxx <compiler>    Override C++ compiler\n"
    "  --run-prefix <cmd>  Prefix command when running binaries\n"
    "  --local-env[=<name>] Use ./.bake/local_env (or ./.bake/local_env/<name>) as isolated BAKE_HOME and build root\n"
    "  --local             Setup only: install into BAKE_HOME (skip /usr/local/bin)\n"
    "  --standalone        Use amalgamated dependency sources in deps/\n"
    "  --strict            Enable strict compiler warnings and checks\n"
    "  --trace             Enable trace logging (Flecs log level 0)\n"
    "  -j <count>          Number of parallel jobs for build/test execution\n"
    "  -r                  Recursive clean/rebuild\n"
    "  -h, --help          Show this help\n";

void bake_print_help(void) {
    fputs(bake_help_text, stdout);
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

static void bake_list_project_fini(bake_list_project_t *project) {
    ecs_os_free(project->id);
    bake_strlist_fini(&project->cfgs);
}

static void bake_list_template_fini(bake_list_template_t *template_info) {
    ecs_os_free(template_info->id);
    bake_strlist_fini(&template_info->entries);
}

#define BAKE_VEC_FINALIZE(T, vec, cmp_fn, out_ptr, count_ptr) do { \
    int32_t _c = ecs_vec_count(&(vec)); \
    if (_c > 1) qsort(ecs_vec_first(&(vec)), (size_t)_c, sizeof(T), (cmp_fn)); \
    if (_c > 0) { \
        T *_p = ecs_os_malloc_n(T, _c); \
        if (!_p) goto cleanup; \
        ecs_os_memcpy_n(_p, ecs_vec_first_t(&(vec), T), T, _c); \
        *(out_ptr) = _p; \
    } else *(out_ptr) = NULL; \
    ecs_vec_fini_t(NULL, &(vec), T); \
    *(count_ptr) = _c; \
} while (0)

#define BAKE_VEC_DROP_ITEMS(T, vec, fini_fn) do { \
    T *_v = ecs_vec_first_t(&(vec), T); \
    int32_t _vc = ecs_vec_count(&(vec)); \
    for (int32_t _i = 0; _i < _vc; _i++) (fini_fn)(&_v[_i]); \
    ecs_vec_fini_t(NULL, &(vec), T); \
} while (0)

static void bake_list_projects_free(bake_list_project_t *projects, int32_t count) {
    for (int32_t i = 0; i < count; i++) {
        bake_list_project_fini(&projects[i]);
    }
    ecs_os_free(projects);
}

static void bake_list_templates_free(bake_list_template_t *templates, int32_t count) {
    for (int32_t i = 0; i < count; i++) {
        bake_list_template_fini(&templates[i]);
    }
    ecs_os_free(templates);
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

    int rc = -1;
    bake_dir_entry_t *cfg_dirs = NULL;
    int32_t cfg_count = 0;
    char *artefact_name = bake_project_cfg_artefact_name(cfg);
    if (!artefact_name) {
        return 0;
    }

    if (!bake_path_exists(platform_dir) || !bake_path_is_dir(platform_dir)) {
        rc = 0;
        goto cleanup;
    }

    if (bake_dir_list(platform_dir, &cfg_dirs, &cfg_count) != 0) {
        goto cleanup;
    }

    const char *subdir = cfg->kind == BAKE_PROJECT_PACKAGE ? "lib" : "bin";
    for (int32_t i = 0; i < cfg_count; i++) {
        const bake_dir_entry_t *cfg_dir = &cfg_dirs[i];
        if (!cfg_dir->is_dir || bake_is_dot_dir(cfg_dir->name)) {
            continue;
        }
        char *base = bake_path_join(cfg_dir->path, subdir);
        char *artefact_path = base ? bake_path_join(base, artefact_name) : NULL;
        ecs_os_free(base);
        if (!artefact_path) goto cleanup;
        int append_rc = bake_path_exists(artefact_path)
            ? bake_strlist_append_unique(cfgs_out, cfg_dir->name) : 0;
        ecs_os_free(artefact_path);
        if (append_rc != 0) goto cleanup;
    }

    if (cfgs_out->count > 1) {
        qsort(cfgs_out->items, (size_t)cfgs_out->count, sizeof(char*), bake_list_cmp_string_ptr);
    }
    rc = 0;

cleanup:
    bake_dir_entries_free(cfg_dirs, cfg_count);
    ecs_os_free(artefact_name);
    if (rc != 0) bake_strlist_fini(cfgs_out);
    return rc;
}

static int bake_list_collect_projects(
    const bake_context_t *ctx,
    const char *platform_dir,
    bake_list_project_t **projects_out,
    int32_t *count_out)
{
    *projects_out = NULL;
    *count_out = 0;

    int rc = -1;
    bake_dir_entry_t *entries = NULL;
    int32_t entry_count = 0;
    ecs_vec_t vec = {0};
    char *meta_dir = bake_path_join(ctx->bake_home, "meta");
    if (!meta_dir) {
        goto cleanup;
    }

    if (!bake_path_exists(meta_dir) || !bake_path_is_dir(meta_dir)) {
        rc = 0;
        goto cleanup;
    }

    if (bake_dir_list(meta_dir, &entries, &entry_count) != 0) {
        goto cleanup;
    }

    for (int32_t i = 0; i < entry_count; i++) {
        const bake_dir_entry_t *entry = &entries[i];
        if (!entry->is_dir || bake_is_dot_dir(entry->name)) {
            continue;
        }

        char *project_json = bake_path_join(entry->path, "project.json");
        if (!project_json || !bake_path_exists(project_json)) {
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
            goto cleanup;
        }

        bool include = cfg.kind == BAKE_PROJECT_CONFIG || cfgs.count > 0;
        if (!include || cfg.kind == BAKE_PROJECT_TEMPLATE) {
            bake_strlist_fini(&cfgs);
            bake_project_cfg_fini(&cfg);
            continue;
        }

        bake_list_project_t *project = ecs_vec_append_t(NULL, &vec, bake_list_project_t);
        project->id = ecs_os_strdup(entry->name);
        project->kind = cfg.kind;
        project->cfgs = cfgs;
        if (!project->id) {
            bake_strlist_fini(&project->cfgs);
            ecs_vec_remove_last(&vec);
            bake_project_cfg_fini(&cfg);
            goto cleanup;
        }
        bake_project_cfg_fini(&cfg);
    }

    BAKE_VEC_FINALIZE(bake_list_project_t, vec, bake_list_cmp_project, projects_out, count_out);
    rc = 0;

cleanup:
    if (rc != 0) BAKE_VEC_DROP_ITEMS(bake_list_project_t, vec, bake_list_project_fini);
    bake_dir_entries_free(entries, entry_count);
    ecs_os_free(meta_dir);
    return rc;
}

static int bake_list_collect_templates(
    const bake_context_t *ctx,
    bake_list_template_t **templates_out,
    int32_t *count_out)
{
    *templates_out = NULL;
    *count_out = 0;

    int rc = -1;
    bake_dir_entry_t *entries = NULL;
    int32_t entry_count = 0;
    ecs_vec_t vec = {0};
    char *template_root = bake_path_join(ctx->bake_home, "template");
    if (!template_root) {
        goto cleanup;
    }

    if (!bake_path_exists(template_root) || !bake_path_is_dir(template_root)) {
        rc = 0;
        goto cleanup;
    }

    if (bake_dir_list(template_root, &entries, &entry_count) != 0) {
        goto cleanup;
    }

    for (int32_t i = 0; i < entry_count; i++) {
        const bake_dir_entry_t *entry = &entries[i];
        if (!entry->is_dir || bake_is_dot_dir(entry->name)) {
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
            if (!item->is_dir || bake_is_dot_dir(item->name)) {
                continue;
            }
            if (bake_strlist_append(&items, item->name) != 0) {
                bake_dir_entries_free(item_entries, item_count);
                bake_strlist_fini(&items);
                goto cleanup;
            }
        }
        bake_dir_entries_free(item_entries, item_count);

        if (items.count == 0) {
            bake_strlist_fini(&items);
            continue;
        }
        qsort(items.items, (size_t)items.count, sizeof(char*), bake_list_cmp_string_ptr);

        bake_list_template_t *tmpl = ecs_vec_append_t(NULL, &vec, bake_list_template_t);
        tmpl->id = ecs_os_strdup(entry->name);
        tmpl->entries = items;
        if (!tmpl->id) {
            bake_strlist_fini(&tmpl->entries);
            ecs_vec_remove_last(&vec);
            goto cleanup;
        }
    }

    BAKE_VEC_FINALIZE(bake_list_template_t, vec, bake_list_cmp_template, templates_out, count_out);
    rc = 0;

cleanup:
    if (rc != 0) BAKE_VEC_DROP_ITEMS(bake_list_template_t, vec, bake_list_template_fini);
    bake_dir_entries_free(entries, entry_count);
    ecs_os_free(template_root);
    return rc;
}

static int bake_list_projects(bake_context_t *ctx) {
    char *platform = bake_host_platform();
    if (!platform) {
        return -1;
    }

    char *platform_dir = bake_path_join(ctx->bake_home, platform);
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
        bake_list_projects_free(projects, project_count);
        ecs_os_free(platform_dir);
        ecs_os_free(platform);
        return -1;
    }

    int32_t package_count = 0;
    int32_t application_count = 0;

    printf("Listing projects in %s for platforms:\n", ctx->bake_home);
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

    bake_list_projects_free(projects, project_count);
    bake_list_templates_free(templates, template_count);
    ecs_os_free(platform_dir);
    ecs_os_free(platform);

    return 0;
}

static void bake_print_cfg_strlist(
    const char *label,
    const bake_strlist_t *values,
    int width)
{
    if (!values || !values->count) {
        return;
    }
    char *joined = bake_strlist_join(values, ", ");
    printf("%-*s %s\n", width, label, joined ? joined : "");
    ecs_os_free(joined);
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
    if (bake_path_is_abs(target)) {
        abs = ecs_os_strdup(target);
    } else {
        abs = bake_path_join(ctx->opts.cwd, target);
    }
    if (!abs) {
        return NULL;
    }

    project = bake_model_find_project_by_path(ctx->world, abs, entity_out);
    ecs_os_free(abs);
    return project;
}

static int bake_info_project(bake_context_t *ctx) {
    if (bake_discover_projects(ctx, ctx->opts.cwd, false) < 0) {
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
    printf("kind:        %s\n", bake_project_kind_str(cfg->kind));
    printf("path:        %s\n", cfg->path ? cfg->path : "<external>");
    printf("language:    %s\n", cfg->language ? cfg->language : "<none>");
    printf("output:      %s\n", cfg->output_name ? cfg->output_name : "<none>");
    printf("external:    %s\n", project->external ? "true" : "false");

    bake_print_cfg_strlist("use:", &cfg->use, 12);
    bake_print_cfg_strlist("use-private:", &cfg->use_private, 12);
    bake_print_cfg_strlist("use-build:", &cfg->use_build, 12);
    bake_print_cfg_strlist("use-runtime:", &cfg->use_runtime, 12);

    return 0;
}

int bake_execute(bake_context_t *ctx, const char *argv0) {
    const char *cmd = ctx->opts.command;
    if (!cmd) {
        return bake_build(ctx);
    }

    static const struct { const char *name; int (*fn)(bake_context_t*); } table[] = {
        {"build", bake_build}, {"run", bake_build_run},
        {"clean", bake_build_clean}, {"rebuild", bake_build_rebuild},
        {"list", bake_list_projects}, {"info", bake_info_project},
        {"reset", bake_env_reset},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (!strcmp(cmd, table[i].name)) return table[i].fn(ctx);
    }

    if (!strcmp(cmd, "cleanup")) {
        int32_t removed = 0;
        int rc = bake_env_cleanup(ctx, &removed);
        if (rc == 0) {
            ecs_trace("removed %d stale project(s) from bake environment", removed);
        }
        return rc;
    }

    if (!strcmp(cmd, "setup")) {
        return bake_env_setup(ctx, argv0);
    }

    if (!strcmp(cmd, "help") || !strcmp(cmd, "--help") || !strcmp(cmd, "-h")) {
        bake_print_help();
        return 0;
    }

    ecs_err("unknown command: %s", cmd);
    bake_print_help();
    return -1;
}
