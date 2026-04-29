#include "json_helpers.h"
#include "bake/os.h"
#include <flecs.h>

char* bake_json_strdup_value(const JSON_Value *value) {
    if (!value) {
        return NULL;
    }

    JSON_Value_Type type = json_value_get_type(value);
    if (type == JSONString) {
        const char *str = json_value_get_string(value);
        return str ? ecs_os_strdup(str) : NULL;
    }

    if (type == JSONBoolean) {
        return ecs_os_strdup(json_value_get_boolean(value) ? "true" : "false");
    }

    if (type == JSONNull) {
        return ecs_os_strdup("null");
    }

    char *serialized = json_serialize_to_string(value);
    if (!serialized) {
        return NULL;
    }

    char *out = ecs_os_strdup(serialized);
    json_free_serialized_string(serialized);
    return out;
}

int bake_json_parse_strlist(const JSON_Array *array, bake_strlist_t *list) {
    size_t count = json_array_get_count(array);
    for (size_t i = 0; i < count; i++) {
        JSON_Value *value = json_array_get_value(array, i);
        JSON_Value_Type type = json_value_get_type(value);
        if (type == JSONArray || type == JSONObject) {
            return -1;
        }

        char *str = bake_json_strdup_value(value);
        if (!str) {
            return -1;
        }

        if (bake_strlist_append_owned(list, str) != 0) {
            return -1;
        }
    }

    return 0;
}

int bake_json_get_string(const JSON_Object *object, const char *key, char **out) {
    JSON_Value *value = json_object_get_value(object, key);
    if (!value) {
        return 1;
    }

    JSON_Value_Type type = json_value_get_type(value);
    if (type == JSONObject || type == JSONArray) {
        return -1;
    }

    char *str = bake_json_strdup_value(value);
    if (!str) {
        return -1;
    }

    ecs_os_free(*out);
    *out = str;
    return 0;
}

int bake_json_get_bool(const JSON_Object *object, const char *key, bool *out) {
    JSON_Value *value = json_object_get_value(object, key);
    if (!value) {
        return 1;
    }

    if (json_value_get_type(value) != JSONBoolean) {
        return -1;
    }

    *out = json_value_get_boolean(value) != 0;
    return 0;
}

int bake_json_get_array(const JSON_Object *object, const char *key, bake_strlist_t *out) {
    JSON_Value *value = json_object_get_value(object, key);
    if (!value) {
        return 1;
    }

    if (json_value_get_type(value) != JSONArray) {
        return -1;
    }

    return bake_json_parse_strlist(json_value_get_array(value), out);
}

int bake_json_get_array_alias(
    const JSON_Object *object,
    const char *key,
    const char *alias,
    bake_strlist_t *out)
{
    int rc = bake_json_get_array(object, key, out);
    if (rc == 1 && alias) {
        rc = bake_json_get_array(object, alias, out);
    }
    return rc;
}

int bake_json_get_string_alias(
    const JSON_Object *object,
    const char *key,
    const char *alias,
    char **out)
{
    int rc = bake_json_get_string(object, key, out);
    if (rc == 1 && alias) {
        rc = bake_json_get_string(object, alias, out);
    }
    return rc;
}

int bake_json_get_object_optional(
    const JSON_Object *object,
    const char *key,
    const JSON_Object **out)
{
    *out = NULL;

    JSON_Value *value = json_object_get_value(object, key);
    if (!value) {
        return 0;
    }

    if (json_value_get_type(value) != JSONObject) {
        return -1;
    }

    *out = json_value_get_object(value);
    return 0;
}
