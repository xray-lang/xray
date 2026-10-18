/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_initialize.c - One initialize response and workspace transaction
 *
 * KEY CONCEPT:
 *   Explicit workspace folders win, including an empty/null set. A selected
 *   rootUri determines both URI and path; rootPath-only input is encoded by
 *   the same owned Source path boundary used for later folder notifications.
 */
#include "xlsp_source_initialize.h"
#include "xlsp_initialize_result.h"
#include "xlsp_index_pool.h"
#include "../../shared/xr_utf8_core.h"
#include <string.h>

struct XlspSourceInitialize {
    XrCompileResources *resources, *index_resources;
    struct XlspSourceIndexStore *index;
    XlspSourceFolders *folders;
    XrJsonValue *result;
    unsigned capabilities;
    XlspInitializeOptions options;
};
static XrXirStatus initialize_resource(XrCompileResourceStatus s) {
    return s == XR_COMPILE_RESOURCE_OK ? XR_XIR_OK : s == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET :
        s == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus initialize_work(XrCompileResources *r, uint64_t units) {
    return initialize_resource(xr_compile_resources_work(r, units));
}
static int initialize_byte(void *context, const uint8_t *address, uint8_t *out) {
    if (initialize_work(context, 1) != XR_XIR_OK) return 0;
    *out = *address; return 1;
}
static XrXirStatus initialize_text(XrCompileResources *r, const char *text, size_t length) {
    if (!text || length == SIZE_MAX) return XR_XIR_BAD_STRUCTURE;
    for (size_t at = 0; at < length;) {
        XrUtf8Step step;
        if (!xr_utf8_core_decode_step_read((const uint8_t *)text + at, length - at,
                initialize_byte, r, &step)) return XR_XIR_BUDGET;
        if (step.error != XR_UTF8_OK || !step.scalar) return XR_XIR_BAD_STRUCTURE;
        at += step.consumed;
    }
    return XR_XIR_OK;
}
static XrXirStatus initialize_member(XrCompileResources *r, const XrJsonValue *object,
    const char *key, const XrJsonValue **out) {
    if (!object || object->type != XR_JSON_OBJECT || object->as.object.count < 0 ||
        object->as.object.capacity < object->as.object.count ||
        (object->as.object.count && !object->as.object.members)) return XR_XIR_BAD_STRUCTURE;
    const XrJsonValue *found = NULL; size_t length = strlen(key);
    for (int i = 0; i < object->as.object.count; ++i) {
        XrXirStatus s = initialize_work(r, sizeof(XrJsonMember)); if (s != XR_XIR_OK) return s;
        const XrJsonMember *m = &object->as.object.members[i]; if (!m->value) return XR_XIR_BAD_STRUCTURE;
        s = initialize_text(r, m->key, m->key_len); if (s != XR_XIR_OK) return s;
        s = initialize_work(r, length + 1); if (s != XR_XIR_OK) return s;
        if (m->key_len == length && !memcmp(m->key, key, length)) {
            if (found) return XR_XIR_BAD_STRUCTURE;
            found = m->value;
        }
    }
    *out = found; return XR_XIR_OK;
}
static unsigned initialize_capability_bits(const XrLspServer *server) {
    return (unsigned)server->capabilities.completion | ((unsigned)server->capabilities.hover << 1) |
        ((unsigned)server->capabilities.definition << 2) | ((unsigned)server->capabilities.references << 3) |
        ((unsigned)server->capabilities.document_symbol << 4) | ((unsigned)server->capabilities.formatting << 5) |
        ((unsigned)server->capabilities.rename << 6);
}
static XrXirStatus initialize_client(XrCompileResources *r, const XrJsonValue *params, XlspInitializeOptions *options) {
    const XrJsonValue *cap = NULL, *text = NULL, *rename = NULL, *prepare = NULL, *action = NULL, *literal = NULL, *kind = NULL, *values = NULL;
    XrXirStatus s = initialize_member(r, params, "capabilities", &cap);
    if (s != XR_XIR_OK) return s;
    if (!cap || cap->type != XR_JSON_OBJECT) return XR_XIR_BAD_STRUCTURE;
    s = initialize_member(r, cap, "textDocument", &text); if (s != XR_XIR_OK || !text) return s;
    s = initialize_member(r, text, "rename", &rename);
    if (s == XR_XIR_OK && rename) s = initialize_member(r, rename, "prepareSupport", &prepare);
    if (s != XR_XIR_OK) return s;
    if (prepare) { if (prepare->type != XR_JSON_BOOL) return XR_XIR_BAD_STRUCTURE; options->prepare_rename = prepare->as.boolean; }
    s = initialize_member(r, text, "codeAction", &action);
    if (s == XR_XIR_OK && action) s = initialize_member(r, action, "codeActionLiteralSupport", &literal);
    if (s != XR_XIR_OK || !literal) return s;
    s = initialize_member(r, literal, "codeActionKind", &kind);
    if (s == XR_XIR_OK) s = initialize_member(r, kind, "valueSet", &values);
    if (s != XR_XIR_OK) return s;
    if (!values || values->type != XR_JSON_ARRAY || values->as.array.count < 0 ||
        values->as.array.capacity < values->as.array.count ||
        (values->as.array.count && !values->as.array.items)) return XR_XIR_BAD_STRUCTURE;
    for (int i = 0; i < values->as.array.count; ++i) {
        s = initialize_work(r, sizeof(XrJsonValue *)); if (s != XR_XIR_OK) return s;
        const XrJsonValue *value = values->as.array.items[i];
        if (!value || value->type != XR_JSON_STRING) return XR_XIR_BAD_STRUCTURE;
        s = initialize_text(r, value->as.string, value->string_len); if (s != XR_XIR_OK) return s;
    }
    options->code_action_literals = true; return XR_XIR_OK;
}
static bool initialize_unstarted(const XrLspServer *server) {
    return !server->initialized && !server->shutdown_received && !server->exit_received &&
        !server->workspace_folder_count && !server->index_pool && !server->source_workspace;
}
/* These typed stack views borrow only during the synchronous decoder call;
 * the returned transaction copies every selected client byte. */
static XrXirStatus initialize_folder_array(XrCompileResources *r, const XrLspServer *server,
    const XrJsonValue *folders, XlspSourceFolders **out) {
    XrJsonValue added = *folders, removed = {.type = XR_JSON_ARRAY};
    XrJsonMember events[] = {{"removed", 7, &removed}, {"added", 5, &added}};
    XrJsonValue event = {.type = XR_JSON_OBJECT}; event.as.object.members = events; event.as.object.count = event.as.object.capacity = 2;
    XrJsonMember params[] = {{"event", 5, &event}};
    XrJsonValue root = {.type = XR_JSON_OBJECT}; root.as.object.members = params; root.as.object.count = root.as.object.capacity = 1;
    return xlsp_source_folders_read(r, server, &root, out);
}
static XrXirStatus initialize_root(XrCompileResources *r, const XrLspServer *server,
    const XrJsonValue *uri, XlspSourceFolders **out) {
    XrJsonValue selected = *uri, name = {.type = XR_JSON_STRING, .string_len = 4}; name.as.string = "root";
    XrJsonMember fields[] = {{"uri", 3, &selected}, {"name", 4, &name}};
    XrJsonValue folder = {.type = XR_JSON_OBJECT}; folder.as.object.members = fields; folder.as.object.count = folder.as.object.capacity = 2;
    XrJsonValue *items[] = {&folder}; XrJsonValue array = {.type = XR_JSON_ARRAY};
    array.as.array.items = items; array.as.array.count = array.as.array.capacity = 1;
    return initialize_folder_array(r, server, &array, out);
}
static XrXirStatus initialize_path(XrCompileResources *r, const XrLspServer *server,
    const XrJsonValue *path, XlspSourceFolders **out) {
    if (path->string_len == SIZE_MAX) return XR_XIR_BUDGET;
    char *copy = NULL, *encoded = NULL;
    XrXirStatus s = initialize_resource(xr_compile_resources_alloc(r, path->string_len + 1, (void **)&copy));
    if (s == XR_XIR_OK) s = initialize_work(r, path->string_len + 1);
    if (s == XR_XIR_OK) { memcpy(copy, path->as.string, path->string_len); copy[path->string_len] = 0; s = xlsp_index_file_uri(r, copy, &encoded); }
    if (s == XR_XIR_OK) {
        size_t length = 0;
        do { s = initialize_work(r, 1); if (s != XR_XIR_OK) break; } while (encoded[length++]);
        if (s == XR_XIR_OK) { XrJsonValue uri = {.type = XR_JSON_STRING, .string_len = length - 1}; uri.as.string = encoded; s = initialize_root(r, server, &uri, out); }
    }
    xr_compile_resources_free(encoded); xr_compile_resources_free(copy); return s;
}
static XrXirStatus initialize_folders(XrCompileResources *r, const XrLspServer *server,
    const XrJsonValue *params, XlspSourceFolders **out) {
    const XrJsonValue *folders = NULL, *uri = NULL, *path = NULL;
    XrXirStatus s = initialize_member(r, params, "workspaceFolders", &folders);
    if (s == XR_XIR_OK) s = initialize_member(r, params, "rootUri", &uri);
    if (s == XR_XIR_OK) s = initialize_member(r, params, "rootPath", &path);
    const XrJsonValue *strings[] = {uri, path};
    for (size_t i = 0; i < 2 && s == XR_XIR_OK; ++i) if (strings[i] && strings[i]->type != XR_JSON_NULL) {
        if (strings[i]->type != XR_JSON_STRING) s = XR_XIR_BAD_STRUCTURE;
        else s = initialize_text(r, strings[i]->as.string, strings[i]->string_len);
    }
    if (s != XR_XIR_OK) return s;
    XrJsonValue empty = {.type = XR_JSON_ARRAY};
    if (folders) return initialize_folder_array(r, server, folders->type == XR_JSON_NULL ? &empty : folders, out);
    if (uri) return uri->type == XR_JSON_NULL ? initialize_folder_array(r, server, &empty, out) : initialize_root(r, server, uri, out);
    if (path && path->type != XR_JSON_NULL) return initialize_path(r, server, path, out);
    return initialize_folder_array(r, server, &empty, out);
}
/* Response shape and depth are bounded by the server's fixed capability
 * schema. Charge the actual produced projection before any state publication. */
static XrXirStatus initialize_projection(XrCompileResources *r, const XrJsonValue *value) {
    XrXirStatus s = initialize_work(r, sizeof(*value)); if (s != XR_XIR_OK) return s;
    if (value->type == XR_JSON_STRING) return initialize_work(r, value->string_len + 1);
    if (value->type == XR_JSON_ARRAY) for (int i = 0; i < value->as.array.count && s == XR_XIR_OK; ++i)
        s = initialize_projection(r, value->as.array.items[i]);
    if (value->type == XR_JSON_OBJECT) for (int i = 0; i < value->as.object.count && s == XR_XIR_OK; ++i) {
        const XrJsonMember *member = &value->as.object.members[i];
        s = initialize_work(r, sizeof(*member) + member->key_len + 1);
        if (s == XR_XIR_OK) s = initialize_projection(r, member->value);
    }
    return s;
}
XR_FUNC void xlsp_source_initialize_free(XlspSourceInitialize *owner) {
    if (!owner) return;
    xjson_free(owner->result); xlsp_source_folders_free(owner->folders); xr_compile_resources_free(owner);
}
XR_FUNC XrXirStatus xlsp_source_initialize_prepare(XrCompileResources *r, const XrLspServer *server,
    const XrJsonValue *params, XlspSourceInitialize **out) {
    if (!r || !server || !out || *out || !server->source_index || !server->index_resources) return XR_XIR_BAD_STRUCTURE;
    if (!initialize_unstarted(server)) return XR_XIR_BAD_STAGE;
    XlspSourceInitialize *owner = NULL;
    XrXirStatus s = initialize_resource(xr_compile_resources_calloc(r, 1, sizeof(*owner), (void **)&owner));
    if (s != XR_XIR_OK) return s;
    owner->resources = r; owner->index = server->source_index; owner->index_resources = server->index_resources;
    owner->capabilities = initialize_capability_bits(server);
    s = initialize_client(r, params, &owner->options);
    if (s == XR_XIR_OK) s = initialize_folders(r, server, params, &owner->folders);
    if (s == XR_XIR_OK) { owner->result = xlsp_initialize_result(server, &owner->options); if (!owner->result) s = XR_XIR_OUT_OF_MEMORY; }
    if (s == XR_XIR_OK) s = initialize_projection(r, owner->result);
    if (s == XR_XIR_OK) *out = owner; else xlsp_source_initialize_free(owner);
    return s;
}
XR_FUNC XrXirStatus xlsp_source_initialize_commit(XlspSourceInitialize *owner, XrLspServer *server, XrJsonValue **out) {
    if (!owner || !server || !out || *out) return XR_XIR_BAD_STRUCTURE;
    if (!owner->result) return XR_XIR_BAD_STAGE;
    if (!initialize_unstarted(server) || owner->capabilities != initialize_capability_bits(server) ||
        owner->index != server->source_index || owner->index_resources != server->index_resources) return XR_XIR_UNRESOLVED;
    XrXirStatus s = initialize_work(owner->resources, sizeof(bool) + 2 * sizeof(void *)); bool changed = false;
    if (s == XR_XIR_OK) s = xlsp_source_folders_commit(owner->folders, server, &changed);
    if (s != XR_XIR_OK) return s;
    server->initialized = true; *out = owner->result; owner->result = NULL; return XR_XIR_OK;
}
