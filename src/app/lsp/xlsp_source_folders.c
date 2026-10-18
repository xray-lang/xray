/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_folders.c - Atomic workspace-root ownership and invalidation
 *
 * KEY CONCEPT:
 *   Late invalid input cannot remove an earlier root. Retiring the entire old
 *   index pool prevents stale results from resubmitting removed directories.
 */
#include "xlsp_source_folders.h"
#include "xlsp_index_pool.h"
#include "xlsp_source_index_store.h"
#include "xlsp_source_workspace.h"
#include "../../shared/xr_utf8_core.h"
#include "../../base/xmalloc.h"
#include <string.h>

struct XlspSourceFolders {
    XrCompileResources *resources;
    uint64_t revision;
    XlspWorkspaceFolder base[MAX_WORKSPACE_FOLDERS];
    XlspWorkspaceFolder *added, *removed;
    const XlspWorkspaceFolder *next[MAX_WORKSPACE_FOLDERS];
    size_t base_count, added_count, removed_count, count;
    bool changed;
};
static XrXirStatus folders_status(XrCompileResourceStatus s) {
    return s == XR_COMPILE_RESOURCE_OK ? XR_XIR_OK : s == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET :
        s == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus folders_work(XrCompileResources *r, uint64_t units) {
    return folders_status(xr_compile_resources_work(r, units));
}
static int folders_byte(void *context, const uint8_t *address, uint8_t *out) {
    if (folders_work(context, 1) != XR_XIR_OK) return 0;
    *out = *address; return 1;
}
static XrXirStatus folders_text(XrCompileResources *r, const char *text, size_t length) {
    if (!text || length == SIZE_MAX) return XR_XIR_BAD_STRUCTURE;
    for (size_t at = 0; at < length;) {
        XrUtf8Step step;
        if (!xr_utf8_core_decode_step_read((const uint8_t *)text + at, length - at,
                folders_byte, r, &step)) return XR_XIR_BUDGET;
        if (step.error != XR_UTF8_OK || !step.scalar) return XR_XIR_BAD_STRUCTURE;
        at += step.consumed;
    }
    return XR_XIR_OK;
}
static XrXirStatus folders_member(XrCompileResources *r, const XrJsonValue *object,
    const char *key, const XrJsonValue **out) {
    if (!object || object->type != XR_JSON_OBJECT || object->as.object.count < 0 ||
        object->as.object.capacity < object->as.object.count ||
        (object->as.object.count && !object->as.object.members)) return XR_XIR_BAD_STRUCTURE;
    size_t length = strlen(key); const XrJsonValue *found = NULL;
    for (int i = 0; i < object->as.object.count; ++i) {
        XrXirStatus s = folders_work(r, sizeof(XrJsonMember)); if (s != XR_XIR_OK) return s;
        const XrJsonMember *m = &object->as.object.members[i]; if (!m->value) return XR_XIR_BAD_STRUCTURE;
        s = folders_text(r, m->key, m->key_len); if (s != XR_XIR_OK) return s;
        s = folders_work(r, length + 1); if (s != XR_XIR_OK) return s;
        if (m->key_len == length && !memcmp(m->key, key, length)) {
            if (found) return XR_XIR_BAD_STRUCTURE;
            found = m->value;
        }
    }
    *out = found; return XR_XIR_OK;
}
static XrXirStatus folders_length(XrCompileResources *r, const char *text, size_t *out) {
    if (!text) return XR_XIR_BAD_STRUCTURE;
    for (size_t i = 0;; ++i) {
        XrXirStatus s = folders_work(r, 1); if (s != XR_XIR_OK) return s;
        if (!text[i]) { *out = i; return XR_XIR_OK; }
        if (i == SIZE_MAX - 1) return XR_XIR_BUDGET;
    }
}
static XrXirStatus folders_copy(XrCompileResources *r, const char *text, size_t length, char **out) {
    if (length == SIZE_MAX) return XR_XIR_BUDGET;
    XrXirStatus s = folders_work(r, length + 1); if (s != XR_XIR_OK) return s;
    char *copy = NULL; s = folders_status(xr_compile_resources_alloc(r, length + 1, (void **)&copy));
    if (s == XR_XIR_OK) { memcpy(copy, text, length); copy[length] = 0; *out = copy; }
    return s;
}
static XrXirStatus folders_equal(XrCompileResources *r, const char *a, const char *b, bool *out) {
    if (!a || !b) return XR_XIR_BAD_STRUCTURE;
    for (size_t i = 0;; ++i) {
        XrXirStatus s = folders_work(r, 2); if (s != XR_XIR_OK) return s;
        if (a[i] != b[i]) { *out = false; return XR_XIR_OK; }
        if (!a[i]) { *out = true; return XR_XIR_OK; }
        if (i == SIZE_MAX - 1) return XR_XIR_BUDGET;
    }
}
static void folders_release(XlspWorkspaceFolder *folder, bool receiving) {
    if (receiving) { xr_free(folder->uri); xr_free(folder->name); xr_free(folder->path); }
    else { xr_compile_resources_free(folder->uri); xr_compile_resources_free(folder->name); xr_compile_resources_free(folder->path); }
}
XR_FUNC void xlsp_source_folders_free(XlspSourceFolders *owner) {
    if (!owner) return;
    for (size_t i = 0; i < owner->base_count; ++i) folders_release(&owner->base[i], false);
    if (owner->added) for (size_t i = 0; i < owner->added_count; ++i) folders_release(&owner->added[i], false);
    if (owner->removed) for (size_t i = 0; i < owner->removed_count; ++i) folders_release(&owner->removed[i], false);
    xr_compile_resources_free(owner->added); xr_compile_resources_free(owner->removed); xr_compile_resources_free(owner);
}
static XrXirStatus folders_capture(XrCompileResources *r, const XrLspServer *server, XlspSourceFolders **out) {
    if (!r || !server || !out || *out || server->workspace_folder_count < 0 ||
        server->workspace_folder_count > MAX_WORKSPACE_FOLDERS) return XR_XIR_BAD_STRUCTURE;
    XlspSourceFolders *owner = NULL;
    XrXirStatus s = folders_status(xr_compile_resources_calloc(r, 1, sizeof(*owner), (void **)&owner));
    if (s != XR_XIR_OK) return s;
    owner->resources = r; owner->revision = server->source_revision;
    owner->count = owner->base_count = (size_t)server->workspace_folder_count;
    for (size_t i = 0; i < owner->base_count && s == XR_XIR_OK; ++i) {
        const XlspWorkspaceFolder *src = &server->workspace_folders[i]; XlspWorkspaceFolder *dst = &owner->base[i];
        s = folders_work(r, sizeof(*src)); if (s != XR_XIR_OK) break;
        dst->config_loaded = src->config_loaded; dst->index_requested = src->index_requested; dst->index_completed = src->index_completed;
        const char *strings[] = {src->uri, src->name, src->path}; char **copies[] = {&dst->uri, &dst->name, &dst->path};
        for (size_t j = 0; j < 3 && s == XR_XIR_OK; ++j) {
            size_t length = 0; s = folders_length(r, strings[j], &length);
            if (s == XR_XIR_OK) s = folders_copy(r, strings[j], length, copies[j]);
        }
        owner->next[i] = dst;
    }
    if (s == XR_XIR_OK) *out = owner; else xlsp_source_folders_free(owner);
    return s;
}
static XrXirStatus folders_decode(XrCompileResources *r, const XrJsonValue *value, XlspWorkspaceFolder *folder) {
    const XrJsonValue *uri = NULL, *name = NULL; XrXirStatus s = folders_member(r, value, "uri", &uri);
    if (s == XR_XIR_OK) s = folders_member(r, value, "name", &name);
    if (s != XR_XIR_OK) return s;
    if (!uri || uri->type != XR_JSON_STRING || !name || name->type != XR_JSON_STRING) return XR_XIR_BAD_STRUCTURE;
    s = folders_text(r, name->as.string, name->string_len);
    if (s == XR_XIR_OK) s = xlsp_source_uri_path(r, uri->as.string, uri->string_len, &folder->path);
    if (s == XR_XIR_OK) s = folders_copy(r, uri->as.string, uri->string_len, &folder->uri);
    if (s == XR_XIR_OK) s = folders_copy(r, name->as.string, name->string_len, &folder->name);
    folder->index_requested = true; return s;
}
static XrXirStatus folders_array(XlspSourceFolders *owner, const XrJsonValue *array, bool add) {
    if (!array || array->type != XR_JSON_ARRAY || array->as.array.count < 0 ||
        array->as.array.capacity < array->as.array.count ||
        (array->as.array.count && !array->as.array.items)) return XR_XIR_BAD_STRUCTURE;
    size_t count = (size_t)array->as.array.count;
    if (count > SIZE_MAX / sizeof(XlspWorkspaceFolder)) return XR_XIR_BUDGET;
    XlspWorkspaceFolder **rows = add ? &owner->added : &owner->removed;
    if (add) owner->added_count = count; else owner->removed_count = count;
    XrXirStatus s = XR_XIR_OK;
    if (count) s = folders_status(xr_compile_resources_calloc(owner->resources, count, sizeof(**rows), (void **)rows));
    for (size_t i = 0; i < count && s == XR_XIR_OK; ++i) {
        s = folders_work(owner->resources, sizeof(XrJsonValue *));
        if (s == XR_XIR_OK) s = folders_decode(owner->resources, array->as.array.items[i], &(*rows)[i]);
        for (size_t j = 0; j < i && s == XR_XIR_OK; ++j) {
            bool same = false; s = folders_equal(owner->resources, (*rows)[i].uri, (*rows)[j].uri, &same);
            if (s == XR_XIR_OK && same) s = XR_XIR_BAD_STRUCTURE;
        }
    }
    return s;
}
static XrXirStatus folders_plan(XlspSourceFolders *owner) {
    for (size_t i = 0; i < owner->removed_count; ++i) for (size_t j = 0; j < owner->count; ++j) {
        bool same = false; XrXirStatus s = folders_equal(owner->resources, owner->removed[i].uri, owner->next[j]->uri, &same);
        if (s != XR_XIR_OK) return s;
        if (!same) continue;
        s = folders_work(owner->resources, (owner->count - j - 1) * sizeof(*owner->next)); if (s != XR_XIR_OK) return s;
        memmove(owner->next + j, owner->next + j + 1, (owner->count - j - 1) * sizeof(*owner->next));
        --owner->count; owner->changed = true; break;
    }
    for (size_t i = 0; i < owner->added_count; ++i) {
        for (size_t j = 0; j < owner->count; ++j) {
            bool same = false; XrXirStatus s = folders_equal(owner->resources, owner->added[i].uri, owner->next[j]->uri, &same);
            if (s != XR_XIR_OK) return s;
            if (same) return XR_XIR_BAD_STRUCTURE;
        }
        if (owner->count == MAX_WORKSPACE_FOLDERS) return XR_XIR_BUDGET;
        XrXirStatus s = folders_work(owner->resources, sizeof(*owner->next)); if (s != XR_XIR_OK) return s;
        owner->next[owner->count++] = &owner->added[i]; owner->changed = true;
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xlsp_source_folders_read(XrCompileResources *r, const XrLspServer *server,
    const XrJsonValue *params, XlspSourceFolders **out) {
    if (!out || *out) return XR_XIR_BAD_STRUCTURE;
    XlspSourceFolders *owner = NULL; XrXirStatus s = folders_capture(r, server, &owner);
    const XrJsonValue *event = NULL, *removed = NULL, *added = NULL;
    if (s == XR_XIR_OK) s = folders_member(r, params, "event", &event);
    if (s == XR_XIR_OK) s = folders_member(r, event, "removed", &removed);
    if (s == XR_XIR_OK) s = folders_member(r, event, "added", &added);
    if (s == XR_XIR_OK) s = folders_array(owner, removed, false);
    if (s == XR_XIR_OK) s = folders_array(owner, added, true);
    if (s == XR_XIR_OK) s = folders_plan(owner);
    if (s == XR_XIR_OK) *out = owner; else xlsp_source_folders_free(owner);
    return s;
}
XR_FUNC XrXirStatus xlsp_source_folders_single(XrCompileResources *r, const XrLspServer *server,
    const char *uri, const char *name, bool add, XlspSourceFolders **out) {
    if (!out || *out || !uri || !name) return XR_XIR_BAD_STRUCTURE;
    XlspSourceFolders *owner = NULL; XrXirStatus s = folders_capture(r, server, &owner);
    XlspWorkspaceFolder **row = owner ? (add ? &owner->added : &owner->removed) : NULL;
    if (s == XR_XIR_OK) {
        if (add) owner->added_count = 1; else owner->removed_count = 1;
        s = folders_status(xr_compile_resources_calloc(r, 1, sizeof(**row), (void **)row));
    }
    size_t uri_length = 0, name_length = 0;
    if (s == XR_XIR_OK) s = folders_length(r, uri, &uri_length);
    if (s == XR_XIR_OK) s = folders_length(r, name, &name_length);
    if (s == XR_XIR_OK) s = folders_text(r, name, name_length);
    if (s == XR_XIR_OK) s = xlsp_source_uri_path(r, uri, uri_length, &(*row)->path);
    if (s == XR_XIR_OK) s = folders_copy(r, uri, uri_length, &(*row)->uri);
    if (s == XR_XIR_OK) s = folders_copy(r, name, name_length, &(*row)->name);
    if (s == XR_XIR_OK) { (*row)->index_requested = true; s = folders_plan(owner); }
    if (s == XR_XIR_OK) *out = owner; else xlsp_source_folders_free(owner);
    return s;
}
static XrXirStatus folders_current(const XlspSourceFolders *owner, const XrLspServer *server) {
    if (server->workspace_folder_count < 0 || (size_t)server->workspace_folder_count != owner->base_count ||
        server->source_revision != owner->revision) return XR_XIR_UNRESOLVED;
    for (size_t i = 0; i < owner->base_count; ++i) {
        const XlspWorkspaceFolder *a = &owner->base[i], *b = &server->workspace_folders[i];
        XrXirStatus s = folders_work(owner->resources, sizeof(*b)); if (s != XR_XIR_OK) return s;
        if (a->config_loaded != b->config_loaded || a->index_requested != b->index_requested ||
            a->index_completed != b->index_completed) return XR_XIR_UNRESOLVED;
        const char *left[] = {a->uri, a->name, a->path}, *right[] = {b->uri, b->name, b->path};
        for (size_t j = 0; j < 3; ++j) {
            bool same = false; s = folders_equal(owner->resources, left[j], right[j], &same);
            if (s != XR_XIR_OK) return s;
            if (!same) return XR_XIR_UNRESOLVED;
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus folders_receive(XrCompileResources *r, const XlspWorkspaceFolder *src, XlspWorkspaceFolder *dst) {
    const char *strings[] = {src->uri, src->name, src->path}; char **copies[] = {&dst->uri, &dst->name, &dst->path};
    for (size_t i = 0; i < 3; ++i) {
        size_t length = 0; XrXirStatus s = folders_length(r, strings[i], &length);
        if (s == XR_XIR_OK) s = folders_work(r, length + 1); if (s != XR_XIR_OK) return s;
        *copies[i] = xr_malloc(length + 1); if (!*copies[i]) return XR_XIR_OUT_OF_MEMORY;
        memcpy(*copies[i], strings[i], length + 1);
    }
    dst->config_loaded = src->config_loaded; dst->index_requested = true; dst->index_completed = false;
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xlsp_source_folders_commit(const XlspSourceFolders *owner, XrLspServer *server, bool *changed) {
    if (!owner || !server || !changed) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus s = folders_current(owner, server); if (s != XR_XIR_OK) return s;
    if (!owner->changed) { *changed = false; return XR_XIR_OK; }
    if (server->source_revision == UINT64_MAX) return XR_XIR_BUDGET;
    if (!server->index_resources || !server->source_index) return XR_XIR_BAD_STRUCTURE;
    XlspWorkspaceFolder incoming[MAX_WORKSPACE_FOLDERS] = {0}; XlspSourceIndexStore *index = NULL;
    for (size_t i = 0; i < owner->count && s == XR_XIR_OK; ++i) s = folders_receive(owner->resources, owner->next[i], &incoming[i]);
    if (s == XR_XIR_OK) s = xlsp_source_index_store_new(server->index_resources, &index);
    if (s == XR_XIR_OK) s = folders_work(owner->resources, sizeof(incoming) * 2 + sizeof(uint64_t) + 8 * sizeof(void *));
    if (s != XR_XIR_OK) {
        for (size_t i = 0; i < owner->count; ++i) folders_release(&incoming[i], true);
        xlsp_source_index_store_free(index); return s;
    }
    XlspWorkspaceFolder retired[MAX_WORKSPACE_FOLDERS]; memcpy(retired, server->workspace_folders, sizeof(retired));
    XrLspIndexPool *pool = server->index_pool; XlspSourceIndexStore *old_index = server->source_index;
    XlspSourceWorkspace *workspace = server->source_workspace;
    memcpy(server->workspace_folders, incoming, sizeof(incoming)); server->workspace_folder_count = (int)owner->count;
    ++server->source_revision; server->source_index = index; server->source_workspace = NULL; server->index_pool = NULL;
    server->indexing_in_progress = false; server->index_cancelled = true; server->files_indexed = server->files_total = 0;
    xlsp_index_pool_free(pool); xlsp_source_index_store_free(old_index); xlsp_source_workspace_free(workspace);
    for (size_t i = 0; i < owner->base_count; ++i) folders_release(&retired[i], true);
    *changed = true; return XR_XIR_OK;
}
