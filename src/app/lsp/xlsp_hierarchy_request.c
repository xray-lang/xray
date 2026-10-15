/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_hierarchy_request.c - Ledger-owned hierarchy capture and cancellable run
 */
#include "xlsp_hierarchy_request.h"
#include "xlsp_source_params.h"
#include "xlsp_source_scope.h"
#include "xlsp_source_call_hierarchy.h"
#include "xlsp_source_type_hierarchy.h"
#include "xlsp_source_binding_lenses.h"
#include "../../base/xchecks.h"
#include <limits.h>
#include <string.h>

struct XlspHierarchyRequest {
    XrXirCompileContext context;
    XlspSourceDocument *documents;
    XlspSourceBuffer **buffers;
    size_t count, root_count;
    char *roots[MAX_WORKSPACE_FOLDERS];
    XlspSourceParams *params;
    const char *uri;
    size_t uri_length;
    XlspConfig config;
    uint64_t revision;
    unsigned mode;
    XlspSourceScopeQueryKind kind;
    bool started;
};
static unsigned hierarchy_stage(XlspSourceScopeQueryKind kind) {
    return kind == XLSP_SOURCE_BINDING_LENSES ? 12u : kind == XLSP_SOURCE_TYPE_HIERARCHY ? 25u : 24u;
}
static XrXirStatus hierarchy_resource(XrCompileResourceStatus status) {
    return status == XR_COMPILE_RESOURCE_OK ? XR_XIR_OK :
        status == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET :
        status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus hierarchy_length(XrCompileResources *resources, const char *text, size_t *out) {
    if (!text) return XR_XIR_BAD_STRUCTURE;
    for (size_t i = 0;; ++i) {
        XrXirStatus status = hierarchy_resource(xr_compile_resources_work(resources, 1));
        if (status != XR_XIR_OK) return status;
        if (!text[i]) { *out = i; return XR_XIR_OK; }
        if (i == SIZE_MAX - 1) return XR_XIR_BUDGET;
    }
}
static XrXirStatus hierarchy_string(XrCompileResources *resources, const char *text, char **out) {
    size_t length = 0;
    XrXirStatus status = hierarchy_length(resources, text, &length);
    if (status != XR_XIR_OK) return status;
    status = hierarchy_resource(xr_compile_resources_work(resources, length + 1));
    if (status != XR_XIR_OK) return status;
    status = hierarchy_resource(xr_compile_resources_alloc(resources, length + 1, (void **)out));
    if (status == XR_XIR_OK) memcpy(*out, text, length + 1);
    return status;
}
XR_FUNC void xlsp_hierarchy_request_free(XlspHierarchyRequest *request) {
    if (!request) return;
    XrCompileResources *resources = request->context.resources;
    for (size_t i = 0; (request->documents || request->buffers) && i < request->count; ++i) {
        if (request->documents) xr_compile_resources_free((char *)request->documents[i].uri);
        if (request->buffers) xlsp_source_buffer_free(request->buffers[i]);
    }
    for (size_t i = 0; i < request->root_count; ++i) xr_compile_resources_free(request->roots[i]);
    for (int i = 0; i < request->config.ignore_pattern_count; ++i)
        xr_compile_resources_free(request->config.ignore_patterns[i].pattern);
    xr_compile_resources_free(request->config.ignore_patterns);
    xr_compile_resources_free(request->config.log_path);
    xlsp_source_params_free(request->params);
    xr_compile_resources_free(request->documents); xr_compile_resources_free(request->buffers);
    xr_compile_resources_free(request);
    xr_compile_resources_release(resources);
}
static XrXirStatus hierarchy_config(XlspHierarchyRequest *request, const XlspConfig *config) {
    if (config->ignore_pattern_count < 0 || (config->ignore_pattern_count && !config->ignore_patterns))
        return XR_XIR_BAD_STRUCTURE;
    XrCompileResources *resources = request->context.resources;
    request->config = *config;
    request->config.ignore_patterns = NULL; request->config.log_path = NULL;
    request->config.ignore_pattern_count = 0; request->config.ignore_pattern_capacity = 0;
    size_t count = (size_t)config->ignore_pattern_count;
    if (count > SIZE_MAX / sizeof(*config->ignore_patterns)) return XR_XIR_BUDGET;
    XrXirStatus status = XR_XIR_OK;
    if (count) status = hierarchy_resource(xr_compile_resources_calloc(resources, count,
        sizeof(*config->ignore_patterns), (void **)&request->config.ignore_patterns));
    if (status != XR_XIR_OK) return status;
    request->config.ignore_pattern_capacity = config->ignore_pattern_count;
    for (size_t i = 0; i < count; ++i) {
        const XlspIgnorePattern *input = &config->ignore_patterns[i];
        XlspIgnorePattern *pattern = &request->config.ignore_patterns[i];
        status = hierarchy_string(resources, input->pattern, &pattern->pattern);
        if (status != XR_XIR_OK) return status;
        pattern->is_glob = input->is_glob; pattern->is_dir_only = input->is_dir_only;
        ++request->config.ignore_pattern_count;
    }
    if (config->log_path) status = hierarchy_string(resources, config->log_path, &request->config.log_path);
    return status;
}
static XrXirStatus hierarchy_document(XlspHierarchyRequest *request, size_t index,
    const XrLspDocument *doc) {
    XrCompileResources *resources = request->context.resources;
    if (!doc || !doc->content || doc->length == SIZE_MAX) return XR_XIR_BAD_STRUCTURE;
    size_t length = 0;
    XrXirStatus status = hierarchy_length(resources, doc->uri, &length);
    if (status != XR_XIR_OK) return status;
    char *path = NULL, *uri = NULL;
    status = xlsp_source_uri_path(resources, doc->uri, length, &path); xr_compile_resources_free(path);
    if (status != XR_XIR_OK) return status;
    status = xlsp_source_buffer_new(resources, doc->content, doc->length, (int)doc->version, &request->buffers[index]);
    if (status != XR_XIR_OK) return status;
    status = hierarchy_string(resources, doc->uri, &uri);
    if (status != XR_XIR_OK) return status;
    request->documents[index] = (XlspSourceDocument){uri, xlsp_source_buffer_text(request->buffers[index]),
        length, doc->length, doc->version};
    return XR_XIR_OK;
}
static XrXirStatus hierarchy_documents(XlspHierarchyRequest *request, const XrLspServer *server) {
    const XrLspDocTable *table = server->doc_table;
    if (!table) return XR_XIR_OK;
    if (table->doc_count < 0 || table->bucket_count <= 0 || !table->buckets) return XR_XIR_BAD_STRUCTURE;
    request->count = (size_t)table->doc_count;
    if (request->count > SIZE_MAX / sizeof(*request->documents) ||
        request->count > SIZE_MAX / sizeof(*request->buffers)) return XR_XIR_BUDGET;
    XrCompileResources *resources = request->context.resources;
    XrXirStatus status = XR_XIR_OK;
    if (request->count) status = hierarchy_resource(xr_compile_resources_calloc(resources, request->count,
        sizeof(*request->documents), (void **)&request->documents));
    if (status == XR_XIR_OK && request->count) status = hierarchy_resource(xr_compile_resources_calloc(resources,
        request->count, sizeof(*request->buffers), (void **)&request->buffers));
    if (status != XR_XIR_OK) return status;
    size_t used = 0;
    for (int i = 0; i < table->bucket_count; ++i) {
        status = hierarchy_resource(xr_compile_resources_work(resources, 1));
        if (status != XR_XIR_OK) return status;
        for (const XrLspDocBucket *bucket = table->buckets[i]; bucket; bucket = bucket->next) {
            if (used == request->count || !bucket->doc || bucket->doc->server != server) return XR_XIR_BAD_STRUCTURE;
            status = hierarchy_document(request, used, bucket->doc);
            if (status != XR_XIR_OK) return status;
            ++used;
        }
    }
    return used == request->count ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus hierarchy_member(XrCompileResources *resources, const XrJsonValue *object,
    const char *key, bool unique, const XrJsonValue **out) {
    if (!object || object->type != XR_JSON_OBJECT) return XR_XIR_BAD_STRUCTURE;
    size_t length = strlen(key);
    for (int i = 0; i < object->as.object.count; ++i) {
        XrXirStatus status = hierarchy_resource(xr_compile_resources_work(resources, sizeof(XrJsonMember)));
        if (status != XR_XIR_OK) return status;
        const XrJsonMember *member = &object->as.object.members[i];
        if (member->key_len != length) continue;
        status = hierarchy_resource(xr_compile_resources_work(resources, length));
        if (status != XR_XIR_OK) return status;
        if (!memcmp(member->key, key, length)) {
            if (*out) return XR_XIR_BAD_STRUCTURE;
            *out = member->value;
            if (!unique) return XR_XIR_OK;
        }
    }
    return *out ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus hierarchy_inputs(XlspHierarchyRequest *request, const XrLspServer *server,
    const XrJsonValue *params) {
    XrCompileResources *resources = request->context.resources;
    XrXirStatus status = hierarchy_resource(xlsp_source_params_new(resources, params, &request->params));
    if (status != XR_XIR_OK) return status;
    const XrJsonValue *owned = xlsp_source_params_view(request->params);
    const XrJsonValue *object = NULL, *uri = NULL;
    status = hierarchy_member(resources, owned, request->mode ? "item" : "textDocument", false, &object);
    if (status == XR_XIR_OK) status = hierarchy_member(resources, object, "uri", false, &uri);
    if (status != XR_XIR_OK) return status;
    if (uri->type != XR_JSON_STRING || !uri->as.string) return XR_XIR_BAD_STRUCTURE;
    char *path = NULL;
    status = xlsp_source_uri_path(resources, uri->as.string, uri->string_len, &path); xr_compile_resources_free(path);
    if (status != XR_XIR_OK) return status;
    request->uri = uri->as.string; request->uri_length = uri->string_len;
    status = hierarchy_config(request, &server->config);
    if (status != XR_XIR_OK) return status;
    status = hierarchy_documents(request, server);
    if (status != XR_XIR_OK) return status;
    request->root_count = (size_t)server->workspace_folder_count;
    for (size_t i = 0; i < request->root_count; ++i) {
        status = hierarchy_string(resources, server->workspace_folders[i].path, &request->roots[i]);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xlsp_hierarchy_request_capture(XrLspServer *server, const XrJsonValue *params,
    unsigned mode, XlspSourceScopeQueryKind kind, XlspHierarchyRequest **out) {
    if (!server || !out || *out || kind < XLSP_SOURCE_CALL_HIERARCHY ||
        kind > XLSP_SOURCE_BINDING_LENSES || mode > 2 || (kind == XLSP_SOURCE_BINDING_LENSES && mode))
        return XR_XIR_BAD_STRUCTURE;
    server->source_failure.stage = hierarchy_stage(kind); server->source_failure.status = XR_XIR_BAD_STRUCTURE;
    if (!params || server->shutdown_received || server->workspace_folder_count < 0 ||
        server->workspace_folder_count > MAX_WORKSPACE_FOLDERS) return XR_XIR_BAD_STRUCTURE;
    const XrCompileResourceLimits limits = {UINT64_C(64)*1024*1024, UINT64_C(8)*1024*1024, UINT64_C(128000000)};
    XrCompileResources *resources = NULL; XlspHierarchyRequest *request = NULL;
    XrXirStatus status = hierarchy_resource(xr_compile_resources_new(&limits, &resources));
    if (status != XR_XIR_OK) goto done;
    status = hierarchy_resource(xr_compile_resources_calloc(resources, 1, sizeof(*request), (void **)&request));
    if (status != XR_XIR_OK) goto done;
    request->context = (XrXirCompileContext){resources, xr_xir_compile_default_limits()};
    // Transfer the initial ledger lease with the owner, including failure cleanup.
    resources = NULL;
    request->mode = mode; request->kind = kind; request->revision = server->source_revision;
    status = hierarchy_inputs(request, server, params);
    if (status == XR_XIR_OK) { *out = request; request = NULL; }
done:
    xlsp_hierarchy_request_free(request); xr_compile_resources_release(resources);
    server->source_failure.status = (unsigned)status;
    if (status == XR_XIR_OK) server->source_failure.stage = 0;
    return status;
}
static bool hierarchy_cancelled(const _Atomic bool *cancel) {
    return cancel && atomic_load_explicit(cancel, memory_order_acquire);
}
static bool hierarchy_cancel_poll(const void *context) {
    return hierarchy_cancelled((const _Atomic bool *)context);
}
static XrXirStatus hierarchy_lenses(XlspHierarchyRequest *request, XlspSourceScope *scope,
    size_t entry, XrJsonValue **out) {
    const XrJsonValue *document = NULL, *uri = NULL;
    XrCompileResources *resources = request->context.resources;
    XrXirStatus status = hierarchy_member(resources, xlsp_source_params_view(request->params),
        "textDocument", true, &document);
    if (status == XR_XIR_OK) status = hierarchy_member(resources, document, "uri", true, &uri);
    if (status != XR_XIR_OK) return status;
    return xlsp_source_binding_lenses_json(&request->context, xlsp_source_scope_documents(scope),
        xlsp_source_scope_count(scope), entry, out);
}
XR_FUNC XrXirStatus xlsp_hierarchy_request_run(XlspHierarchyRequest *request,
    const _Atomic bool *cancel, XlspSourceRequestResult *out) {
    if (!request || request->started || !out || out->json || out->stage || out->cancelled ||
        out->status != XR_XIR_OK) return XR_XIR_BAD_STRUCTURE;
    request->started = true;
    XlspSourceRequestResult result = {.stage = hierarchy_stage(request->kind)};
    XlspSourceScope *scope = NULL; bool bound = false;
    if (hierarchy_cancelled(cancel)) { result.cancelled = true; goto done; }
    if (cancel) {
        result.status = hierarchy_resource(xr_compile_resources_begin_cancel(request->context.resources,
            hierarchy_cancel_poll, (const void *)cancel));
        if (result.status != XR_XIR_OK) goto done;
        bound = true;
    }
    const char *roots[MAX_WORKSPACE_FOLDERS];
    for (size_t i = 0; i < request->root_count; ++i) roots[i] = request->roots[i];
    XlspSourceScopeOptions options = {&request->context, request->documents, request->count, roots,
        request->root_count, UINT32_C(50)*1024*1024, &request->config};
    result.status = xlsp_source_scope_capture(&options, &scope);
    if (result.status != XR_XIR_OK) goto done;
    size_t entry = SIZE_MAX;
    result.status = xlsp_source_scope_find(scope, request->uri, request->uri_length, &entry);
    if (result.status != XR_XIR_OK) goto done;
    if (request->kind == XLSP_SOURCE_BINDING_LENSES) result.status = hierarchy_lenses(request, scope, entry, &result.json);
    else {
        XlspSourceHierarchyRequest input = {&request->context, xlsp_source_scope_documents(scope),
            xlsp_source_scope_known_versions(scope), xlsp_source_scope_count(scope), entry,
            xlsp_source_params_view(request->params), request->revision, request->mode};
        result.status = request->kind == XLSP_SOURCE_TYPE_HIERARCHY ? xlsp_source_type_hierarchy_json(&input, &result.json) :
            xlsp_source_call_hierarchy_json(&input, &result.json);
    }
    if (result.status == XR_XIR_OK) result.stage = 0;
done:
    xlsp_source_scope_free(scope);
    if (bound) {
        XrCompileResourceStatus ended = xr_compile_resources_end_cancel(request->context.resources,
            hierarchy_cancel_poll, (const void *)cancel);
        XR_CHECK(ended == XR_COMPILE_RESOURCE_OK || ended == XR_COMPILE_RESOURCE_CANCELLED,
            "Hierarchy cancellation must retain its binding");
        if (ended == XR_COMPILE_RESOURCE_CANCELLED) result.cancelled = true;
    }
    if (hierarchy_cancelled(cancel)) result.cancelled = true;
    if (result.cancelled) { xjson_free(result.json); result.json = NULL; }
    *out = result; return XR_XIR_OK;
}
XR_FUNC XrJsonValue *xlsp_source_scope_query(XrLspServer *server, const XrJsonValue *params,
    unsigned mode, XlspSourceScopeQueryKind kind) {
    if (!server) return NULL;
    uint64_t revision = server->source_revision;
    uint64_t generation = server->doc_table ? server->doc_table->generation : 0;
    XlspHierarchyRequest *request = NULL; XlspSourceRequestResult result = {0};
    XrXirStatus status = xlsp_hierarchy_request_capture(server, params, mode, kind, &request);
    if (status == XR_XIR_OK) status = xlsp_hierarchy_request_run(request, NULL, &result);
    if (status == XR_XIR_OK) status = result.status;
    xlsp_hierarchy_request_free(request);
    if (status == XR_XIR_OK && (server->shutdown_received || server->source_revision != revision ||
        (server->doc_table ? server->doc_table->generation : 0) != generation)) status = XR_XIR_UNRESOLVED;
    if (status != XR_XIR_OK) { xjson_free(result.json); result.json = NULL; }
    server->source_failure.status = (unsigned)status;
    server->source_failure.stage = status == XR_XIR_OK ? 0u : hierarchy_stage(kind);
    return result.json;
}
