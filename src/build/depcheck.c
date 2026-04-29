#include "build_internal.h"
#include "depcheck_internal.h"
#include "bake/os.h"

#include <ctype.h>

static bool bake_dep_char_is_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

bool bake_dep_token_outdated(const char *token, int64_t obj_mtime) {
    if (!token[0]) {
        return false;
    }

    int64_t mtime = bake_os_file_mtime(token);
    if (mtime < 0) {
        return true;
    }

    return mtime > obj_mtime;
}

bool bake_depfile_outdated(const char *dep_path, int64_t obj_mtime) {
    size_t len = 0;
    char *content = bake_file_read(dep_path, &len);
    if (!content) {
        return true;
    }

    bool seen_colon = false;
    bool outdated = false;
    size_t token_cap = 256;
    size_t token_len = 0;
    char *token = ecs_os_malloc(token_cap);
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

        if (ch == '\\' && (i + 2) < len && content[i + 1] == '\r' && content[i + 2] == '\n') {
            i += 2;
            continue;
        }

        if (ch == '\\' && (i + 1) < len) {
            if ((token_len + 2) >= token_cap) {
                size_t next_cap = token_cap * 2;
                if (next_cap <= token_cap) {
                    ecs_os_free(token);
                    ecs_os_free(content);
                    return true;
                }
                char *next = ecs_os_realloc_n(token, char, next_cap);
                if (!next) {
                    ecs_os_free(token);
                    ecs_os_free(content);
                    return true;
                }
                token = next;
                token_cap = next_cap;
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
            size_t next_cap = token_cap * 2;
            if (next_cap <= token_cap) {
                ecs_os_free(token);
                ecs_os_free(content);
                return true;
            }
            char *next = ecs_os_realloc_n(token, char, next_cap);
            if (!next) {
                ecs_os_free(token);
                ecs_os_free(content);
                return true;
            }
            token = next;
            token_cap = next_cap;
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

int64_t bake_project_json_mtime(const bake_project_cfg_t *cfg) {
    if (!cfg || !cfg->path || !cfg->path[0]) {
        return -1;
    }

    char *project_json = bake_path_join(cfg->path, "project.json");
    if (!project_json) {
        return -1;
    }

    int64_t mtime = bake_os_file_mtime(project_json);
    ecs_os_free(project_json);
    return mtime;
}

bool bake_compile_unit_outdated(
    const bake_compile_unit_t *unit,
    int64_t project_json_mtime)
{
    if (!unit || !unit->src || !unit->obj) {
        return true;
    }

    if (!bake_path_exists(unit->obj)) {
        return true;
    }

    int64_t obj_mtime = bake_os_file_mtime(unit->obj);
    int64_t src_mtime = bake_os_file_mtime(unit->src);
    if (obj_mtime < 0 || src_mtime < 0) {
        return true;
    }

    if (src_mtime > obj_mtime) {
        return true;
    }

    if (project_json_mtime < 0 || project_json_mtime > obj_mtime) {
        return true;
    }

    if (unit->dep) {
        int64_t dep_mtime = bake_os_file_mtime(unit->dep);
        if (dep_mtime < 0 || dep_mtime < src_mtime) {
            return true;
        }
        if (bake_depfile_outdated(unit->dep, obj_mtime)) {
            return true;
        }
    }

    return false;
}

bool bake_project_json_outdated(
    const bake_project_cfg_t *cfg,
    int64_t artefact_mtime)
{
    int64_t pj_mtime = bake_project_json_mtime(cfg);
    if (pj_mtime < 0) {
        return true;
    }

    return pj_mtime > artefact_mtime;
}

char* bake_library_name_from_artefact(const char *artefact) {
    char *name = bake_path_basename(artefact);
    if (!name) {
        return NULL;
    }

#if defined(_WIN32)
    const char *suffix = ".lib";
    size_t len = strlen(name);
    size_t suffix_len = strlen(suffix);
    if (len > suffix_len && !strcmp(name + len - suffix_len, suffix)) {
        name[len - suffix_len] = '\0';
    }
#else
    const char *suffix = ".a";
    size_t len = strlen(name);
    size_t suffix_len = strlen(suffix);
    if (len > suffix_len && !strcmp(name + len - suffix_len, suffix)) {
        name[len - suffix_len] = '\0';
    }
    if (!strncmp(name, "lib", 3)) {
        memmove(name, name + 3, strlen(name + 3) + 1);
    }
#endif

    return name;
}

bool bake_has_dep_artefact_for_lib(
    const bake_strlist_t *artefacts,
    const char *lib)
{
    if (!artefacts || !lib || !lib[0]) {
        return false;
    }

    for (int32_t i = 0; i < artefacts->count; i++) {
        char *name = bake_library_name_from_artefact(artefacts->items[i]);
        if (!name) {
            continue;
        }

        bool match = !strcmp(name, lib);
        ecs_os_free(name);
        if (match) {
            return true;
        }
    }

    return false;
}
