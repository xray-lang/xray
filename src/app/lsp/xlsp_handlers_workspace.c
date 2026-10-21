/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_handlers_workspace.c - LSP workspace handlers
 *   didChangeWatchedFiles, didChangeWorkspaceFolders, didChangeConfiguration
 */

#include "../../base/xresource_limits.h"
#include "xlsp_handlers_workspace.h"
#include "xlsp_server.h"
#include "../../base/xjson.h"
#include "xlsp_workspace.h"
#include "xlsp_analysis.h"
#include "../../frontend/analyzer/xanalyzer.h"
#include "xlsp_cache.h"
#include "xlsp_imports.h"
#include "xlsp_utils.h"
#include "xlsp_source_index_store.h"
#include "xlsp_source_settings.h"
#include "xlsp_source_watch.h"
#include "xlsp_source_folders.h"
#include "xlsp_source_workspace.h"
#include "xlsp_document_store.h"
#include "../../api/xsource_compile.h"
#include "xlsp_index_pool.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"

// File change types (LSP spec)
#define FILE_CHANGE_CREATED 1
#define FILE_CHANGE_CHANGED 2
#define FILE_CHANGE_DELETED 3

XR_FUNC void xlsp_handle_ws_did_change_watched_files(XrLspServer *server, XrJsonValue *params) {
    if (!server) return;
    XrCompileResourceLimits limits = (XrCompileResourceLimits)XR_RESOURCE_SANDBOX_COMPILER_LIMITS;
    XrCompileResources *resources = NULL; XlspSourceWatch *batch = NULL;
    XrCompileResourceStatus opened = xr_compile_resources_new(&limits, &resources);
    XrXirStatus status = opened == XR_COMPILE_RESOURCE_OK ? XR_XIR_OK :
        opened == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
    if (status == XR_XIR_OK) status = xlsp_source_watch_read(resources, params, &batch);
    if (status == XR_XIR_OK) status = xlsp_source_watch_commit(batch, server);
    if (status != XR_XIR_OK) {
        server->source_failure.stage = 14; server->source_failure.status = (unsigned)status;
        lsp_log("File change notification rejected (status=%u)", (unsigned)status); goto done;
    }
    server->source_failure.stage = server->source_failure.status = 0;
    size_t count = 0; const XlspSourceWatchEvent *events = xlsp_source_watch_items(batch, &count);
    for (size_t i = 0; i < count; ++i) {
        const XlspSourceWatchEvent *event = &events[i];
        XrLspDocument *doc = xlsp_document_store_get(server->doc_table, event->uri);
        xlsp_index_pool_cancel_file(server->index_pool, event->path);
        XrXirStatus removed = xlsp_source_index_store_remove(server->source_index, event->index_uri);
        if (removed != XR_XIR_OK) xlsp_index_record_failure(server, removed);
        xlsp_exports_cache_remove(server, event->path);
        if (event->kind == FILE_CHANGE_CREATED || event->kind == FILE_CHANGE_CHANGED) {
            if (!doc) xlsp_workspace_index_file(server, event->uri, event->path);
            else {
                doc->dirty = true; xlsp_parse_document(doc, server); xlsp_publish_diagnostics(server, doc);
            }
        } else if (event->kind == FILE_CHANGE_DELETED && server->workspace_analyzer) {
            xa_analyzer_remove_file(server->workspace_analyzer, event->path);
        }
    }
done:
    xlsp_source_watch_free(batch); xr_compile_resources_release(resources);
}

// ============================================================================
// Workspace Folders
// ============================================================================

static void workspace_folders_apply(XrLspServer *server, const XrJsonValue *params,
    const char *uri, const char *name, bool add) {
    if (!server) return;
    XrCompileResourceLimits limits = (XrCompileResourceLimits)XR_RESOURCE_SANDBOX_COMPILER_LIMITS;
    XrCompileResources *resources = NULL; XlspSourceFolders *owner = NULL; bool changed = false;
    XrCompileResourceStatus opened = xr_compile_resources_new(&limits, &resources);
    XrXirStatus status = opened == XR_COMPILE_RESOURCE_OK ? XR_XIR_OK :
        opened == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
    if (status == XR_XIR_OK) status = params ? xlsp_source_folders_read(resources, server, params, &owner) :
        xlsp_source_folders_single(resources, server, uri, name, add, &owner);
    if (status == XR_XIR_OK) status = xlsp_source_folders_commit(owner, server, &changed);
    if (status != XR_XIR_OK) {
        server->source_failure.stage = 15; server->source_failure.status = (unsigned)status;
        lsp_log("Workspace folders rejected (status=%u)", (unsigned)status); goto done;
    }
    server->source_failure.stage = server->source_failure.status = 0;
    if (!changed) goto done;
    xlsp_free_exports_cache(server);
    if (server->index_progress_token) {
        xlsp_progress_end(server, server->index_progress_token, "Workspace folders changed");
        xr_free(server->index_progress_token); server->index_progress_token = NULL;
    }
    if (server->workspace_folder_count) {
        const char *roots[MAX_WORKSPACE_FOLDERS];
        for (int i = 0; i < server->workspace_folder_count; ++i) roots[i] = server->workspace_folders[i].path;
        xlsp_workspace_start_background_index_roots(server, roots, server->workspace_folder_count);
    }
    lsp_log("Workspace folders updated: %d total", server->workspace_folder_count);
done:
    xlsp_source_folders_free(owner); xr_compile_resources_release(resources);
}
XR_FUNC void xlsp_handle_ws_add_folder(XrLspServer *server, const char *uri, const char *name) {
    workspace_folders_apply(server, NULL, uri, name ? name : "", true);
}
XR_FUNC void xlsp_handle_ws_remove_folder(XrLspServer *server, const char *uri) {
    workspace_folders_apply(server, NULL, uri, "", false);
}
XR_FUNC void xlsp_handle_ws_did_change_workspace_folders(XrLspServer *server, XrJsonValue *params) {
    if (!params) return;
    workspace_folders_apply(server, params, NULL, NULL, false);
}

// ============================================================================
// Configuration
// ============================================================================

static void xlsp_handle_ws_apply_configuration(XrLspServer *server, XrJsonValue *settings) {
    if (!server || !settings) return;
    XrCompileResourceLimits limits = (XrCompileResourceLimits)XR_RESOURCE_SANDBOX_COMPILER_LIMITS;
    XrCompileResources *resources = NULL;
    XrCompileResourceStatus opened = xr_compile_resources_new(&limits, &resources);
    XrXirStatus status = opened == XR_COMPILE_RESOURCE_OK ? XR_XIR_OK :
        opened == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
    XlspSourceSettings *patch = NULL; bool clear = false;
    if (status == XR_XIR_OK) status = xlsp_source_settings_read(resources, settings, &patch);
    if (status == XR_XIR_OK) status = xlsp_source_settings_apply(patch, server, &clear);
    xlsp_source_settings_free(patch);
    xr_compile_resources_release(resources);
    if (status == XR_XIR_UNRESOLVED) return;
    server->source_failure.stage = status == XR_XIR_OK ? 0 : 13;
    server->source_failure.status = (unsigned)status;
    if (status != XR_XIR_OK) {
        lsp_log("Configuration rejected (status=%u)", (unsigned)status); return;
    }
    if (clear) xlsp_clear_all_diagnostics(server);
    lsp_log("Configuration updated: debounce=%dms, diagnostics=%s, maxItems=%d, tabSize=%d",
        server->config.diagnostic_debounce_ms, server->config.diagnostics_enabled ? "on" : "off",
        server->config.completion_max_items, server->config.format_tab_size);
}

XR_FUNC void xlsp_handle_ws_did_change_configuration(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *settings = xjson_get_object(params, "settings");
    xlsp_handle_ws_apply_configuration(server, settings);
}
