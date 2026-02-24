#include "bake2/context.h"
#include "bake2/environment.h"
#include "bake2/model.h"

#if !defined(_WIN32)
#include <unistd.h>
#endif

static int32_t b2_default_threads(void) {
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

int b2_context_init(b2_context_t *ctx, const b2_options_t *opts) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->opts = *opts;
    ctx->thread_count = b2_default_threads();

    ctx->world = ecs_init();
    if (!ctx->world) {
        return -1;
    }

    if (b2_model_init(ctx->world) != 0) {
        b2_context_fini(ctx);
        return -1;
    }

    if (b2_environment_init_paths(ctx) != 0) {
        b2_context_fini(ctx);
        return -1;
    }

    if (b2_environment_load(ctx) != 0) {
        b2_context_fini(ctx);
        return -1;
    }

    return 0;
}

void b2_context_fini(b2_context_t *ctx) {
    if (ctx->world) {
        b2_model_fini(ctx->world);
        ecs_fini(ctx->world);
        ctx->world = NULL;
    }

    ecs_os_free(ctx->bake_home);
    ecs_os_free(ctx->env_path);
    ctx->bake_home = NULL;
    ctx->env_path = NULL;
}
