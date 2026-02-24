#include "build_internal.h"
#include "bake2/os.h"

#include <ctype.h>

static bool bake_dep_char_is_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static bool bake_dep_token_outdated(const char *token, int64_t obj_mtime) {
    if (!token[0]) {
        return false;
    }

    int64_t mtime = bake_file_mtime(token);
    if (mtime < 0) {
        return true;
    }

    return mtime > obj_mtime;
}

static bool bake_depfile_outdated(const char *dep_path, int64_t obj_mtime) {
    size_t len = 0;
    char *content = bake_read_file(dep_path, &len);
    if (!content) {
        return true;
    }

    bool seen_colon = false;
    bool outdated = false;
    int token_cap = 256;
    int token_len = 0;
    char *token = ecs_os_malloc((size_t)token_cap);
    if (!token) {
        ecs_os_free(content);
        return true;
    }

    for (size_t i = 0; i < len; i++) {
        char ch = content[i];

        if (!seen_colon) {
            if (ch == ':') {
                seen_colon = true;
            }
            continue;
        }

        if (ch == '\\' && (i + 1) < len && content[i + 1] == '\n') {
            i++;
            continue;
        }

        if (ch == '\\' && (i + 1) < len) {
            if ((token_len + 2) >= token_cap) {
                token_cap *= 2;
                char *next = ecs_os_realloc_n(token, char, token_cap);
                if (!next) {
                    ecs_os_free(token);
                    ecs_os_free(content);
                    return true;
                }
                token = next;
            }
            token[token_len++] = content[++i];
            continue;
        }

        if (bake_dep_char_is_space(ch)) {
            token[token_len] = '\0';
            if (token_len && bake_dep_token_outdated(token, obj_mtime)) {
                outdated = true;
            }
            token_len = 0;
            if (outdated) {
                break;
            }
            continue;
        }

        if ((token_len + 2) >= token_cap) {
            token_cap *= 2;
            char *next = ecs_os_realloc_n(token, char, token_cap);
            if (!next) {
                ecs_os_free(token);
                ecs_os_free(content);
                return true;
            }
            token = next;
        }
        token[token_len++] = ch;
    }

    if (!outdated) {
        token[token_len] = '\0';
        if (token_len && bake_dep_token_outdated(token, obj_mtime)) {
            outdated = true;
        }
    }

    ecs_os_free(token);
    ecs_os_free(content);
    return outdated;
}

static bool bake_compile_unit_outdated(const bake_compile_unit_t *unit) {
    if (!unit || !unit->src || !unit->obj) {
        return true;
    }

    if (!bake_path_exists(unit->obj)) {
        return true;
    }

    int64_t obj_mtime = bake_file_mtime(unit->obj);
    int64_t src_mtime = bake_file_mtime(unit->src);
    if (obj_mtime < 0 || src_mtime < 0) {
        return true;
    }

    if (src_mtime > obj_mtime) {
        return true;
    }

    if (unit->dep) {
        int64_t dep_mtime = bake_file_mtime(unit->dep);
        if (dep_mtime < 0 || dep_mtime < src_mtime) {
            return true;
        }
        if (bake_depfile_outdated(unit->dep, obj_mtime)) {
            return true;
        }
    }

    return false;
}

int bake_list_append_fmt(ecs_strbuf_t *buf, const bake_strlist_t *list, const char *prefix, bool quote) {
    for (int32_t i = 0; i < list->count; i++) {
        if (quote) {
            ecs_strbuf_append(buf, " %s\"%s\"", prefix, list->items[i]);
        } else {
            ecs_strbuf_append(buf, " %s%s", prefix, list->items[i]);
        }
    }
    return 0;
}

char* bake_project_id_as_macro(const char *id) {
    if (!id) {
        return NULL;
    }

    size_t len = strlen(id);
    char *out = ecs_os_malloc(len + 1);
    if (!out) {
        return NULL;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)id[i];
        if (isalnum(ch)) {
            out[i] = (char)tolower(ch);
        } else {
            out[i] = '_';
        }
    }
    out[len] = '\0';
    return out;
}

static void bake_merge_strlist_unique_local(bake_strlist_t *dst, const bake_strlist_t *src) {
    for (int32_t i = 0; i < src->count; i++) {
        if (!bake_strlist_contains(dst, src->items[i])) {
            bake_strlist_append(dst, src->items[i]);
        }
    }
}

bake_compiler_kind_t bake_detect_compiler_kind(const char *cc, const char *cxx) {
    const char *probe = cxx && cxx[0] ? cxx : cc;
    if (!probe) {
#if defined(_WIN32)
        return B2_COMPILER_MSVC;
#else
        return B2_COMPILER_GCC;
#endif
    }

    if (strstr(probe, "cl") || strstr(probe, "cl.exe")) {
        return B2_COMPILER_MSVC;
    }
    if (strstr(probe, "clang")) {
        return B2_COMPILER_CLANG;
    }
    if (strstr(probe, "gcc") || strstr(probe, "g++") || strstr(probe, "cc") || strstr(probe, "c++")) {
        return B2_COMPILER_GCC;
    }

    return B2_COMPILER_UNKNOWN;
}

void bake_add_mode_flags(const char *mode, bake_compiler_kind_t kind, bake_strlist_t *cflags, bake_strlist_t *cxxflags, bake_strlist_t *ldflags) {
    if (!mode || !strcmp(mode, "debug")) {
        if (kind == B2_COMPILER_MSVC) {
            bake_strlist_append(cflags, "/Zi");
            bake_strlist_append(cflags, "/Od");
            bake_strlist_append(cxxflags, "/Zi");
            bake_strlist_append(cxxflags, "/Od");
        } else {
            bake_strlist_append(cflags, "-g");
            bake_strlist_append(cflags, "-O0");
            bake_strlist_append(cxxflags, "-g");
            bake_strlist_append(cxxflags, "-O0");
        }
        return;
    }

    if (!strcmp(mode, "release")) {
        if (kind == B2_COMPILER_MSVC) {
            bake_strlist_append(cflags, "/O2");
            bake_strlist_append(cxxflags, "/O2");
        } else {
            bake_strlist_append(cflags, "-O3");
            bake_strlist_append(cxxflags, "-O3");
            bake_strlist_append(cflags, "-DNDEBUG");
            bake_strlist_append(cxxflags, "-DNDEBUG");
        }
        return;
    }

    if (!strcmp(mode, "profile")) {
        if (kind == B2_COMPILER_MSVC) {
            bake_strlist_append(cflags, "/O2");
            bake_strlist_append(cxxflags, "/O2");
        } else {
            bake_strlist_append(cflags, "-O2");
            bake_strlist_append(cxxflags, "-O2");
            bake_strlist_append(cflags, "-pg");
            bake_strlist_append(cxxflags, "-pg");
            bake_strlist_append(ldflags, "-pg");
        }
        return;
    }

    if (!strcmp(mode, "sanitize")) {
        if (kind == B2_COMPILER_MSVC) {
            bake_strlist_append(cflags, "/fsanitize=address");
            bake_strlist_append(cxxflags, "/fsanitize=address");
        } else {
            bake_strlist_append(cflags, "-O1");
            bake_strlist_append(cxxflags, "-O1");
            bake_strlist_append(cflags, "-g");
            bake_strlist_append(cxxflags, "-g");
            bake_strlist_append(cflags, "-fsanitize=address");
            bake_strlist_append(cxxflags, "-fsanitize=address");
            bake_strlist_append(cflags, "-fsanitize=undefined");
            bake_strlist_append(cxxflags, "-fsanitize=undefined");
            bake_strlist_append(ldflags, "-fsanitize=address");
            bake_strlist_append(ldflags, "-fsanitize=undefined");
        }
    }
}

void bake_add_strict_flags(
    bool strict,
    bake_compiler_kind_t kind,
    bake_strlist_t *cflags,
    bake_strlist_t *cxxflags,
    bake_strlist_t *ldflags)
{
    if (!strict) {
        return;
    }

    if (kind == B2_COMPILER_MSVC) {
        bake_strlist_append(cflags, "/W3");
        bake_strlist_append(cflags, "/WX");
        bake_strlist_append(cxxflags, "/W3");
        bake_strlist_append(cxxflags, "/WX");
        bake_strlist_append(ldflags, "/WX");
        return;
    }

    bake_strlist_append(cflags, "-Werror");
    bake_strlist_append(cflags, "-Wshadow");
    bake_strlist_append(cflags, "-Wconversion");
    bake_strlist_append(cflags, "-Wsign-conversion");
    bake_strlist_append(cflags, "-Wfloat-conversion");
    bake_strlist_append(cflags, "-Wundef");
    bake_strlist_append(cflags, "-Wpedantic");

    bake_strlist_append(cxxflags, "-Werror");
    bake_strlist_append(cxxflags, "-Wshadow");
    bake_strlist_append(cxxflags, "-Wconversion");
    bake_strlist_append(cxxflags, "-Wsign-conversion");
    bake_strlist_append(cxxflags, "-Wfloat-conversion");
    bake_strlist_append(cxxflags, "-Wundef");
    bake_strlist_append(cxxflags, "-Wpedantic");

    bake_strlist_append(ldflags, "-Werror");
    bake_strlist_append(ldflags, "-Wpedantic");
}

static void bake_collect_dependency_include_dirs(ecs_world_t *world, ecs_entity_t project_entity, bake_strlist_t *paths) {
    for (int32_t i = 0;; i++) {
        ecs_entity_t dep = ecs_get_target(world, project_entity, B2DependsOn, i);
        if (!dep) {
            break;
        }

        const BakeProject *dep_project = ecs_get(world, dep, BakeProject);
        if (!dep_project || !dep_project->cfg) {
            continue;
        }

        if (dep_project->cfg->path) {
            char *include = bake_join_path(dep_project->cfg->path, "include");
            if (include) {
                if (bake_path_exists(include) && !bake_strlist_contains(paths, include)) {
                    bake_strlist_append(paths, include);
                }
                ecs_os_free(include);
            }
        }

        for (int32_t u = 0; u < dep_project->cfg->dependee.use.count; u++) {
            const BakeProject *dependee_use = bake_model_find_project(
                world,
                dep_project->cfg->dependee.use.items[u],
                NULL);
            if (!dependee_use || !dependee_use->cfg || !dependee_use->cfg->path) {
                continue;
            }

            char *include = bake_join_path(dependee_use->cfg->path, "include");
            if (include) {
                if (bake_path_exists(include) && !bake_strlist_contains(paths, include)) {
                    bake_strlist_append(paths, include);
                }
                ecs_os_free(include);
            }
        }
    }
}

static void bake_collect_dependency_link_inputs(
    ecs_world_t *world,
    ecs_entity_t project_entity,
    const char *mode,
    bake_strlist_t *artefacts,
    bake_strlist_t *libpaths,
    bake_strlist_t *libs)
{
    for (int32_t i = 0;; i++) {
        ecs_entity_t dep = ecs_get_target(world, project_entity, B2DependsOn, i);
        if (!dep) {
            break;
        }

        const BakeBuildResult *result = ecs_get(world, dep, BakeBuildResult);
        if (result && result->artefact && !bake_strlist_contains(artefacts, result->artefact)) {
            bake_strlist_append(artefacts, result->artefact);
        }

        const BakeProject *dep_project = ecs_get(world, dep, BakeProject);
        if (dep_project && dep_project->cfg && dep_project->cfg->path) {
            char *lib = bake_project_build_root(dep_project->cfg->path, mode);
            if (lib && bake_path_exists(lib) && !bake_strlist_contains(libpaths, lib)) {
                bake_strlist_append(libpaths, lib);
            }
            ecs_os_free(lib);
        }

        if (dep_project && dep_project->cfg) {
            bake_merge_strlist_unique_local(libs, &dep_project->cfg->dependee.libs);

            for (int32_t u = 0; u < dep_project->cfg->dependee.use.count; u++) {
                ecs_entity_t dependee_use_e = 0;
                const BakeProject *use_project = bake_model_find_project(
                    world,
                    dep_project->cfg->dependee.use.items[u],
                    &dependee_use_e);
                if (use_project && use_project->cfg) {
                    const BakeBuildResult *use_result = ecs_get(
                        world, dependee_use_e, BakeBuildResult);
                    if (use_result && use_result->artefact &&
                        !bake_strlist_contains(artefacts, use_result->artefact))
                    {
                        bake_strlist_append(artefacts, use_result->artefact);
                    }

                    if (use_project->cfg->path) {
                        char *lib = bake_project_build_root(
                            use_project->cfg->path, mode);
                        if (lib && bake_path_exists(lib) &&
                            !bake_strlist_contains(libpaths, lib))
                        {
                            bake_strlist_append(libpaths, lib);
                        }
                        ecs_os_free(lib);
                    }

                    bake_merge_strlist_unique_local(
                        libs, &use_project->cfg->dependee.libs);
                }
            }
        }
    }
}

typedef struct bake_compile_ctx_t {
    bake_context_t *ctx;
    const bake_project_cfg_t *cfg;
    const bake_build_paths_t *paths;
    const bake_compile_list_t *units;
    const bake_lang_cfg_t *c_lang;
    const bake_lang_cfg_t *cpp_lang;
    const bake_strlist_t *mode_cflags;
    const bake_strlist_t *mode_cxxflags;
    bake_strlist_t dep_includes;
    bool *compile_mask;
    int32_t compile_total;
    int32_t compile_done;
    ecs_os_mutex_t print_lock;
    int32_t cursor;
    int32_t failed;
} bake_compile_ctx_t;

static bool bake_path_is_sep(char ch) {
    return ch == '/' || ch == '\\';
}

static bool bake_path_has_prefix(const char *path, const char *prefix, size_t *prefix_len_out) {
    if (!path || !prefix) {
        return false;
    }

    size_t prefix_len = strlen(prefix);
    while (prefix_len > 0 && bake_path_is_sep(prefix[prefix_len - 1])) {
        prefix_len--;
    }

    if (strncmp(path, prefix, prefix_len)) {
        return false;
    }

    if (path[prefix_len] && !bake_path_is_sep(path[prefix_len])) {
        return false;
    }

    if (prefix_len_out) {
        *prefix_len_out = prefix_len;
    }
    return true;
}

static char* bake_compile_display_path(const bake_project_cfg_t *cfg, const char *src) {
    if (!src) {
        return NULL;
    }

    const char *display = src;
    size_t prefix_len = 0;
    if (cfg && cfg->path && bake_path_has_prefix(src, cfg->path, &prefix_len)) {
        display = src + prefix_len;
        while (bake_path_is_sep(*display)) {
            display++;
        }

        if (!strncmp(display, "src/", 4) || !strncmp(display, "src\\", 4)) {
            display += 4;
        }
    }

    if (!display[0]) {
        return bake_basename(src);
    }

    char *out = bake_strdup(display);
    if (!out) {
        return bake_basename(src);
    }

    for (char *ch = out; *ch; ch++) {
        if (*ch == '\\') {
            *ch = '/';
        }
    }

    return out;
}

static int bake_compile_single(bake_compile_ctx_t *ctx, const bake_compile_unit_t *unit) {
    if (ctx->print_lock) {
        ecs_os_mutex_lock(ctx->print_lock);
    }
    int32_t done = ecs_os_ainc(&ctx->compile_done);
    int32_t pct = (done * 100) / ctx->compile_total;
    char *display_path = bake_compile_display_path(ctx->cfg, unit->src);
    B2_LOG("[%6d%%] %s", pct, display_path ? display_path : unit->src);
    if (ctx->print_lock) {
        ecs_os_mutex_unlock(ctx->print_lock);
    }
    ecs_os_free(display_path);

    const bake_lang_cfg_t *lang = unit->cpp ? ctx->cpp_lang : ctx->c_lang;
    const bake_strlist_t *mode_flags = unit->cpp ? ctx->mode_cxxflags : ctx->mode_cflags;

    char *obj_dir = bake_dirname(unit->obj);
    if (!obj_dir || bake_mkdirs(obj_dir) != 0) {
        ecs_os_free(obj_dir);
        return -1;
    }
    ecs_os_free(obj_dir);

    ecs_strbuf_t cmd = ECS_STRBUF_INIT;
    bake_compile_cmd_ctx_t cmd_ctx = {
        .ctx = ctx->ctx,
        .cfg = ctx->cfg,
        .unit = unit,
        .lang = lang,
        .mode_flags = mode_flags,
        .dep_includes = &ctx->dep_includes
    };

    int rc = 0;
    if (ctx->ctx->compiler_kind == B2_COMPILER_MSVC) {
        rc = bake_compose_compile_command_msvc(&cmd_ctx, &cmd);
    } else {
        rc = bake_compose_compile_command_posix(&cmd_ctx, &cmd);
    }
    if (rc != 0) {
        return -1;
    }

    char *command = ecs_strbuf_get(&cmd);
    rc = bake_run_command_quiet(command);
    ecs_os_free(command);
    return rc;
}

static void* bake_compile_worker(void *arg) {
    bake_compile_ctx_t *ctx = arg;

    for (;;) {
        if (ctx->failed || bake_proc_was_interrupted()) {
            break;
        }

        int32_t index = ecs_os_ainc(&ctx->cursor) - 1;
        if (index >= ctx->units->count) {
            break;
        }

        if (!ctx->compile_mask[index]) {
            continue;
        }

        if (bake_compile_single(ctx, &ctx->units->items[index]) != 0) {
            ecs_os_ainc(&ctx->failed);
            break;
        }
    }

    return NULL;
}

int bake_compile_units_parallel(
    bake_context_t *ctx,
    const bake_project_cfg_t *cfg,
    const bake_build_paths_t *paths,
    const bake_compile_list_t *units,
    const bake_lang_cfg_t *lang,
    const bake_lang_cfg_t *cpp_lang,
    const bake_strlist_t *mode_cflags,
    const bake_strlist_t *mode_cxxflags)
{
    bake_proc_clear_interrupt();

    if (units->count == 0) {
        return 0;
    }

    ecs_entity_t project_entity = 0;
    bake_model_find_project(ctx->world, cfg->id, &project_entity);

    bake_compile_ctx_t compile_ctx = {
        .ctx = ctx,
        .cfg = cfg,
        .paths = paths,
        .units = units,
        .c_lang = lang,
        .cpp_lang = cpp_lang,
        .mode_cflags = mode_cflags,
        .mode_cxxflags = mode_cxxflags,
        .compile_mask = NULL,
        .compile_total = 0,
        .compile_done = 0,
        .print_lock = 0,
        .cursor = 0,
        .failed = 0
    };

    compile_ctx.compile_mask = ecs_os_calloc_n(bool, units->count);
    if (!compile_ctx.compile_mask) {
        return -1;
    }

    for (int32_t i = 0; i < units->count; i++) {
        compile_ctx.compile_mask[i] = bake_compile_unit_outdated(&units->items[i]);
        if (compile_ctx.compile_mask[i]) {
            compile_ctx.compile_total++;
        }
    }

    if (compile_ctx.compile_total == 0) {
        ecs_os_free(compile_ctx.compile_mask);
        return 0;
    }

    bake_strlist_init(&compile_ctx.dep_includes);
    bake_collect_dependency_include_dirs(ctx->world, project_entity, &compile_ctx.dep_includes);
    compile_ctx.print_lock = ecs_os_mutex_new();

    int32_t workers = ctx->thread_count;
    if (workers < 1) {
        workers = 1;
    }
    if (workers > compile_ctx.compile_total) {
        workers = compile_ctx.compile_total;
    }

    ecs_os_thread_t *threads = ecs_os_malloc_n(ecs_os_thread_t, workers);
    if (!threads) {
        bake_strlist_fini(&compile_ctx.dep_includes);
        if (compile_ctx.print_lock) {
            ecs_os_mutex_free(compile_ctx.print_lock);
        }
        ecs_os_free(compile_ctx.compile_mask);
        return -1;
    }

    for (int32_t i = 0; i < workers; i++) {
        threads[i] = ecs_os_thread_new(bake_compile_worker, &compile_ctx);
    }

    for (int32_t i = 0; i < workers; i++) {
        ecs_os_thread_join(threads[i]);
    }

    ecs_os_free(threads);
    bake_strlist_fini(&compile_ctx.dep_includes);
    if (compile_ctx.print_lock) {
        ecs_os_mutex_free(compile_ctx.print_lock);
    }
    ecs_os_free(compile_ctx.compile_mask);

    return compile_ctx.failed ? -1 : 0;
}

int bake_link_project_binary(
    bake_context_t *ctx,
    ecs_entity_t project_entity,
    const bake_project_cfg_t *cfg,
    const bake_build_paths_t *paths,
    const bake_compile_list_t *units,
    const bake_lang_cfg_t *lang,
    const bake_strlist_t *mode_ldflags,
    char **artefact_out)
{
    if (cfg->kind == B2_PROJECT_CONFIG || cfg->kind == B2_PROJECT_TEMPLATE) {
        *artefact_out = NULL;
        return 0;
    }

    bake_strlist_t dep_artefacts;
    bake_strlist_t dep_libpaths;
    bake_strlist_t dep_libs;
    bake_strlist_init(&dep_artefacts);
    bake_strlist_init(&dep_libpaths);
    bake_strlist_init(&dep_libs);

    bake_collect_dependency_link_inputs(
        ctx->world,
        project_entity,
        ctx->opts.mode,
        &dep_artefacts,
        &dep_libpaths,
        &dep_libs);

    bool is_lib = cfg->kind == B2_PROJECT_PACKAGE;
#if defined(_WIN32)
    const char *exe_ext = ".exe";
    const char *lib_ext = ".lib";
    const char *lib_prefix = "";
#else
    const char *exe_ext = "";
    const char *lib_ext = ".a";
    const char *lib_prefix = "lib";
#endif

    char *file_name = NULL;
    if (is_lib) {
        file_name = bake_asprintf("%s%s%s", lib_prefix, cfg->output_name, lib_ext);
    } else {
        file_name = bake_asprintf("%s%s", cfg->output_name, exe_ext);
    }

    if (!file_name) {
        bake_strlist_fini(&dep_artefacts);
        bake_strlist_fini(&dep_libpaths);
        bake_strlist_fini(&dep_libs);
        return -1;
    }

    char *artefact = bake_join_path(is_lib ? paths->lib_dir : paths->bin_dir, file_name);
    ecs_os_free(file_name);
    if (!artefact) {
        bake_strlist_fini(&dep_artefacts);
        bake_strlist_fini(&dep_libpaths);
        bake_strlist_fini(&dep_libs);
        return -1;
    }

    bool use_cpp = false;
    for (int32_t i = 0; i < units->count; i++) {
        if (units->items[i].cpp) {
            use_cpp = true;
            break;
        }
    }

    ecs_strbuf_t cmd = ECS_STRBUF_INIT;
    bake_link_cmd_ctx_t cmd_ctx = {
        .ctx = ctx,
        .cfg = cfg,
        .paths = paths,
        .units = units,
        .lang = lang,
        .mode_ldflags = mode_ldflags,
        .dep_artefacts = &dep_artefacts,
        .dep_libpaths = &dep_libpaths,
        .dep_libs = &dep_libs,
        .artefact = artefact,
        .use_cpp = use_cpp
    };

    int compose_rc = 0;
    if (ctx->compiler_kind == B2_COMPILER_MSVC) {
        compose_rc = bake_compose_link_command_msvc(&cmd_ctx, &cmd);
    } else {
        compose_rc = bake_compose_link_command_posix(&cmd_ctx, &cmd);
    }
    if (compose_rc != 0) {
        bake_strlist_fini(&dep_artefacts);
        bake_strlist_fini(&dep_libpaths);
        bake_strlist_fini(&dep_libs);
        ecs_os_free(artefact);
        return -1;
    }

    char *command = ecs_strbuf_get(&cmd);
    int rc = bake_run_command_quiet(command);
    ecs_os_free(command);

    bake_strlist_fini(&dep_artefacts);
    bake_strlist_fini(&dep_libpaths);
    bake_strlist_fini(&dep_libs);

    if (rc != 0) {
        ecs_os_free(artefact);
        return -1;
    }

    *artefact_out = artefact;
    return 0;
}
