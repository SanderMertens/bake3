#include "bench_internal.h"
#include "bake/os.h"

#include "parson.h"
#include "common/json_helpers.h"
#include "common/harness_util.h"

static void bake_benchsuite_spec_fini(bake_benchsuite_spec_t *suite) {
    if (!suite) {
        return;
    }

    ecs_os_free(suite->id);
    bake_strlist_fini(&suite->benchcases);
    memset(suite, 0, sizeof(*suite));
}

void bake_benchsuite_list_fini(bake_benchsuite_list_t *list) {
    for (int32_t i = 0; i < list->count; i++) {
        bake_benchsuite_spec_fini(&list->items[i]);
    }

    ecs_os_free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void bake_benchsuite_list_append(
    bake_benchsuite_list_t *list,
    bake_benchsuite_spec_t *suite)
{
    if (list->count == list->capacity) {
        int32_t new_cap = list->capacity ? list->capacity * 2 : 8;
        list->items = ecs_os_realloc_n(list->items, bake_benchsuite_spec_t, new_cap);
        list->capacity = new_cap;
    }
    list->items[list->count++] = *suite;
}

static int bake_parse_bench_cases(JSON_Array *cases, bake_benchsuite_spec_t *suite) {
    size_t benchcase_count = json_array_get_count(cases);
    for (size_t t = 0; t < benchcase_count; t++) {
        JSON_Value *case_value = json_array_get_value(cases, t);
        char *name = bake_json_strdup_value(case_value);
        if (!name) {
            ecs_err("benchsuite '%s': invalid benchcase at index %d",
                suite->id, (int)t);
            return -1;
        }
        if (!bake_harness_symbol_chars_valid(name)) {
            ecs_err("benchsuite '%s': benchcase name '%s' contains invalid characters",
                suite->id, name);
            ecs_os_free(name);
            return -1;
        }
        bake_strlist_append_owned(&suite->benchcases, name);
    }
    return 0;
}

static int bake_parse_bench_suite(
    const JSON_Object *suite_obj,
    bake_benchsuite_list_t *out)
{
    bake_benchsuite_spec_t suite = {0};
    bake_strlist_init(&suite.benchcases);

    const char *id = json_object_get_string(suite_obj, "id");
    JSON_Array *cases = json_object_get_array(suite_obj, "benchcases");
    if (!id || !cases) {
        ecs_err("benchsuite %s%s%s: missing '%s' attribute",
            id ? "'" : "", id ? id : "<unnamed>", id ? "'" : "",
            id ? "benchcases" : "id");
        bake_benchsuite_spec_fini(&suite);
        return -1;
    }

    if (!bake_harness_symbol_valid(id)) {
        ecs_err("benchsuite id '%s' is not a valid C identifier", id);
        bake_benchsuite_spec_fini(&suite);
        return -1;
    }

    if (!json_array_get_count(cases)) {
        ecs_warn("benchsuite '%s' has no benchcases, skipping", id);
        bake_benchsuite_spec_fini(&suite);
        return 0;
    }

    suite.id = ecs_os_strdup(id);
    suite.setup = json_object_get_boolean(suite_obj, "setup") == 1;
    suite.teardown = json_object_get_boolean(suite_obj, "teardown") == 1;

    if (bake_parse_bench_cases(cases, &suite) != 0) {
        bake_benchsuite_spec_fini(&suite);
        return -1;
    }

    bake_benchsuite_list_append(out, &suite);

    return 0;
}

int bake_parse_project_benches(const char *path, bake_benchsuite_list_t *out) {
    char *json = bake_file_read(path, NULL);
    if (!json) {
        return -1;
    }

    JSON_Value *root_value = json_parse_string_with_comments(json);
    const JSON_Object *root = root_value ? json_value_get_object(root_value) : NULL;
    if (!root) {
        json_value_free(root_value);
        ecs_os_free(json);
        return -1;
    }

    const JSON_Object *bench_obj = json_object_get_object(root, "bench");
    if (!bench_obj) {
        json_value_free(root_value);
        ecs_os_free(json);
        return 0;
    }

    JSON_Array *suites = json_object_get_array(bench_obj, "benchsuites");
    if (!suites) {
        json_value_free(root_value);
        ecs_os_free(json);
        return 0;
    }

    int rc = 0;
    size_t suite_count = json_array_get_count(suites);
    for (size_t i = 0; i < suite_count; i++) {
        const JSON_Object *suite_obj = json_array_get_object(suites, i);
        if (!suite_obj || bake_parse_bench_suite(suite_obj, out) != 0) {
            rc = -1;
            break;
        }
    }

    json_value_free(root_value);
    ecs_os_free(json);
    return rc;
}
