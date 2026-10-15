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
#include "xlsp_protocol_output.h"
#include "xlsp_watcher_registration.h"
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
    if (!server) return;
    XrXirStatus status = XR_XIR_OK;
    const char *paths[MAX_WORKSPACE_FOLDERS]; int count = server->workspace_folder_count;
    if (!server->initialized || server->shutdown_received || server->exit_received) status = XR_XIR_BAD_STAGE;
    else if (!params || params->type != XR_JSON_OBJECT || count < 0 || count > MAX_WORKSPACE_FOLDERS) status = XR_XIR_BAD_STRUCTURE;
    for (int i = 0; status == XR_XIR_OK && i < count; ++i) {
        paths[i] = server->workspace_folders[i].path;
        if (!paths[i] || !paths[i][0]) status = XR_XIR_BAD_STRUCTURE;
    }
    XrJsonValue *registration = NULL;
    if (status == XR_XIR_OK) {
        registration = xlsp_watcher_registration_params(); if (!registration) status = XR_XIR_OUT_OF_MEMORY;
    }
    if (status == XR_XIR_OK) {
        status = xlsp_protocol_send_request(server, "client/registerCapability", registration); registration = NULL;
    }
    xjson_free(registration);
    server->source_failure.stage = status == XR_XIR_OK ? 0 : 17;
    server->source_failure.status = (unsigned)status;
    if (status != XR_XIR_OK) {
        server->exit_received = true; lsp_log("Initialized registration failed: %u", (unsigned)status); return;
    }
    lsp_log("File watcher registration request sent for **/*.xr");
    for (int i = 0; i < count; ++i) server->workspace_folders[i].index_requested = true;
    if (count) xlsp_workspace_start_background_index_roots(server, paths, count);
}
