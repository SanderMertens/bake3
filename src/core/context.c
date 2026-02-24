#include "bake2/context.h"
#include "bake2/environment.h"
#include "bake2/model.h"

#if !defined(_WIN32)
#include <unistd.h>
#endif

static int32_t bake_default_threads(void) {
    const char *threads = getenv("BAKE_THREADS");
    if (threads && threads[0]) {
        int32_t value = atoi(threads);
        if (value > 0) {
            return value;
        }
    }

#if defined(_WIN32)
    return 4;
#else
    long cpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpu > 1 && cpu < 256) {
        return (int32_t)cpu;
    }
    return 4;
#endif
}

int bake_context_init(bake_context_t *ctx, const bake_options_t *opts) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->opts = *opts;
    if (opts->jobs > 0) {
        ctx->thread_count = opts->jobs;
    } else {
        ctx->thread_count = bake_default_threads();
    }

    ctx->world = ecs_init();
    if (!ctx->world) {
        return -1;
    }

    if (bake_model_init(ctx->world) != 0) {
        bake_context_fini(ctx);
        return -1;
    }

    if (bake_environment_init_paths(ctx) != 0) {
        bake_context_fini(ctx);
        return -1;
    }

    if (bake_environment_load(ctx) != 0) {
        bake_context_fini(ctx);
        return -1;
    }

    return 0;
}

void bake_context_fini(bake_context_t *ctx) {
    if (ctx->world) {
        bake_model_fini(ctx->world);
        ecs_fini(ctx->world);
        ctx->world = NULL;
    }

    ecs_os_free(ctx->bake_home);
    ecs_os_free(ctx->env_path);
    ctx->bake_home = NULL;
    ctx->env_path = NULL;
}
