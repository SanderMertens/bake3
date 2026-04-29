#ifndef BAKE3_JSON_HELPERS_H
#define BAKE3_JSON_HELPERS_H

#include "bake/common.h"
#include "bake/strlist.h"

#include "parson.h"

char* bake_json_strdup_value(const JSON_Value *value);

int bake_json_parse_strlist(const JSON_Array *array, bake_strlist_t *list);

int bake_json_get_string(const JSON_Object *object, const char *key, char **out);

int bake_json_get_bool(const JSON_Object *object, const char *key, bool *out);

int bake_json_get_array(const JSON_Object *object, const char *key, bake_strlist_t *out);

int bake_json_get_array_alias(
    const JSON_Object *object,
    const char *key,
    const char *alias,
    bake_strlist_t *out);

int bake_json_get_string_alias(
    const JSON_Object *object,
    const char *key,
    const char *alias,
    char **out);

int bake_json_get_object_optional(
    const JSON_Object *object,
    const char *key,
    const JSON_Object **out);

#endif
