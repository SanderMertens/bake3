#include "bake/bundle.h"
#include "bake/environment.h"
#include "bake/model.h"
#include "bake/os.h"

#include <flecs.h>

static char* bake_bundle_ref_segment(const bake_bundle_t *bundle) {
    if (bundle->commit && bundle->commit[0]) {
        return flecs_asprintf("commits/%s", bundle->commit);
    }
    if (bundle->tag && bundle->tag[0]) {
        return flecs_asprintf("tags/%s", bundle->tag);
    }
    if (bundle->branch && bundle->branch[0]) {
        return flecs_asprintf("branches/%s", bundle->branch);
    }
    return ecs_os_strdup("default");
}

static char* bake_bundle_root_dir(const bake_project_cfg_t *cfg, const bake_bundle_t *bundle) {
    if (!cfg || !cfg->path || !bundle || !bundle->id) {
        return NULL;
    }
    char *bake = bake_path_join(cfg->path, ".bake");
    char *bundles = bake ? bake_path_join(bake, "bundles") : NULL;
    ecs_os_free(bake);
    char *id_dir = bundles ? bake_path_join(bundles, bundle->id) : NULL;
    ecs_os_free(bundles);
    if (!id_dir) {
        return NULL;
    }
    char *ref = bake_bundle_ref_segment(bundle);
    char *root = ref ? bake_path_join(id_dir, ref) : NULL;
    ecs_os_free(id_dir);
    ecs_os_free(ref);
    return root;
}

static char* bake_bundle_source_dir(const char *root) {
    return root ? bake_path_join(root, "src") : NULL;
}

static char* bake_bundle_build_dir(const char *root, const char *triplet) {
    if (!root || !triplet) return NULL;
    char *base = bake_path_join(root, "build");
    char *path = base ? bake_path_join(base, triplet) : NULL;
    ecs_os_free(base);
    return path;
}

static char* bake_bundle_install_dir(const char *root, const char *triplet) {
    if (!root || !triplet) return NULL;
    char *base = bake_path_join(root, "install");
    char *path = base ? bake_path_join(base, triplet) : NULL;
    ecs_os_free(base);
    return path;
}

static char* bake_bundle_install_marker(const char *install_dir) {
    return bake_path_join(install_dir, ".bake_bundle_built");
}

static const char* bake_bundle_cmake_build_type(const char *mode) {
    if (!mode || !mode[0]) {
        return "Debug";
    }
    if (!strcmp(mode, "release")) return "Release";
    if (!strcmp(mode, "profile")) return "RelWithDebInfo";
    if (!strcmp(mode, "sanitize")) return "Debug";
    return "Debug";
}

static int bake_bundle_clone(const bake_bundle_t *bundle, const char *dest) {
    if (!bundle->repository || !bundle->repository[0]) {
        ecs_err("bundle '%s' has no repository", bundle->id);
        return -1;
    }

    const char *ref = NULL;
    if (bundle->branch && bundle->branch[0]) ref = bundle->branch;
    else if (bundle->tag && bundle->tag[0]) ref = bundle->tag;

    char *quoted_dest = bake_shell_quote_arg(dest);
    char *quoted_repo = bake_shell_quote_arg(bundle->repository);
    char *quoted_ref = ref ? bake_shell_quote_arg(ref) : NULL;
    if (!quoted_dest || !quoted_repo || (ref && !quoted_ref)) {
        ecs_os_free(quoted_dest);
        ecs_os_free(quoted_repo);
        ecs_os_free(quoted_ref);
        return -1;
    }

    ecs_strbuf_t cmd = ECS_STRBUF_INIT;
    ecs_strbuf_appendstr(&cmd, "git clone --recurse-submodules");
    if (ref) {
        ecs_strbuf_append(&cmd, " --branch %s --depth 1 --shallow-submodules", quoted_ref);
    } else if (!bundle->commit || !bundle->commit[0]) {
        ecs_strbuf_appendstr(&cmd, " --depth 1 --shallow-submodules");
    }
    ecs_strbuf_append(&cmd, " %s %s", quoted_repo, quoted_dest);
    ecs_os_free(quoted_repo);
    ecs_os_free(quoted_ref);

    char *cmd_str = ecs_strbuf_get(&cmd);
    if (!cmd_str) {
        ecs_os_free(quoted_dest);
        return -1;
    }
    int rc = bake_run_command(cmd_str, true);
    ecs_os_free(cmd_str);
    if (rc != 0) {
        ecs_os_free(quoted_dest);
        return -1;
    }

    if (bundle->commit && bundle->commit[0]) {
        ecs_strbuf_t co = ECS_STRBUF_INIT;
        char *quoted_path = quoted_dest;
        quoted_dest = NULL;
        ecs_strbuf_append(&co, "git -C %s fetch --depth 1 origin %s", quoted_path, bundle->commit);
        char *fetch_cmd = ecs_strbuf_get(&co);
        rc = fetch_cmd ? bake_run_command(fetch_cmd, true) : -1;
        ecs_os_free(fetch_cmd);
        if (rc != 0) {
            ecs_os_free(quoted_path);
            return -1;
        }

        ecs_strbuf_t cmd2 = ECS_STRBUF_INIT;
        ecs_strbuf_append(&cmd2, "git -C %s checkout %s", quoted_path, bundle->commit);
        char *checkout_cmd = ecs_strbuf_get(&cmd2);
        rc = checkout_cmd ? bake_run_command(checkout_cmd, true) : -1;
        ecs_os_free(checkout_cmd);
        ecs_os_free(quoted_path);
        if (rc != 0) {
            return -1;
        }
    }

    ecs_os_free(quoted_dest);
    return 0;
}

static int bake_bundle_run_cmake(
    const bake_bundle_t *bundle,
    const char *src_dir,
    const char *build_dir,
    const char *install_dir,
    const char *mode)
{
    char *quoted_src = bake_shell_quote_arg(src_dir);
    char *quoted_build = bake_shell_quote_arg(build_dir);
    char *quoted_install = bake_shell_quote_arg(install_dir);
    int rc = -1;

    if (!quoted_src || !quoted_build || !quoted_install) {
        goto cleanup;
    }

    const char *build_type = bake_bundle_cmake_build_type(mode);
    const char *cmake_launcher = bake_target_is_emscripten() ? "emcmake cmake" : "cmake";

    ecs_strbuf_t configure = ECS_STRBUF_INIT;
    ecs_strbuf_append(&configure,
        "%s -S %s -B %s -DCMAKE_INSTALL_PREFIX=%s -DCMAKE_BUILD_TYPE=%s -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON",
        cmake_launcher, quoted_src, quoted_build, quoted_install, build_type);
    for (int32_t i = 0; i < bundle->cmake_args.count; i++) {
        char *q = bake_shell_quote_arg(bundle->cmake_args.items[i]);
        if (!q) {
            ecs_strbuf_reset(&configure);
            goto cleanup;
        }
        ecs_strbuf_append(&configure, " %s", q);
        ecs_os_free(q);
    }
    char *cfg_cmd = ecs_strbuf_get(&configure);
    rc = cfg_cmd ? bake_run_command(cfg_cmd, true) : -1;
    ecs_os_free(cfg_cmd);
    if (rc != 0) goto cleanup;

    ecs_strbuf_t build = ECS_STRBUF_INIT;
    ecs_strbuf_append(&build, "cmake --build %s --config %s", quoted_build, build_type);
    char *build_cmd = ecs_strbuf_get(&build);
    rc = build_cmd ? bake_run_command(build_cmd, true) : -1;
    ecs_os_free(build_cmd);
    if (rc != 0) goto cleanup;

    ecs_strbuf_t install = ECS_STRBUF_INIT;
    ecs_strbuf_append(&install, "cmake --install %s --config %s", quoted_build, build_type);
    char *install_cmd = ecs_strbuf_get(&install);
    rc = install_cmd ? bake_run_command(install_cmd, true) : -1;
    ecs_os_free(install_cmd);

cleanup:
    ecs_os_free(quoted_src);
    ecs_os_free(quoted_build);
    ecs_os_free(quoted_install);
    return rc;
}

static int bake_bundle_resolve_lib(
    const bake_bundle_t *bundle,
    const char *base_dir,
    const char *const *libdirs,
    char **libpath_out)
{
    const char *libname = (bundle->library && bundle->library[0]) ? bundle->library : bundle->id;

#if defined(_WIN32)
    /* MSVC emits <name>.lib; MinGW (used on the Windows CI runner) emits
     * lib<name>.a or <name>.dll.a. */
    const char *prefixes[] = { "", "lib", NULL };
    const char *exts[] = { ".lib", ".a", ".dll.a", NULL };
#elif defined(__APPLE__)
    const char *prefixes[] = { "lib", NULL };
    const char *exts[] = { ".a", ".dylib", NULL };
#else
    const char *prefixes[] = { "lib", NULL };
    const char *exts[] = { ".a", ".so", NULL };
#endif

    for (int32_t d = 0; libdirs[d]; d++) {
        char *libdir = libdirs[d][0]
            ? bake_path_join(base_dir, libdirs[d])
            : ecs_os_strdup(base_dir);
        if (!libdir) continue;
        if (!bake_path_exists(libdir)) {
            ecs_os_free(libdir);
            continue;
        }
        for (int32_t pi = 0; prefixes[pi]; pi++) {
            for (int32_t ei = 0; exts[ei]; ei++) {
                char *file = flecs_asprintf("%s%s%s", prefixes[pi], libname, exts[ei]);
                char *full = file ? bake_path_join(libdir, file) : NULL;
                ecs_os_free(file);
                if (full && bake_path_exists(full)) {
                    *libpath_out = libdir;
                    ecs_os_free(full);
                    return 0;
                }
                ecs_os_free(full);
            }
        }
        ecs_os_free(libdir);
    }

    return -1;
}

static bool bake_bundle_uses_cargo(const bake_bundle_t *bundle) {
    return bundle->build_system && !strcmp(bundle->build_system, "cargo");
}

static int bake_bundle_run_cargo(
    const bake_bundle_t *bundle,
    const char *src_dir,
    const char *build_dir)
{
    char *quoted_src = bake_shell_quote_arg(src_dir);
    char *quoted_build = bake_shell_quote_arg(build_dir);
    int rc = -1;

    if (!quoted_src || !quoted_build) {
        goto cleanup;
    }

    ecs_strbuf_t cmd = ECS_STRBUF_INIT;
    ecs_strbuf_append(&cmd,
        "cargo build --release --manifest-path %s/Cargo.toml --target-dir %s",
        quoted_src, quoted_build);

    char *cmd_str = ecs_strbuf_get(&cmd);
    rc = cmd_str ? bake_run_command(cmd_str, true) : -1;
    ecs_os_free(cmd_str);
    BAKE_UNUSED(bundle);

cleanup:
    ecs_os_free(quoted_src);
    ecs_os_free(quoted_build);
    return rc;
}

static int bake_bundle_add_include(bake_project_cfg_t *cfg, const char *path) {
    if (!path || !path[0] || !bake_path_exists(path)) {
        return 0;
    }
    return bake_strlist_append_unique(&cfg->bundle_includes, path);
}

static int bake_bundle_apply_to_project(
    bake_project_cfg_t *cfg,
    const bake_bundle_t *bundle,
    const char *src_root,
    const char *install_dir)
{
    /* Default include dir: install/include for cmake builds, src for header-only */
    if (bundle->header_only) {
        if (bake_bundle_add_include(cfg, src_root) != 0) return -1;
    } else {
        char *include_dir = bake_path_join(install_dir, "include");
        int rc = include_dir ? bake_bundle_add_include(cfg, include_dir) : -1;
        ecs_os_free(include_dir);
        if (rc != 0) return -1;
    }

    /* User-specified extra include subdirs (relative to src_root) */
    for (int32_t i = 0; i < bundle->includes.count; i++) {
        const char *rel = bundle->includes.items[i];
        char *full = (rel[0] == '/' || rel[0] == '\\')
            ? ecs_os_strdup(rel)
            : bake_path_join(src_root, rel);
        int rc = full ? bake_bundle_add_include(cfg, full) : -1;
        ecs_os_free(full);
        if (rc != 0) return -1;
    }

    /* Extra source files compiled into the consuming project */
    for (int32_t i = 0; i < bundle->sources.count; i++) {
        const char *rel = bundle->sources.items[i];
        char *full = (rel[0] == '/' || rel[0] == '\\')
            ? ecs_os_strdup(rel)
            : bake_path_join(src_root, rel);
        if (!full || !bake_path_exists(full)) {
            ecs_err("bundle '%s': source file not found: %s", bundle->id, full ? full : rel);
            ecs_os_free(full);
            return -1;
        }
        int rc = bake_strlist_append_unique(&cfg->bundle_sources, full);
        ecs_os_free(full);
        if (rc != 0) return -1;
    }

    if (!bundle->header_only) {
        static const char *cmake_libdirs[] = { "lib", "lib64", "lib32", NULL };
        static const char *cargo_libdirs[] = { "release", NULL };
        const char *const *libdirs;
        const char *base;
        if (bake_bundle_uses_cargo(bundle)) {
            libdirs = cargo_libdirs;
            base = install_dir;
        } else {
            libdirs = cmake_libdirs;
            base = install_dir;
        }
        char *libpath = NULL;
        if (bake_bundle_resolve_lib(bundle, base, libdirs, &libpath) != 0) {
            const char *libname = (bundle->library && bundle->library[0]) ? bundle->library : bundle->id;
            ecs_err("bundle '%s': could not locate library '%s' under %s", bundle->id, libname, base);
            return -1;
        }

        if (bake_strlist_append_unique(&cfg->bundle_libpaths, libpath) != 0) {
            ecs_os_free(libpath);
            return -1;
        }

#if !defined(_WIN32)
        /* Cargo builds typically produce a shared library next to the static
         * one; embed an rpath so the resulting binary can locate it at run
         * time. Windows uses different mechanisms (DLL search path) and is
         * not handled here. */
        if (bake_bundle_uses_cargo(bundle)) {
            char *rpath = flecs_asprintf("-Wl,-rpath,%s", libpath);
            int rc = rpath ? bake_strlist_append_unique(&cfg->bundle_ldflags, rpath) : -1;
            ecs_os_free(rpath);
            if (rc != 0) {
                ecs_os_free(libpath);
                return -1;
            }
        }
#endif

        ecs_os_free(libpath);

        const char *libname = (bundle->library && bundle->library[0]) ? bundle->library : bundle->id;
        if (bake_strlist_append_unique(&cfg->bundle_libs, libname) != 0) {
            return -1;
        }
    }

    if (bake_strlist_merge_unique(&cfg->bundle_libs, &bundle->libs) != 0) {
        return -1;
    }

    if (bake_strlist_merge_unique(&cfg->bundle_ldflags, &bundle->ldflags) != 0) {
        return -1;
    }

    return 0;
}

static int bake_bundle_prepare_one(
    bake_context_t *ctx,
    bake_project_cfg_t *cfg,
    const bake_bundle_t *bundle)
{
    if (!bundle || !bundle->id || !bundle->id[0]) {
        return -1;
    }

    int rc = -1;
    char *triplet = bake_host_triplet(ctx->opts.mode);
    char *root_dir = bake_bundle_root_dir(cfg, bundle);
    char *src_dir = bake_bundle_source_dir(root_dir);
    char *build_dir = triplet ? bake_bundle_build_dir(root_dir, triplet) : NULL;
    char *install_dir = triplet ? bake_bundle_install_dir(root_dir, triplet) : NULL;
    char *marker = install_dir ? bake_bundle_install_marker(install_dir) : NULL;
    char *cmake_src_dir = NULL;

    if (!triplet || !root_dir || !src_dir || !build_dir || !install_dir || !marker) {
        goto cleanup;
    }

    if (!bake_path_exists(src_dir)) {
        char *src_root = bake_path_dirname(src_dir);
        if (src_root && bake_os_mkdirs(src_root) != 0) {
            ecs_os_free(src_root);
            goto cleanup;
        }
        ecs_os_free(src_root);

        ecs_trace("#[green][#[normal] bundle#[green]]#[normal] fetching %s", bundle->id);
        if (bake_bundle_clone(bundle, src_dir) != 0) {
            ecs_err("failed to clone bundle '%s'", bundle->id);
            goto cleanup;
        }
    }

    cmake_src_dir = (bundle->subdir && bundle->subdir[0])
        ? bake_path_join(src_dir, bundle->subdir)
        : ecs_os_strdup(src_dir);
    if (!cmake_src_dir) {
        goto cleanup;
    }

    if (!bundle->header_only) {
        bool uses_cargo = bake_bundle_uses_cargo(bundle);

        if (!uses_cargo) {
            char *cmakelists = bake_path_join(cmake_src_dir, "CMakeLists.txt");
            bool has_cmake = cmakelists && bake_path_exists(cmakelists);
            ecs_os_free(cmakelists);

            if (!has_cmake) {
                ecs_err("bundle '%s' has no CMakeLists.txt at %s", bundle->id, cmake_src_dir);
                goto cleanup;
            }
        } else {
            char *cargo_toml = bake_path_join(cmake_src_dir, "Cargo.toml");
            bool has_cargo = cargo_toml && bake_path_exists(cargo_toml);
            ecs_os_free(cargo_toml);

            if (!has_cargo) {
                ecs_err("bundle '%s' has no Cargo.toml at %s", bundle->id, cmake_src_dir);
                goto cleanup;
            }
        }

        if (!bake_path_exists(marker)) {
            if (bake_os_mkdirs(build_dir) != 0 || bake_os_mkdirs(install_dir) != 0) {
                goto cleanup;
            }

            ecs_trace("#[green][#[normal] bundle#[green]]#[normal] building %s", bundle->id);
            int rc = uses_cargo
                ? bake_bundle_run_cargo(bundle, cmake_src_dir, install_dir)
                : bake_bundle_run_cmake(bundle, cmake_src_dir, build_dir, install_dir, ctx->opts.mode);
            if (rc != 0) {
                ecs_err("failed to build bundle '%s'", bundle->id);
                goto cleanup;
            }

            if (bake_file_write(marker, "ok\n") != 0) {
                goto cleanup;
            }
        }
    }

    if (bake_bundle_apply_to_project(cfg, bundle, cmake_src_dir, install_dir) != 0) {
        goto cleanup;
    }

    rc = 0;

cleanup:
    ecs_os_free(triplet);
    ecs_os_free(root_dir);
    ecs_os_free(src_dir);
    ecs_os_free(build_dir);
    ecs_os_free(install_dir);
    ecs_os_free(marker);
    ecs_os_free(cmake_src_dir);
    return rc;
}

bool bake_bundle_is_declared(const bake_project_cfg_t *cfg, const char *id) {
    if (!cfg || !id) {
        return false;
    }
    return bake_bundle_list_find(&cfg->bundles, id) != NULL;
}

int bake_bundle_prepare_for_project(bake_context_t *ctx, bake_project_cfg_t *cfg) {
    if (!ctx || !cfg) {
        return 0;
    }

    const char *cmd = ctx->opts.command;
    if (cmd && (!strcmp(cmd, "clean") || !strcmp(cmd, "info") ||
                !strcmp(cmd, "list") || !strcmp(cmd, "cleanup") ||
                !strcmp(cmd, "reset")))
    {
        return 0;
    }

    int32_t count = bake_bundle_list_count(&cfg->bundles);
    for (int32_t i = 0; i < count; i++) {
        const bake_bundle_t *bundle = bake_bundle_list_get(&cfg->bundles, i);
        if (!bundle) continue;
        if (bake_bundle_prepare_one(ctx, cfg, bundle) != 0) {
            return -1;
        }
    }

    return 0;
}
