/**
 * @file
 * @brief Collects lines of code per project with cloc for the build report.
 */

#include "build_internal.h"
#include "bake/build_report.h"
#include "bake/os.h"
#include "common/json_helpers.h"

#include "parson.h"
#include <flecs.h>

/** @brief Directory names cloc never descends into. */
#define BAKE_LOC_EXCLUDE_DIRS ".bake,generated,deps,bundles"

/**
 * @brief Per language line counts, accumulated across cloc runs.
 */
typedef struct bake_loc_lang_total_t {
    char *language;
    int32_t files;
    int32_t code;
    int32_t comment;
    int32_t blank;
} bake_loc_lang_total_t;

/**
 * @brief Aggregated line counts for a project or the whole workspace.
 */
typedef struct bake_loc_totals_t {
    int32_t files;
    int32_t code;
    int32_t comment;
    int32_t blank;
    bake_loc_lang_total_t *languages;
    int32_t language_count;
} bake_loc_totals_t;

/**
 * @brief Frees the language entries owned by a totals accumulator.
 * @param totals Accumulator to finalize.
 */
static void bake_loc_totals_fini(bake_loc_totals_t *totals) {
    for (int32_t i = 0; i < totals->language_count; i++) {
        ecs_os_free(totals->languages[i].language);
    }

    ecs_os_free(totals->languages);
}

/**
 * @brief Adds a language's counts to a totals accumulator.
 * @param totals Accumulator to update.
 * @param language Language name as reported by cloc.
 * @param files Number of files of that language.
 * @param code Code lines.
 * @param comment Comment lines.
 * @param blank Blank lines.
 */
static void bake_loc_totals_add(
    bake_loc_totals_t *totals,
    const char *language,
    int32_t files,
    int32_t code,
    int32_t comment,
    int32_t blank)
{
    for (int32_t i = 0; i < totals->language_count; i++) {
        if (!strcmp(totals->languages[i].language, language)) {
            totals->languages[i].files += files;
            totals->languages[i].code += code;
            totals->languages[i].comment += comment;
            totals->languages[i].blank += blank;

            return;
        }
    }

    totals->languages = ecs_os_realloc_n(
        totals->languages, bake_loc_lang_total_t, totals->language_count + 1);
    bake_loc_lang_total_t *entry = &totals->languages[totals->language_count++];
    entry->language = ecs_os_strdup(language);
    entry->files = files;
    entry->code = code;
    entry->comment = comment;
    entry->blank = blank;
}

/**
 * @brief Serializes a totals accumulator's languages to a compact JSON object.
 * @param totals Accumulator to serialize.
 * @return A newly allocated JSON object string; never NULL.
 */
static char* bake_loc_serialize_languages(const bake_loc_totals_t *totals) {
    JSON_Value *root_value = json_value_init_object();
    JSON_Object *root = json_value_get_object(root_value);

    for (int32_t i = 0; i < totals->language_count; i++) {
        const bake_loc_lang_total_t *lang = &totals->languages[i];
        JSON_Value *entry_value = json_value_init_object();
        JSON_Object *entry = json_value_get_object(entry_value);
        json_object_set_number(entry, "files", lang->files);
        json_object_set_number(entry, "code", lang->code);
        json_object_set_number(entry, "comment", lang->comment);
        json_object_set_number(entry, "blank", lang->blank);
        json_object_set_value(root, lang->language, entry_value);
    }

    char *serialized = json_serialize_to_string(root_value);
    char *out = ecs_os_strdup(serialized ? serialized : "{}");
    if (serialized) {
        json_free_serialized_string(serialized);
    }

    json_value_free(root_value);

    return out;
}

/**
 * @brief Checks whether a directory holds a usable copy of an executable.
 * @param dir Directory to look in.
 * @param exe Executable file name, with extension.
 * @return True when the file exists.
 */
static bool bake_loc_dir_has_exe(const char *dir, const char *exe) {
    char *joined = bake_path_join(dir, exe);
    bool found = bake_path_exists(joined) != 0;
    ecs_os_free(joined);

    return found;
}

/**
 * @brief Checks whether the cloc executable is reachable on PATH.
 * @return True when cloc is available.
 */
static bool bake_loc_tool_available(void) {
    const char *path = getenv("PATH");
    if (!path || !path[0]) {
        return false;
    }

#ifdef _WIN32
    const char *exe_name = "cloc.exe";
    const char sep = ';';
#else
    const char *exe_name = "cloc";
    const char sep = ':';
#endif

    bool found = false;
    const char *start = path;
    while (*start && !found) {
        const char *next = strchr(start, sep);
        size_t len = next ? (size_t)(next - start) : strlen(start);
        if (len) {
            char *dir = ecs_os_malloc(len + 1);
            memcpy(dir, start, len);
            dir[len] = '\0';
            found = bake_loc_dir_has_exe(dir, exe_name);
            ecs_os_free(dir);
        }

        start = next ? next + 1 : start + len;
    }

    return found;
}

/**
 * @brief Runs cloc over a project's source and include folders.
 * @param cfg Project to scan.
 * @param scratch_dir Directory for the temporary cloc output files.
 * @param totals Receives the counts; left at zero when the folders are absent.
 * @return 0 when the scan produced usable output, non zero otherwise.
 */
static int bake_loc_run_cloc(
    const bake_project_cfg_t *cfg,
    const char *scratch_dir,
    bake_loc_totals_t *totals)
{
    int rc = -1;
    char *src_dir = bake_path_join(cfg->path, "src");
    char *include_dir = bake_path_join(cfg->path, "include");
    bool have_src = bake_path_exists(src_dir) != 0;
    bool have_include = bake_path_exists(include_dir) != 0;

    if (!have_src && !have_include) {
        rc = 0;
        goto done;
    }

    {
        char *out_path = flecs_asprintf("%s/bake_loc_%s.json", scratch_dir, cfg->id);
        char *err_path = flecs_asprintf("%s/bake_loc_%s.err", scratch_dir, cfg->id);
        const char *argv[8];
        int32_t argc = 0;
        argv[argc++] = "cloc";
        argv[argc++] = "--json";
        argv[argc++] = "--quiet";
        argv[argc++] = "--exclude-dir=" BAKE_LOC_EXCLUDE_DIRS;
        if (have_src) {
            argv[argc++] = src_dir;
        }
        if (have_include) {
            argv[argc++] = include_dir;
        }
        argv[argc] = NULL;

        bake_process_stdio_t stdio_cfg = {
            .stdout_path = out_path,
            .stderr_path = err_path
        };
        bake_process_result_t result = {0};
        int spawn_rc = bake_proc_run(argv, &stdio_cfg, &result);

        JSON_Value *root_value = (spawn_rc == 0 && result.exit_code == 0)
            ? json_parse_file(out_path)
            : NULL;

        if (root_value && json_value_get_type(root_value) == JSONObject) {
            JSON_Object *root = json_value_get_object(root_value);
            size_t count = json_object_get_count(root);
            for (size_t i = 0; i < count; i++) {
                const char *name = json_object_get_name(root, i);
                if (!strcmp(name, "header") || !strcmp(name, "SUM")) {
                    continue;
                }

                JSON_Value *entry_value = json_object_get_value_at(root, i);
                if (json_value_get_type(entry_value) != JSONObject) {
                    continue;
                }

                JSON_Object *entry = json_value_get_object(entry_value);
                int32_t files = (int32_t)json_object_get_number(entry, "nFiles");
                int32_t code = (int32_t)json_object_get_number(entry, "code");
                int32_t comment = (int32_t)json_object_get_number(entry, "comment");
                int32_t blank = (int32_t)json_object_get_number(entry, "blank");

                bake_loc_totals_add(totals, name, files, code, comment, blank);
                totals->files += files;
                totals->code += code;
                totals->comment += comment;
                totals->blank += blank;
            }

            rc = 0;
        }

        if (root_value) {
            json_value_free(root_value);
        }

        bake_remove_file_if_exists(out_path);
        bake_remove_file_if_exists(err_path);
        ecs_os_free(out_path);
        ecs_os_free(err_path);
    }

done:
    ecs_os_free(src_dir);
    ecs_os_free(include_dir);

    return rc;
}

void bake_report_collect_loc(
    bake_context_t *ctx,
    const ecs_entity_t *order,
    int32_t count)
{
    if (!ctx->report) {
        return;
    }

    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || !tmpdir[0]) {
        tmpdir = "/tmp";
    }

    bool cloc_available = bake_loc_tool_available();
    bake_loc_totals_t workspace = {0};

    for (int32_t i = 0; i < count; i++) {
        const BakeProject *project = ecs_get(ctx->world, order[i], BakeProject);
        bool own_project = project && project->cfg && !project->external;
        if (!own_project) {
            continue;
        }

        const bake_project_cfg_t *cfg = project->cfg;
        int32_t step = bake_report_open(
            ctx->report, BAKE_REPORT_KIND_LOC, "loc", cfg->id);

        if (!cloc_available) {
            bake_report_close(ctx->report, step, false, "cloc not found");
            continue;
        }

        bake_loc_totals_t totals = {0};
        int rc = bake_loc_run_cloc(cfg, tmpdir, &totals);
        if (rc == 0) {
            char *by_language = bake_loc_serialize_languages(&totals);
            bake_report_set_loc(ctx->report, step,
                totals.files, totals.code, totals.comment, totals.blank,
                by_language);
            ecs_os_free(by_language);

            for (int32_t l = 0; l < totals.language_count; l++) {
                const bake_loc_lang_total_t *lang = &totals.languages[l];
                bake_loc_totals_add(&workspace, lang->language,
                    lang->files, lang->code, lang->comment, lang->blank);
            }

            workspace.files += totals.files;
            workspace.code += totals.code;
            workspace.comment += totals.comment;
            workspace.blank += totals.blank;
        }

        bake_loc_totals_fini(&totals);
        bake_report_close(ctx->report, step, rc == 0,
            rc == 0 ? NULL : "cloc failed");
    }

    if (!cloc_available) {
        bake_report_set_loc_note(ctx->report, "cloc not found");
    } else {
        char *by_language = bake_loc_serialize_languages(&workspace);
        bake_report_set_workspace_loc(ctx->report,
            workspace.files, workspace.code, workspace.comment,
            workspace.blank, by_language);
        ecs_os_free(by_language);
    }

    bake_loc_totals_fini(&workspace);
}
