/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_params.c - Failure-atomic JSON capture on the original ledger
 */
#include "xlsp_source_params.h"
#include <string.h>
struct XlspSourceParams { XrJsonValue *root; };

static void params_value_free(XrJsonValue *value) {
    if (!value) return;
    if (value->type == XR_JSON_STRING) xr_compile_resources_free(value->as.string);
    if (value->type == XR_JSON_ARRAY) {
        for (int i = 0; i < value->as.array.count; ++i) params_value_free(value->as.array.items[i]);
        xr_compile_resources_free(value->as.array.items);
    }
    if (value->type == XR_JSON_OBJECT) {
        for (int i = 0; i < value->as.object.count; ++i) {
            xr_compile_resources_free(value->as.object.members[i].key);
            params_value_free(value->as.object.members[i].value);
        }
        xr_compile_resources_free(value->as.object.members);
    }
    xr_compile_resources_free(value);
}
static XrCompileResourceStatus params_bytes(XrCompileResources *resources,
    const char *bytes, size_t length, char **out) {
    if (!bytes || length == SIZE_MAX) return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    XrCompileResourceStatus status = xr_compile_resources_work(resources, length + 1);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    status = xr_compile_resources_alloc(resources, length + 1, (void **)out);
    if (status == XR_COMPILE_RESOURCE_OK) {
        memcpy(*out, bytes, length); (*out)[length] = 0;
    }
    return status;
}
static XrCompileResourceStatus params_value(XrCompileResources *, const XrJsonValue *,
    unsigned, XrJsonValue **);
static XrCompileResourceStatus params_array(XrCompileResources *resources,
    const XrJsonValue *input, unsigned depth, XrJsonValue *value) {
    int count = input->as.array.count;
    if (count < 0 || input->as.array.capacity < count || (count && !input->as.array.items))
        return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    if ((size_t)count > SIZE_MAX / sizeof(*value->as.array.items)) return XR_COMPILE_RESOURCE_BUDGET;
    XrCompileResourceStatus status = XR_COMPILE_RESOURCE_OK;
    if (count) status = xr_compile_resources_calloc(resources, (size_t)count,
        sizeof(*value->as.array.items), (void **)&value->as.array.items);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    value->as.array.capacity = count;
    for (int i = 0; i < count; ++i) {
        status = params_value(resources, input->as.array.items[i], depth + 1, &value->as.array.items[i]);
        if (status != XR_COMPILE_RESOURCE_OK) return status;
        ++value->as.array.count;
    }
    return status;
}
static XrCompileResourceStatus params_object(XrCompileResources *resources,
    const XrJsonValue *input, unsigned depth, XrJsonValue *value) {
    int count = input->as.object.count;
    if (count < 0 || input->as.object.capacity < count || (count && !input->as.object.members))
        return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    if ((size_t)count > SIZE_MAX / sizeof(*value->as.object.members)) return XR_COMPILE_RESOURCE_BUDGET;
    XrCompileResourceStatus status = XR_COMPILE_RESOURCE_OK;
    if (count) status = xr_compile_resources_calloc(resources, (size_t)count,
        sizeof(*value->as.object.members), (void **)&value->as.object.members);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    value->as.object.capacity = count;
    for (int i = 0; i < count; ++i) {
        const XrJsonMember *input_member = &input->as.object.members[i];
        XrJsonMember *member = &value->as.object.members[i];
        status = params_bytes(resources, input_member->key, input_member->key_len, &member->key);
        if (status != XR_COMPILE_RESOURCE_OK) return status;
        member->key_len = input_member->key_len;
        // Include the initialized key in cleanup before cloning its child.
        ++value->as.object.count;
        status = params_value(resources, input_member->value, depth + 1, &member->value);
        if (status != XR_COMPILE_RESOURCE_OK) return status;
    }
    return status;
}
static XrCompileResourceStatus params_value(XrCompileResources *resources,
    const XrJsonValue *input, unsigned depth, XrJsonValue **out) {
    if (!input || depth >= XJSON_MAX_DEPTH || input->type < XR_JSON_NULL || input->type > XR_JSON_OBJECT)
        return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    XrCompileResourceStatus status = xr_compile_resources_work(resources, 1);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    XrJsonValue *value = NULL;
    status = xr_compile_resources_calloc(resources, 1, sizeof(*value), (void **)&value);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    value->type = input->type; value->is_integer = input->is_integer; value->string_len = input->string_len;
    switch (input->type) {
    case XR_JSON_NULL: break;
    case XR_JSON_BOOL: value->as.boolean = input->as.boolean; break;
    case XR_JSON_NUMBER:
        if (input->is_integer) value->as.integer = input->as.integer;
        else value->as.number = input->as.number;
        break;
    case XR_JSON_STRING: status = params_bytes(resources, input->as.string, input->string_len, &value->as.string); break;
    case XR_JSON_ARRAY: status = params_array(resources, input, depth, value); break;
    case XR_JSON_OBJECT: status = params_object(resources, input, depth, value); break;
    }
    if (status != XR_COMPILE_RESOURCE_OK) { params_value_free(value); return status; }
    *out = value; return XR_COMPILE_RESOURCE_OK;
}
XR_FUNC XrCompileResourceStatus xlsp_source_params_new(XrCompileResources *resources,
    const XrJsonValue *input, XlspSourceParams **out) {
    if (!resources || !input || !out || *out) return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    XlspSourceParams *params = NULL;
    XrCompileResourceStatus status = xr_compile_resources_calloc(resources, 1, sizeof(*params), (void **)&params);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    status = params_value(resources, input, 0, &params->root);
    if (status != XR_COMPILE_RESOURCE_OK) { xlsp_source_params_free(params); return status; }
    *out = params; return XR_COMPILE_RESOURCE_OK;
}
XR_FUNC const XrJsonValue *xlsp_source_params_view(const XlspSourceParams *params) {
    return params ? params->root : NULL;
}
XR_FUNC void xlsp_source_params_free(XlspSourceParams *params) {
    if (!params) return;
    params_value_free(params->root); xr_compile_resources_free(params);
}
