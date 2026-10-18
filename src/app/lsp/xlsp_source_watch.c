/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_watch.c - Whole-batch watch-event admission
 *
 * KEY CONCEPT:
 *   Invalid late events cannot trigger earlier cache invalidation. Original
 *   document identities and decoded filesystem paths survive producer death.
 */
#include "xlsp_source_watch.h"
#include "xlsp_index_pool.h"
#include "xlsp_source_workspace.h"
#include "../../shared/xr_utf8_core.h"
#include <string.h>

struct XlspSourceWatch {XrCompileResources *resources; XlspSourceWatchEvent *items; size_t count;};
static XrXirStatus watch_resource(XrCompileResourceStatus status) {
    return status == XR_COMPILE_RESOURCE_OK ? XR_XIR_OK : status == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET :
        status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus watch_work(XrCompileResources *resources, uint64_t units) {
    return watch_resource(xr_compile_resources_work(resources, units));
}
static int watch_byte(void *context, const uint8_t *address, uint8_t *output) {
    if (watch_work(context, 1) != XR_XIR_OK) return 0;
    *output = *address; return 1;
}
static XrXirStatus watch_key(XrCompileResources *resources, const XrJsonMember *member) {
    if (!member->key || member->key_len == SIZE_MAX || !member->value) return XR_XIR_BAD_STRUCTURE;
    for (size_t at = 0; at < member->key_len;) {
        XrUtf8Step step;
        if (!xr_utf8_core_decode_step_read((const uint8_t *)member->key + at, member->key_len - at,
                watch_byte, resources, &step)) return XR_XIR_BUDGET;
        if (step.error != XR_UTF8_OK || !step.scalar) return XR_XIR_BAD_STRUCTURE;
        at += step.consumed;
    }
    return XR_XIR_OK;
}
static XrXirStatus watch_member(XrCompileResources *resources, const XrJsonValue *object,
    const char *key, size_t length, const XrJsonValue **output) {
    if (!object || object->type != XR_JSON_OBJECT || object->as.object.count < 0 ||
        object->as.object.capacity < object->as.object.count ||
        (object->as.object.count && !object->as.object.members)) return XR_XIR_BAD_STRUCTURE;
    const XrJsonValue *found = NULL;
    for (int i = 0; i < object->as.object.count; ++i) {
        XrXirStatus status = watch_work(resources, sizeof(XrJsonMember)); if (status != XR_XIR_OK) return status;
        const XrJsonMember *member = &object->as.object.members[i];
        status = watch_key(resources, member); if (status != XR_XIR_OK) return status;
        status = watch_work(resources, 1); if (status != XR_XIR_OK) return status;
        if (member->key_len != length) continue;
        status = watch_work(resources, length); if (status != XR_XIR_OK) return status;
        if (!memcmp(member->key, key, length)) {
            if (found) return XR_XIR_BAD_STRUCTURE;
            found = member->value;
        }
    }
    *output = found; return XR_XIR_OK;
}
XR_FUNC void xlsp_source_watch_free(XlspSourceWatch *owner) {
    if (!owner) return;
    if (owner->items) for (size_t i = 0; i < owner->count; ++i) {
        xr_compile_resources_free((void *)owner->items[i].uri);
        xr_compile_resources_free((void *)owner->items[i].path);
        xr_compile_resources_free((void *)owner->items[i].index_uri);
    }
    xr_compile_resources_free(owner->items); xr_compile_resources_free(owner);
}
XR_FUNC const XlspSourceWatchEvent *xlsp_source_watch_items(const XlspSourceWatch *owner, size_t *count) {
    if (count) *count = owner ? owner->count : 0;
    return owner ? owner->items : NULL;
}
static XrXirStatus watch_event(XlspSourceWatch *owner, const XrJsonValue *value, size_t index) {
    const XrJsonValue *uri = NULL, *kind = NULL; XrCompileResources *resources = owner->resources;
    XrXirStatus status = watch_member(resources, value, "uri", 3, &uri);
    if (status == XR_XIR_OK) status = watch_member(resources, value, "type", 4, &kind);
    if (status != XR_XIR_OK) return status;
    if (!uri || uri->type != XR_JSON_STRING || !uri->as.string || uri->string_len == SIZE_MAX ||
        !kind || kind->type != XR_JSON_NUMBER) return XR_XIR_BAD_STRUCTURE;
    if (uri->string_len > UINT64_MAX - 1 - sizeof(XlspSourceWatchEvent)) return XR_XIR_BUDGET;
    uint32_t type;
    if (kind->is_integer) {
        if (kind->as.integer < 1 || kind->as.integer > 3) return XR_XIR_BAD_STRUCTURE;
        type = (uint32_t)kind->as.integer;
    } else {
        double number = kind->as.number;
        if (!(number >= 1 && number <= 3)) return XR_XIR_BAD_STRUCTURE;
        type = (uint32_t)number; if ((double)type != number) return XR_XIR_BAD_STRUCTURE;
    }
    XlspSourceWatchEvent *event = &owner->items[index]; char *path = NULL, *canonical = NULL, *copy = NULL;
    status = xlsp_source_uri_path(resources, uri->as.string, uri->string_len, &path);
    if (status == XR_XIR_OK) status = xlsp_index_file_uri(resources, path, &canonical);
    if (status == XR_XIR_OK) status = watch_resource(xr_compile_resources_alloc(resources, uri->string_len + 1, (void **)&copy));
    if (status == XR_XIR_OK) status = watch_work(resources, uri->string_len + 1 + sizeof(*event));
    if (status == XR_XIR_OK) {
        memcpy(copy, uri->as.string, uri->string_len); copy[uri->string_len] = 0;
        *event = (XlspSourceWatchEvent){copy, path, canonical, type}; return XR_XIR_OK;
    }
    xr_compile_resources_free(copy); xr_compile_resources_free(canonical); xr_compile_resources_free(path); return status;
}
XR_FUNC XrXirStatus xlsp_source_watch_read(XrCompileResources *resources, const XrJsonValue *params,
    XlspSourceWatch **output) {
    if (!resources || !output || *output) return XR_XIR_BAD_STRUCTURE;
    const XrJsonValue *changes = NULL;
    XrXirStatus status = watch_member(resources, params, "changes", 7, &changes);
    if (status != XR_XIR_OK) return status;
    if (!changes || changes->type != XR_JSON_ARRAY || changes->as.array.count < 0 ||
        changes->as.array.capacity < changes->as.array.count ||
        (changes->as.array.count && !changes->as.array.items)) return XR_XIR_BAD_STRUCTURE;
    size_t count = (size_t)changes->as.array.count;
    if (count > SIZE_MAX / sizeof(XlspSourceWatchEvent)) return XR_XIR_BUDGET;
    XlspSourceWatch *owner = NULL;
    status = watch_resource(xr_compile_resources_calloc(resources, 1, sizeof(*owner), (void **)&owner));
    if (status != XR_XIR_OK) return status;
    owner->resources = resources; owner->count = count;
    if (count) status = watch_resource(xr_compile_resources_calloc(resources, count, sizeof(*owner->items), (void **)&owner->items));
    for (size_t i = 0; status == XR_XIR_OK && i < count; ++i) {
        status = watch_work(resources, sizeof(XrJsonValue *));
        if (status == XR_XIR_OK) status = watch_event(owner, changes->as.array.items[i], i);
    }
    if (status == XR_XIR_OK) { *output = owner; return XR_XIR_OK; }
    xlsp_source_watch_free(owner); return status;
}
XR_FUNC XrXirStatus xlsp_source_watch_commit(const XlspSourceWatch *owner, XrLspServer *server) {
    if (!owner || !server) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = watch_work(owner->resources, 1); if (status != XR_XIR_OK) return status;
    if ((uint64_t)owner->count > UINT64_MAX - server->source_revision) return XR_XIR_BUDGET;
    status = watch_work(owner->resources, sizeof(uint64_t) + (owner->count ? sizeof(void *) : 0));
    if (status != XR_XIR_OK) return status;
    server->source_revision += (uint64_t)owner->count;
    if (owner->count) {
        XlspSourceWorkspace *old = server->source_workspace; server->source_workspace = NULL;
        xlsp_source_workspace_free(old);
    }
    return XR_XIR_OK;
}
