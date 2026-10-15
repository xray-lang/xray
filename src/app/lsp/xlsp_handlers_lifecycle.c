/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_handlers_lifecycle.c - LSP lifecycle handlers
 *   initialize, initialized, shutdown, exit
 */

#include "xlsp_handlers_lifecycle.h"
#include "xlsp_server.h"
#include "../../base/xjson.h"
#include "xlsp_workspace.h"
#include "xlsp_utils.h"
#include "xlsp_source_initialize.h"
#include "../../api/xsource_compile.h"
#include "xray_version.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include <string.h>

XR_FUNC XrJsonValue *xlsp_handle_lc_initialize(XrLspServer *server, XrJsonValue *params) {
    if (!server) return NULL;
    XrCompileResourceLimits limits = xr_source_compile_default_resource_limits();
    XrCompileResources *resources = NULL; XlspSourceInitialize *owner = NULL; XrJsonValue *result = NULL;
    XrCompileResourceStatus opened = xr_compile_resources_new(&limits, &resources);
    XrXirStatus status = opened == XR_COMPILE_RESOURCE_OK ? XR_XIR_OK :
        opened == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
    if (status == XR_XIR_OK) status = xlsp_source_initialize_prepare(resources, server, params, &owner);
    if (status == XR_XIR_OK) status = xlsp_source_initialize_commit(owner, server, &result);
    xlsp_source_initialize_free(owner); xr_compile_resources_release(resources);
    server->source_failure.stage = status == XR_XIR_OK ? 0 : 16;
    server->source_failure.status = (unsigned)status;
    if (status != XR_XIR_OK) {
        lsp_log("Initialize rejected (status=%u)", (unsigned)status); return NULL;
    }
    // Project configuration is an independent atomic load after root admission.
    for (int i = 0; i < server->workspace_folder_count; ++i) {
        if (xlsp_config_load_from_toml(&server->config, server->workspace_folders[i].path)) {
            server->workspace_folders[i].config_loaded = true;
            lsp_log("Loaded LSP config from xray.toml in: %s", server->workspace_folders[i].path); break;
        }
    }
    lsp_log("Initializing with root: %s", server->workspace_folder_count ? server->workspace_folders[0].path : "(none)");
    return result;
}

XR_FUNC void xlsp_handle_lc_initialized(XrLspServer *server, XrJsonValue *params) {
    (void) params;
    lsp_log("Client initialized");

    // Register for file watching via client/registerCapability
    // Watch for .xr files changes (create, modify, delete)
    XrJsonValue *reg_params = xjson_new_object();
    XrJsonValue *registrations = xjson_new_array();

    XrJsonValue *registration = xjson_new_object();
    xjson_object_set_new(registration, "id", xjson_new_string("xray-file-watcher"));
    xjson_object_set_new(registration, "method",
                         xjson_new_string("workspace/didChangeWatchedFiles"));

    XrJsonValue *reg_options = xjson_new_object();
    XrJsonValue *watchers = xjson_new_array();

    // Watch all .xr files
    XrJsonValue *watcher = xjson_new_object();
    xjson_object_set_new(watcher, "globPattern", xjson_new_string("**/*.xr"));
    xjson_object_set_new(watcher, "kind", xjson_new_number(LSP_WATCH_ALL));
    xjson_array_push(watchers, watcher);

    xjson_object_set_new(reg_options, "watchers", watchers);
    xjson_object_set_new(registration, "registerOptions", reg_options);
    xjson_array_push(registrations, registration);
    xjson_object_set_new(reg_params, "registrations", registrations);

    // Send registration request (LSP spec requires this to be a request, not notification)
    xlsp_send_request(server, "client/registerCapability", reg_params);

    lsp_log("Registered file watcher for **/*.xr");

    // Start background workspace indexing for all workspace folders.
    // root_path/root_uri were folded into workspace_folders at initialize.
    if (server->workspace_folder_count > 0) {
        const char *paths[MAX_WORKSPACE_FOLDERS];
        int path_count = 0;
        for (int i = 0; i < server->workspace_folder_count; i++) {
            if (server->workspace_folders[i].path) {
                paths[path_count++] = server->workspace_folders[i].path;
                server->workspace_folders[i].index_requested = true;
            }
        }
        if (path_count > 0) {
            xlsp_workspace_start_background_index_roots(server, paths, path_count);
        }
    }
}

XR_FUNC XrJsonValue *xlsp_handle_lc_shutdown(XrLspServer *server, XrJsonValue *params) {
    (void) params;
    lsp_log("Shutdown requested");
    // Per LSP spec: shutdown is a request, not an exit trigger. We
    // flush state, reply with null, then wait for the mandatory
    // `exit` notification. After this point handle_message will
    // reject everything except `shutdown`/`exit` with -32002.
    server->shutdown_received = true;
    return xjson_new_null();
}

XR_FUNC void xlsp_handle_lc_exit(XrLspServer *server, XrJsonValue *params) {
    (void) params;
    lsp_log("Exit notification received");
    // Only `exit` drops us out of xlsp_server_run. A process-wide
    // exit code of 0 vs 1 is decided later based on shutdown_received.
    server->exit_received = true;
}
