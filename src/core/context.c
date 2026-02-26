#include "bake/context.h"
#include "bake/environment.h"
#include "bake/model.h"
#include "bake/os.h"

static
void bake_print_header(
    FILE *stream,
    int level,
    const char *label,
    bool use_colors)
{
    const char *color = level == -2 ? ECS_YELLOW : ECS_RED;
    if (use_colors) fputs(color, stream);
    fputs("[", stream);
    if (use_colors) fputs(ECS_NORMAL, stream);
    fputs(label, stream);
    if (use_colors) fputs(color, stream);
    fputs("] ", stream);
    if (use_colors) fputs(ECS_NORMAL, stream);
}

static
void bake_log(
    int32_t level,
    const char *file, 
    int32_t line,  
    const char *msg)
{
    FILE *stream = ecs_os_api.log_out_;
    if (!stream) {
        stream = stdout;
    }

    bool use_colors = ecs_os_api.flags_ & EcsOsApiLogWithColors;

    if (level == -2) {
        bake_print_header(stream, level, "warning", use_colors);
    } else if (level == -3) {
        bake_print_header(stream, level, "  error", use_colors);
    } else if (level == -4) {
        bake_print_header(stream, level, "  fatal", use_colors);
    }

    if (level >= 0) {
        if (ecs_os_api.log_indent_) {
            char indent[32];
            int i, indent_count = ecs_os_api.log_indent_;
            if (indent_count > 15) indent_count = 15;

            for (i = 0; i < indent_count; i ++) {
                indent[i * 2] = '|';
                indent[i * 2 + 1] = ' ';
            }

            if (ecs_os_api.log_indent_ != indent_count) {
                indent[i * 2 - 2] = '+';
            }

            indent[i * 2] = '\0';

            fputs(indent, stream);
        }
    }

    if (level == -4) {
        if (file) {
            const char *file_ptr = strrchr(file, '/');
            if (!file_ptr) {
                file_ptr = strrchr(file, '\\');
            }

            if (file_ptr) {
                file = file_ptr + 1;
            }

            fputs(file, stream);
            fputs(": ", stream);
        }

        if (line) {
            fprintf(stream, "%d: ", line);
        }
    }

    fputs(msg, stream);

    fputs("\n", stream);

    if (level == -4) {
        flecs_dump_backtrace(stream);
    }
}

int bake_context_init(bake_context_t *ctx, const bake_options_t *opts) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->opts = *opts;
    bake_project_cfg_set_eval_context(opts->mode, NULL);
    if (opts->jobs > 0) {
        ctx->thread_count = opts->jobs;
    } else {
        const char *threads = getenv("BAKE_THREADS");
        if (threads && threads[0]) {
            int32_t value = atoi(threads);
            if (value > 0) {
                ctx->thread_count = value;
            }
        }

        if (ctx->thread_count == 0) {
            ctx->thread_count = bake_os_default_threads();
        }
    }

    ecs_os_set_api_defaults();
    ecs_os_api_t os_api = ecs_os_get_api();
    os_api.log_ = bake_log;
    ecs_os_set_api(&os_api);

    ctx->world = ecs_init();
    if (!ctx->world) {
        return -1;
    }

    if (bake_model_init(ctx->world) != 0) {
        bake_context_fini(ctx);
        return -1;
    }

    ecs_log_set_level(0);

    if (bake_environment_init_paths(ctx) != 0) {
        bake_context_fini(ctx);
        return -1;
    }

    return 0;
}

void bake_context_fini(bake_context_t *ctx) {
    if (ctx->world) {
        ecs_log_set_level(-1);
        ecs_fini(ctx->world);
        ctx->world = NULL;
    }

    ecs_os_free(ctx->bake_home);
    ctx->bake_home = NULL;
}
