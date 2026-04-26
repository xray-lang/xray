/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_handlers_workspace.h - LSP workspace handler declarations
 */

#ifndef XLSP_HANDLERS_WORKSPACE_H
#define XLSP_HANDLERS_WORKSPACE_H

#include "../../base/xjson.h"

typedef struct XrLspServer XrLspServer;

// Workspace handlers
XR_FUNC void xlsp_handle_ws_did_change_watched_files(XrLspServer *server, XrJsonValue *params);
XR_FUNC void xlsp_handle_ws_did_change_workspace_folders(XrLspServer *server, XrJsonValue *params);
XR_FUNC void xlsp_handle_ws_did_change_configuration(XrLspServer *server, XrJsonValue *params);

// Workspace folder helpers
XR_FUNC void xlsp_handle_ws_add_folder(XrLspServer *server, const char *uri, const char *name);
XR_FUNC void xlsp_handle_ws_remove_folder(XrLspServer *server, const char *uri);

#endif  // XLSP_HANDLERS_WORKSPACE_H
