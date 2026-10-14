/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_json.c - Owned base JSON values with checked MCP publication
 *
 * KEY CONCEPT:
 *   The base DOM remains the only JSON representation. Unreceived children
 *   and failed keys are reclaimed before any caller can observe the tree.
 */
#include "xmcp_json.h"
#include "../../base/xmalloc.h"
#include <string.h>
static XR_THREAD_LOCAL bool builder_failed;
XR_FUNC void xmcp_json_begin(void) { builder_failed = false; }
XR_FUNC void xmcp_json_fail(void) { builder_failed = true; }
XR_FUNC bool xmcp_json_failed(void) { return builder_failed; }
XR_FUNC XrJsonValue *xmcp_json_finish(XrJsonValue *owned) {
    if (!owned) builder_failed = true;
    if (builder_failed) { xjson_free(owned); return NULL; }
    return owned;
}
XR_FUNC bool xmcp_json_set(XrJsonValue *object, const char *key, XrJsonValue *owned) {
    if (builder_failed || !xjson_is_object(object) || !key || !owned) {
        xjson_free(owned); builder_failed = true; return false;
    }
    int before = object->as.object.count;
    xjson_object_set(object, key, owned);
    if (object->as.object.count == before + 1) {
        XrJsonMember *member = &object->as.object.members[before];
        if (member->key) return true;
        /* The base setter can publish a NULL key when copying it fails. */
        --object->as.object.count;
        memset(member, 0, sizeof(*member));
    } else if (xjson_get(object, key) == owned) return true;
    xjson_free(owned); builder_failed = true; return false;
}
XR_FUNC bool xmcp_json_push(XrJsonValue *array, XrJsonValue *owned) {
    if (builder_failed || !xjson_is_array(array) || !owned) {
        xjson_free(owned); builder_failed = true; return false;
    }
    int before = array->as.array.count;
    xjson_array_push(array, owned);
    if (array->as.array.count == before + 1) return true;
    xjson_free(owned); builder_failed = true; return false;
}
static XrJsonValue *clone_value(const XrJsonValue *value, unsigned depth) {
    if (builder_failed || !value || depth > 64) { builder_failed = true; return NULL; }
    XrJsonValue *copy = NULL;
    switch (value->type) {
    case XR_JSON_NULL: copy = xjson_new_null(); break;
    case XR_JSON_BOOL: copy = xjson_new_bool(value->as.boolean); break;
    case XR_JSON_NUMBER:
        copy = xjson_new_number(0);
        if (copy) { copy->is_integer = value->is_integer; copy->as = value->as; }
        break;
    case XR_JSON_STRING: copy = xjson_new_string_n(value->as.string, value->string_len); break;
    case XR_JSON_ARRAY:
        copy = xjson_new_array();
        for (int i = 0; copy && !builder_failed && i < value->as.array.count; ++i)
            xmcp_json_push(copy, clone_value(value->as.array.items[i], depth + 1));
        break;
    case XR_JSON_OBJECT:
        copy = xjson_new_object();
        for (int i = 0; copy && !builder_failed && i < value->as.object.count; ++i) {
            const XrJsonMember *member = &value->as.object.members[i];
            if (!member->key || strlen(member->key) != member->key_len) { builder_failed = true; break; }
            xmcp_json_set(copy, member->key, clone_value(member->value, depth + 1));
        }
        break;
    default: builder_failed = true; break;
    }
    return xmcp_json_finish(copy);
}
XR_FUNC XrJsonValue *xmcp_json_clone(const XrJsonValue *borrowed) { return clone_value(borrowed, 0); }
