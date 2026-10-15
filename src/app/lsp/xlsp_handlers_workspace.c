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
#include "../../api/xsource_compile.h"
#include "xlsp_index_pool.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"

// File change types (LSP spec)
#define FILE_CHANGE_CREATED 1
#define FILE_CHANGE_CHANGED 2
#define FILE_CHANGE_DELETED 3

XR_FUNC void xlsp_handle_ws_did_change_watched_files(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *changes = xjson_get_array(params, "changes");
    if (!changes)
        return;

    int change_count = xjson_array_len(changes);
    lsp_log("Received %d file change events", change_count);

    for (int i = 0; i < change_count; i++) {
        XrJsonValue *change = xjson_array_get(changes, i);
        const char *uri = xjson_get_string(change, "uri");
        int type = (int) xjson_get_int(change, "type");

        if (!uri)
            continue;

        if(type>=FILE_CHANGE_CREATED&&type<=FILE_CHANGE_DELETED&&server->source_revision!=UINT64_MAX)
            ++server->source_revision;

        lsp_log("File change: %s (type=%d)", uri, type);

        // Get document if it's open
        XrLspDocument *doc = xlsp_document_get(server, uri);

        char *index_path=NULL,*index_uri=NULL;XrXirStatus index_status=xlsp_source_uri_path(server->index_resources,uri,strlen(uri),&index_path);
        if(index_status==XR_XIR_OK){xlsp_index_pool_cancel_file(server->index_pool,index_path);index_status=xlsp_index_file_uri(server->index_resources,index_path,&index_uri);}
        if(index_status==XR_XIR_OK)index_status=xlsp_source_index_store_remove(server->source_index,index_uri);
        if(index_status!=XR_XIR_OK)xlsp_index_record_failure(server,index_status);
        xr_compile_resources_free(index_uri);xr_compile_resources_free(index_path);

        switch (type) {
            case FILE_CHANGE_CREATED:
            case FILE_CHANGE_CHANGED: {
                const char *path = xlsp_uri_to_path(uri);

                // Invalidate exports cache for this file
                xlsp_exports_cache_remove(server, path);

                // Re-index the file in workspace analyzer (for unopened files)
                if (!doc) {
                    xlsp_workspace_index_file(server, uri, path);
                }

                // If document is open, re-parse and publish diagnostics
                if (doc) {
                    doc->dirty = true;
                    xlsp_parse_document(doc, server);
                    xlsp_publish_diagnostics(server, doc);
                }
                break;
            }

            case FILE_CHANGE_DELETED: {
                const char *del_path = xlsp_uri_to_path(uri);

                // Remove from exports cache
                xlsp_exports_cache_remove(server, del_path);

                // Remove from analyzer so stale symbols don't linger
                if (server->workspace_analyzer) {
                    xa_analyzer_remove_file(server->workspace_analyzer, del_path);
                }

                // Drop the file from the shallow workspace symbol index too.
                if (server->source_index) {
                    {XrXirStatus status=xlsp_source_index_store_remove(server->source_index,uri);
                     if(status!=XR_XIR_OK){xlsp_index_record_failure(server,status);lsp_log("Deleted-file syntax index removal failed");}}
                }

                lsp_log("Purged state for deleted file: %s", del_path);
                break;
            }
        }
    }

    // Re-publish diagnostics for all open documents that import changed files
    // This ensures cross-file errors are updated
    // (For simplicity, we don't track import dependencies here)
}

// ============================================================================
// Workspace Folders
// ============================================================================

// Add a workspace folder
XR_FUNC void xlsp_handle_ws_add_folder(XrLspServer *server, const char *uri, const char *name) {
    if (server->workspace_folder_count >= MAX_WORKSPACE_FOLDERS) {
        lsp_log("Warning: Maximum workspace folders reached");
        return;
    }

    int idx = server->workspace_folder_count++;
    server->workspace_folders[idx].uri = xr_strdup(uri);
    server->workspace_folders[idx].name = name ? xr_strdup(name) : xr_strdup("");

    const char *path = xlsp_uri_to_path(uri);
    server->workspace_folders[idx].path = xr_strdup(path);
    server->workspace_folders[idx].config_loaded = false;
    server->workspace_folders[idx].index_requested = true;
    server->workspace_folders[idx].index_completed = false;

    lsp_log("Added workspace folder: %s (%s)", name ? name : uri, path);

    // Start indexing this folder
    xlsp_workspace_start_background_index(server, path);
}

// Remove a workspace folder and purge related state
XR_FUNC void xlsp_handle_ws_remove_folder(XrLspServer *server, const char *uri) {
    for (int i = 0; i < server->workspace_folder_count; i++) {
        if (strcmp(server->workspace_folders[i].uri, uri) == 0) {
            lsp_log("Removed workspace folder: %s", server->workspace_folders[i].name);

            // Purge analyzer and cache state for files under this folder
            if (server->workspace_folders[i].path) {
                xlsp_workspace_purge_prefix(server, server->workspace_folders[i].path);
            }

            xr_free(server->workspace_folders[i].uri);
            xr_free(server->workspace_folders[i].name);
            xr_free(server->workspace_folders[i].path);

            // Shift remaining folders
            for (int j = i; j < server->workspace_folder_count - 1; j++) {
                server->workspace_folders[j] = server->workspace_folders[j + 1];
            }
            server->workspace_folder_count--;
            if(server->workspace_folder_count) {
                const char *roots[MAX_WORKSPACE_FOLDERS];int count=server->workspace_folder_count;
                for(int root=0;root<count;++root)roots[root]=server->workspace_folders[root].path;
                xlsp_workspace_start_background_index_roots(server,roots,count);
            }
            return;
        }
    }
}

XR_FUNC void xlsp_handle_ws_did_change_workspace_folders(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *event = xjson_get_object(params, "event");
    if (!event)
        return;

    // Handle removed folders
    XrJsonValue *removed = xjson_get_array(event, "removed");
    if (removed) {
        int count = xjson_array_len(removed);
        for (int i = 0; i < count; i++) {
            XrJsonValue *folder = xjson_array_get(removed, i);
            const char *uri = xjson_get_string(folder, "uri");
            if (uri) {
                xlsp_handle_ws_remove_folder(server, uri);
            }
        }
    }

    // Handle added folders
    XrJsonValue *added = xjson_get_array(event, "added");
    if (added) {
        int count = xjson_array_len(added);
        for (int i = 0; i < count; i++) {
            XrJsonValue *folder = xjson_array_get(added, i);
            const char *uri = xjson_get_string(folder, "uri");
            const char *name = xjson_get_string(folder, "name");
            if (uri) {
                xlsp_handle_ws_add_folder(server, uri, name);
            }
        }
    }

    lsp_log("Workspace folders updated: %d total", server->workspace_folder_count);
}

// ============================================================================
// Configuration
// ============================================================================

static void xlsp_handle_ws_apply_configuration(XrLspServer *server, XrJsonValue *settings) {
    if (!server || !settings) return;
    XrCompileResourceLimits limits = xr_source_compile_default_resource_limits();
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
