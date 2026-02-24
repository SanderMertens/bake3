#include "bake2/discovery.h"
#include "bake2/environment.h"
#include "bake2/os.h"

static int bake_should_skip_dir(const char *name, bool skip_special_dirs) {
    if (!strcmp(name, ".") || !strcmp(name, "..")) {
        return 1;
    }
    if (name[0] == '.') {
        return 1;
    }
    if (!strcmp(name, "tmp") || !strcmp(name, "target") || !strcmp(name, "out")) {
        return 1;
    }
    if (skip_special_dirs) {
        if (!strcmp(name, "test") || !strcmp(name, "tests")) {
            return 1;
        }
        if (!strcmp(name, "example") || !strcmp(name, "examples")) {
            return 1;
        }
    }
    return 0;
}

typedef struct bake_discovery_ctx_t {
    bake_context_t *ctx;
    int32_t discovered;
    bool skip_special_dirs;
} bake_discovery_ctx_t;

static int bake_discovery_visit(const bake_dir_entry_t *entry, void *ctx_ptr) {
    bake_discovery_ctx_t *ctx = ctx_ptr;

    if (entry->is_dir) {
        if (bake_should_skip_dir(entry->name, ctx->skip_special_dirs)) {
            return 1;
        }
        return 0;
    }

    if (strcmp(entry->name, "project.json")) {
        return 0;
    }

    if (strstr(entry->path, "/templates/") || strstr(entry->path, "\\templates\\") ||
        strstr(entry->path, "/examples/") || strstr(entry->path, "\\examples\\")) {
        return 0;
    }

    bake_project_cfg_t *cfg = ecs_os_calloc_t(bake_project_cfg_t);
    if (!cfg) {
        return -1;
    }
    bake_project_cfg_init(cfg);

    if (bake_project_cfg_load_file(entry->path, cfg) != 0) {
        bake_project_cfg_fini(cfg);
        ecs_os_free(cfg);
        B2_ERR("failed to parse %s", entry->path);
        return 0;
    }

    bake_model_add_project(ctx->ctx->world, cfg, false);
    ctx->discovered++;
    return 0;
}

int bake_discover_projects(bake_context_t *ctx, const char *start_path) {
    bool build_like_cmd =
        !strcmp(ctx->opts.command, "build") ||
        !strcmp(ctx->opts.command, "run") ||
        !strcmp(ctx->opts.command, "test") ||
        !strcmp(ctx->opts.command, "clean") ||
        !strcmp(ctx->opts.command, "rebuild");

    bake_discovery_ctx_t discovery = {
        .ctx = ctx,
        .discovered = 0,
        .skip_special_dirs = build_like_cmd && ctx->opts.target == NULL
    };

    if (bake_dir_walk_recursive(start_path, bake_discovery_visit, &discovery) != 0) {
        return -1;
    }

    if (bake_environment_import_projects(ctx) < 0) {
        return -1;
    }

    bake_model_link_dependencies(ctx->world);

    if (bake_environment_save(ctx) != 0) {
        B2_ERR("warning: failed to save bake environment");
    }

    return discovery.discovered;
}
