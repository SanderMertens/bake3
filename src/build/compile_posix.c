#include "build_internal.h"
#include "compile_internal.h"
#include "bake/os.h"

static void bake_strbuf_append_quoted_path(
    ecs_strbuf_t *cmd, const char *prefix, const char *path)
{
    ecs_strbuf_append(cmd, "%s\"", prefix);
#if defined(_WIN32)
    for (const char *p = path; *p; p++) {
        ecs_strbuf_appendch(cmd, (*p == '\\') ? '/' : *p);
    }
#else
    ecs_strbuf_appendstr(cmd, path);
#endif
    ecs_strbuf_appendstr(cmd, "\"");
}

int bake_compose_compile_command_posix(const bake_compile_cmd_ctx_t *ctx, ecs_strbuf_t *cmd) {
    const char *compiler = ctx->unit->cpp
        ? (ctx->ctx->opts.cxx ? ctx->ctx->opts.cxx : "c++")
        : (ctx->ctx->opts.cc ? ctx->ctx->opts.cc : "cc");

    ecs_strbuf_append(cmd, "%s -c", compiler);
    for (int32_t i = 0; i < ctx->mode_flags->count; i++) {
        ecs_strbuf_append(cmd, " %s", ctx->mode_flags->items[i]);
    }

    if (!ctx->unit->cpp && ctx->lang->c_standard) {
        ecs_strbuf_append(cmd, " -std=%s", ctx->lang->c_standard);
    } else if (ctx->unit->cpp && ctx->lang->cpp_standard) {
        ecs_strbuf_append(cmd, " -std=%s", ctx->lang->cpp_standard);
    }

    bake_list_append_fmt(cmd, &ctx->lang->cflags, "", false);
    bake_list_append_fmt(cmd, &ctx->lang->defines, "-D", false);
    ecs_strbuf_append(cmd, " -DBAKE_PROJECT_ID=\\\"%s\\\"", ctx->cfg->id);
    if (ctx->cfg->kind == BAKE_PROJECT_PACKAGE) {
        char *macro = bake_project_id_as_macro(ctx->cfg->id);
        if (!macro) {
            return -1;
        }
        ecs_strbuf_append(cmd, " -D%s_EXPORTS", macro);
        ecs_os_free(macro);
    }

    char *include = bake_path_join(ctx->cfg->path, "include");
    if (include && bake_path_exists(include)) {
        bake_strbuf_append_quoted_path(cmd, " -I", include);
    }
    ecs_os_free(include);

    for (int32_t i = 0; i < ctx->lang->include_paths.count; i++) {
        bake_strbuf_append_quoted_path(cmd, " -I", ctx->lang->include_paths.items[i]);
    }
    for (int32_t i = 0; i < ctx->dep_includes->count; i++) {
        bake_strbuf_append_quoted_path(cmd, " -I", ctx->dep_includes->items[i]);
    }

    if (ctx->unit->dep) {
        bake_strbuf_append_quoted_path(cmd, " -MMD -MF ", ctx->unit->dep);
    }

    bake_strbuf_append_quoted_path(cmd, " -o ", ctx->unit->obj);
    bake_strbuf_append_quoted_path(cmd, " ", ctx->unit->src);
    return 0;
}

int bake_compose_link_command_posix(const bake_link_cmd_ctx_t *ctx, ecs_strbuf_t *cmd) {
    bool is_lib = ctx->cfg->kind == BAKE_PROJECT_PACKAGE;
    if (is_lib) {
        bake_strbuf_append_quoted_path(cmd, "ar rcs ", ctx->artefact);
        for (int32_t i = 0; i < ctx->units->count; i++) {
            bake_strbuf_append_quoted_path(cmd, " ", ctx->units->items[i].obj);
        }
        return 0;
    }

    const char *linker = ctx->use_cpp
        ? (ctx->ctx->opts.cxx ? ctx->ctx->opts.cxx : "c++")
        : (ctx->ctx->opts.cc ? ctx->ctx->opts.cc : "cc");

    ecs_strbuf_append(cmd, "%s", linker);
    for (int32_t i = 0; i < ctx->units->count; i++) {
        bake_strbuf_append_quoted_path(cmd, " ", ctx->units->items[i].obj);
    }
#if !defined(__APPLE__)
    if (ctx->dep_artefacts->count > 0) {
        ecs_strbuf_appendstr(cmd, " -Wl,--start-group");
    }
#endif
    for (int32_t i = 0; i < ctx->dep_artefacts->count; i++) {
        bake_strbuf_append_quoted_path(cmd, " ", ctx->dep_artefacts->items[i]);
    }
#if !defined(__APPLE__)
    if (ctx->dep_artefacts->count > 0) {
        ecs_strbuf_appendstr(cmd, " -Wl,--end-group");
    }
#endif
    bake_list_append_fmt(cmd, ctx->mode_ldflags, "", false);
    bake_list_append_fmt(cmd, &ctx->lang->ldflags, "", false);
    bake_list_append_fmt(cmd, ctx->dep_ldflags, "", false);
    for (int32_t i = 0; i < ctx->lang->libpaths.count; i++) {
        bake_strbuf_append_quoted_path(cmd, " -L", ctx->lang->libpaths.items[i]);
    }
    for (int32_t i = 0; i < ctx->dep_libpaths->count; i++) {
        bake_strbuf_append_quoted_path(cmd, " -L", ctx->dep_libpaths->items[i]);
    }
    for (int32_t i = 0; i < ctx->lang->libs.count; i++) {
        ecs_strbuf_append(cmd, " -l%s", ctx->lang->libs.items[i]);
    }
    for (int32_t i = 0; i < ctx->dep_libs->count; i++) {
        ecs_strbuf_append(cmd, " -l%s", ctx->dep_libs->items[i]);
    }
    bake_strbuf_append_quoted_path(cmd, " -o ", ctx->artefact);
    return 0;
}
