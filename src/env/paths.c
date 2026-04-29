#include "bake/environment.h"
#include "bake/os.h"
#include "env_internal.h"

static char* bake_env_artefact_path_impl(
    const bake_context_t *ctx,
    const bake_project_cfg_t *cfg,
    const char *mode,
    bool scoped)
{
    if (!cfg || (scoped && (!cfg->id || !cfg->id[0]))) {
        return NULL;
    }

    char *out_path = NULL;
    char *file_name = NULL;
    char *platform = NULL;
    char *platform_dir = NULL;
    char *cfg_dir = NULL;
    char *out_dir = NULL;
    char *id_dir = NULL;

    file_name = bake_project_cfg_artefact_name(cfg);
    if (!file_name) {
        goto cleanup;
    }

    platform = bake_host_platform();
    platform_dir = platform ? bake_path_join(ctx->bake_home, platform) : NULL;
    cfg_dir = platform_dir ? bake_path_join(platform_dir, mode && mode[0] ? mode : "debug") : NULL;
    const char *subdir = cfg->kind == BAKE_PROJECT_PACKAGE ? "lib" : "bin";
    out_dir = cfg_dir ? bake_path_join(cfg_dir, subdir) : NULL;
    id_dir = scoped ? (out_dir ? bake_path_join(out_dir, cfg->id) : NULL) : NULL;
    out_path = scoped ? (id_dir ? bake_path_join(id_dir, file_name) : NULL) :
        (out_dir ? bake_path_join(out_dir, file_name) : NULL);

cleanup:
    ecs_os_free(file_name);
    ecs_os_free(platform);
    ecs_os_free(platform_dir);
    ecs_os_free(cfg_dir);
    ecs_os_free(out_dir);
    ecs_os_free(id_dir);
    return out_path;
}

char* bake_env_artefact_path(
    const bake_context_t *ctx,
    const bake_project_cfg_t *cfg,
    const char *mode)
{
    return bake_env_artefact_path_impl(ctx, cfg, mode, false);
}

char* bake_env_artefact_path_scoped(
    const bake_context_t *ctx,
    const bake_project_cfg_t *cfg,
    const char *mode)
{
    return bake_env_artefact_path_impl(ctx, cfg, mode, true);
}

char* bake_env_find_artefact_path_current_mode(
    const bake_context_t *ctx,
    const bake_project_cfg_t *cfg,
    const char *mode)
{
    char *scoped = bake_env_artefact_path_scoped(ctx, cfg, mode);
    if (scoped && bake_path_exists(scoped)) {
        return scoped;
    }
    ecs_os_free(scoped);

    char *legacy = bake_env_artefact_path(ctx, cfg, mode);
    if (legacy && bake_path_exists(legacy)) {
        return legacy;
    }
    ecs_os_free(legacy);
    return NULL;
}

char* bake_env_resolve_home_path(const char *env_home) {
    if (!env_home || !env_home[0]) {
        return NULL;
    }

    if (bake_path_is_abs(env_home)) {
        return ecs_os_strdup(env_home);
    }

    char *cwd = bake_os_getcwd();
    if (!cwd) {
        return NULL;
    }

    /* Use cwd-relative path as fallback, but prefer an existing ancestor match.
     * This keeps a relative BAKE_HOME stable when invoking bake from subdirs. */
    char *resolved = bake_path_join(cwd, env_home);
    if (!resolved) {
        ecs_os_free(cwd);
        return NULL;
    }

    char *probe = ecs_os_strdup(cwd);
    ecs_os_free(cwd);
    if (!probe) {
        return resolved;
    }

    while (probe[0]) {
        char *candidate = bake_path_join(probe, env_home);
        if (!candidate) {
            break;
        }

        if (bake_path_exists(candidate)) {
            ecs_os_free(resolved);
            resolved = candidate;
            candidate = NULL;
        }
        ecs_os_free(candidate);

        char *parent = bake_path_dirname(probe);
        if (parent && !parent[0] && bake_path_is_abs(probe)) {
            ecs_os_free(parent);
            parent = ecs_os_strdup("/");
        }

        if (!parent || !parent[0] || !strcmp(parent, ".") || !strcmp(parent, probe)) {
            ecs_os_free(parent);
            break;
        }

        ecs_os_free(probe);
        probe = parent;
    }

    ecs_os_free(probe);
    return resolved;
}

int bake_env_init_paths(bake_context_t *ctx) {
    const char *env_home = getenv("BAKE_HOME");
    if (env_home && env_home[0]) {
        ctx->bake_home = bake_env_resolve_home_path(env_home);
    } else {
        char *home = bake_os_home_path();
        if (!home) {
            return -1;
        }
        ctx->bake_home = bake_path_join(home, "bake3");
        ecs_os_free(home);
    }

    if (!ctx->bake_home) {
        return -1;
    }

    bake_os_setenv("BAKE_HOME", ctx->bake_home);

    if (bake_os_mkdirs(ctx->bake_home) != 0) {
        return -1;
    }

    if (bake_env_ensure_local_test_templates(ctx) != 0) {
        return -1;
    }

    return 0;
}
