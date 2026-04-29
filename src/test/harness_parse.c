#include "harness_internal.h"
#include "bake/os.h"

#include "parson.h"
#include "common/json_helpers.h"

static void bake_param_spec_fini(bake_param_spec_t *param) {
    if (!param) {
        return;
    }

    ecs_os_free(param->name);
    bake_strlist_fini(&param->values);
    memset(param, 0, sizeof(*param));
}

static void bake_suite_spec_fini(bake_suite_spec_t *suite) {
    if (!suite) {
        return;
    }

    ecs_os_free(suite->id);
    bake_strlist_fini(&suite->testcases);

    for (int32_t i = 0; i < suite->param_count; i++) {
        bake_param_spec_fini(&suite->params[i]);
    }
    ecs_os_free(suite->params);

    memset(suite, 0, sizeof(*suite));
}

void bake_suite_list_fini(bake_suite_list_t *list) {
    for (int32_t i = 0; i < list->count; i++) {
        bake_suite_spec_fini(&list->items[i]);
    }

    ecs_os_free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int bake_suite_list_append(bake_suite_list_t *list, bake_suite_spec_t *suite) {
    if (list->count == list->capacity) {
        int32_t new_cap = list->capacity ? list->capacity * 2 : 8;
        bake_suite_spec_t *p = ecs_os_realloc_n(list->items, bake_suite_spec_t, new_cap);
        if (!p) return -1;
        list->items = p;
        list->capacity = new_cap;
    }
    list->items[list->count++] = *suite;
    return 0;
}

static int bake_suite_param_append(bake_suite_spec_t *suite, bake_param_spec_t *param) {
    if (suite->param_count == suite->param_capacity) {
        int32_t new_cap = suite->param_capacity ? suite->param_capacity * 2 : 4;
        bake_param_spec_t *p = ecs_os_realloc_n(suite->params, bake_param_spec_t, new_cap);
        if (!p) return -1;
        suite->params = p;
        suite->param_capacity = new_cap;
    }
    suite->params[suite->param_count++] = *param;
    return 0;
}

static int bake_parse_test_cases(JSON_Array *tests, bake_suite_spec_t *suite) {
    size_t testcase_count = json_array_get_count(tests);
    for (size_t t = 0; t < testcase_count; t++) {
        JSON_Value *test_value = json_array_get_value(tests, t);
        char *name = bake_json_strdup_value(test_value);
        if (!name || bake_strlist_append_owned(&suite->testcases, name) != 0) {
            ecs_os_free(name);
            return -1;
        }
    }
    return 0;
}

static int bake_parse_test_parameters(const JSON_Object *params_obj, bake_suite_spec_t *suite) {
    size_t param_count = json_object_get_count(params_obj);
    for (size_t p = 0; p < param_count; p++) {
        const char *param_name = json_object_get_name(params_obj, p);
        JSON_Value *param_value = json_object_get_value_at(params_obj, p);
        if (!param_name || !param_value || json_value_get_type(param_value) != JSONArray) {
            return -1;
        }

        bake_param_spec_t param = {0};
        bake_strlist_init(&param.values);
        param.name = ecs_os_strdup(param_name);
        if (!param.name) {
            bake_param_spec_fini(&param);
            return -1;
        }

        JSON_Array *values = json_value_get_array(param_value);
        size_t value_count = values ? json_array_get_count(values) : 0;
        for (size_t v = 0; v < value_count; v++) {
            JSON_Value *value = json_array_get_value(values, v);
            char *value_str = bake_json_strdup_value(value);
            if (!value_str || bake_strlist_append_owned(&param.values, value_str) != 0) {
                ecs_os_free(value_str);
                bake_param_spec_fini(&param);
                return -1;
            }
        }

        if (bake_suite_param_append(suite, &param) != 0) {
            bake_param_spec_fini(&param);
            return -1;
        }
    }
    return 0;
}

static int bake_parse_test_suite(const JSON_Object *suite_obj, bake_suite_list_t *out) {
    bake_suite_spec_t suite = {0};
    bake_strlist_init(&suite.testcases);

    const char *id = json_object_get_string(suite_obj, "id");
    JSON_Array *tests = json_object_get_array(suite_obj, "testcases");
    if (!id || !tests) {
        bake_suite_spec_fini(&suite);
        return -1;
    }

    suite.id = ecs_os_strdup(id);
    if (!suite.id) {
        bake_suite_spec_fini(&suite);
        return -1;
    }

    suite.setup = json_object_get_boolean(suite_obj, "setup") == 1;
    suite.teardown = json_object_get_boolean(suite_obj, "teardown") == 1;

    if (bake_parse_test_cases(tests, &suite) != 0) {
        bake_suite_spec_fini(&suite);
        return -1;
    }

    const JSON_Object *params_obj = json_object_get_object(suite_obj, "params");
    if (params_obj) {
        if (bake_parse_test_parameters(params_obj, &suite) != 0) {
            bake_suite_spec_fini(&suite);
            return -1;
        }
    }

    if (bake_suite_list_append(out, &suite) != 0) {
        bake_suite_spec_fini(&suite);
        return -1;
    }

    return 0;
}

int bake_parse_project_tests(const char *path, bake_suite_list_t *out) {
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

    const JSON_Object *test_obj = json_object_get_object(root, "test");
    if (!test_obj) {
        json_value_free(root_value);
        ecs_os_free(json);
        return 0;
    }

    JSON_Array *suites = json_object_get_array(test_obj, "testsuites");
    if (!suites) {
        json_value_free(root_value);
        ecs_os_free(json);
        return 0;
    }

    int rc = 0;
    size_t suite_count = json_array_get_count(suites);
    for (size_t i = 0; i < suite_count; i++) {
        const JSON_Object *suite_obj = json_array_get_object(suites, i);
        if (!suite_obj || bake_parse_test_suite(suite_obj, out) != 0) {
            rc = -1;
            break;
        }
    }

    json_value_free(root_value);
    ecs_os_free(json);
    return rc;
}
