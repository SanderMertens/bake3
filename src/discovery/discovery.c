#include "bake2/discovery.h"
#include "bake2/environment.h"
#include "bake2/os.h"

static int b2_should_skip_dir(const char *name) {
    if (!strcmp(name, ".") || !strcmp(name, "..")) {
        return 1;
    }
    if (name[0] == '.') {
        return 1;
    }
    if (!strcmp(name, "build") || !strncmp(name, "build-", 6)) {
        return 1;
    }
    if (!strcmp(name, "tmp") || !strcmp(name, "target") || !strcmp(name, "out")) {
        return 1;
    }
    return 0;
}

typedef struct b2_discovery_ctx_t {
    b2_context_t *ctx;
    int32_t discovered;
} b2_discovery_ctx_t;

static int b2_discovery_visit(const b2_dir_entry_t *entry, void *ctx_ptr) {
    b2_discovery_ctx_t *ctx = ctx_ptr;

    if (entry->is_dir) {
        if (b2_should_skip_dir(entry->name)) {
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

    b2_project_cfg_t *cfg = ecs_os_calloc_t(b2_project_cfg_t);
    if (!cfg) {
        return -1;
    }
    b2_project_cfg_init(cfg);

    if (b2_project_cfg_load_file(entry->path, cfg) != 0) {
        b2_project_cfg_fini(cfg);
        ecs_os_free(cfg);
        B2_ERR("failed to parse %s", entry->path);
        return 0;
    }

    b2_model_add_project(ctx->ctx->world, cfg, false);
    ctx->discovered++;
    return 0;
}

int b2_discover_projects(b2_context_t *ctx, const char *start_path) {
    b2_discovery_ctx_t discovery = {
        .ctx = ctx,
        .discovered = 0
    };

    if (b2_dir_walk_recursive(start_path, b2_discovery_visit, &discovery) != 0) {
        return -1;
    }

    b2_model_link_dependencies(ctx->world);

    if (b2_environment_save(ctx) != 0) {
        B2_ERR("warning: failed to save bake environment");
    }

    return discovery.discovered;
}
