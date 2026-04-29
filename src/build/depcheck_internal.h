#ifndef BAKE_DEPCHECK_INTERNAL_H
#define BAKE_DEPCHECK_INTERNAL_H

#include "build_internal.h"

bool bake_dep_token_outdated(const char *token, int64_t obj_mtime);
bool bake_depfile_outdated(const char *dep_path, int64_t obj_mtime);
int64_t bake_project_json_mtime(const bake_project_cfg_t *cfg);
bool bake_compile_unit_outdated(
    const bake_compile_unit_t *unit,
    int64_t project_json_mtime);
bool bake_project_json_outdated(
    const bake_project_cfg_t *cfg,
    int64_t artefact_mtime);
char* bake_library_name_from_artefact(const char *artefact);
bool bake_has_dep_artefact_for_lib(
    const bake_strlist_t *artefacts,
    const char *lib);

#endif
