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
#include "xlsp_semantic_tokens.h"
#include "xray_version.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"

XrJsonValue *xlsp_handle_lc_initialize(XrLspServer *server, XrJsonValue *params) {
    // Collect workspace folders from initialize params.
    // If the client provides workspaceFolders, use them directly.
    // Otherwise, fall back to rootUri / rootPath and create a
    // synthetic workspace folder so the rest of the code has a
    // single model to work with.
    XrJsonValue *folders = xjson_get_array(params, "workspaceFolders");
    if (folders) {
        int count = xjson_array_len(folders);
        for (int i = 0; i < count && i < MAX_WORKSPACE_FOLDERS; i++) {
            XrJsonValue *folder = xjson_array_get(folders, i);
            const char *uri = xjson_get_string(folder, "uri");
            const char *name = xjson_get_string(folder, "name");
            if (uri) {
                int idx = server->workspace_folder_count++;
                server->workspace_folders[idx].uri = xr_strdup(uri);
                server->workspace_folders[idx].name = name ? xr_strdup(name) : xr_strdup("");
                server->workspace_folders[idx].path = xr_strdup(xlsp_uri_to_path(uri));
            }
        }
        lsp_log("Initialized with %d workspace folders", server->workspace_folder_count);
    }

    // Fold rootUri / rootPath into workspace_folders if no folders were
    // provided (single-root fallback).
    if (server->workspace_folder_count == 0) {
        const char *root_uri = xjson_get_string(params, "rootUri");
        const char *root_path = xjson_get_string(params, "rootPath");
        if (root_uri || root_path) {
            int idx = server->workspace_folder_count++;
            server->workspace_folders[idx].uri = root_uri ? xr_strdup(root_uri) : NULL;
            server->workspace_folders[idx].name = xr_strdup("root");
            if (root_path) {
                server->workspace_folders[idx].path = xr_strdup(root_path);
            } else if (root_uri) {
                server->workspace_folders[idx].path = xr_strdup(xlsp_uri_to_path(root_uri));
            }
            lsp_log("Folded rootUri/rootPath into workspace folder[0]: %s",
                    server->workspace_folders[0].path ? server->workspace_folders[0].path
                                                      : "(null)");
        }
    }

    // Load project-specific config from xray.toml (first folder wins)
    for (int i = 0; i < server->workspace_folder_count; i++) {
        if (server->workspace_folders[i].path) {
            if (xlsp_config_load_from_toml(&server->config, server->workspace_folders[i].path)) {
                server->workspace_folders[i].config_loaded = true;
                lsp_log("Loaded LSP config from xray.toml in: %s",
                        server->workspace_folders[i].path);
                break;
            }
        }
    }

    const char *display_root =
        server->workspace_folder_count > 0 ? server->workspace_folders[0].path : "(none)";
    lsp_log("Initializing with root: %s", display_root);

    // Build capabilities response
    XrJsonValue *result = xjson_new_object();
    XrJsonValue *capabilities = xjson_new_object();

    // Text document sync (incremental)
    XrJsonValue *textDocSync = xjson_new_object();
    xjson_object_set_new(textDocSync, "openClose", xjson_new_bool(true));
    xjson_object_set_new(textDocSync, "change", xjson_new_number(LSP_SYNC_INCREMENTAL));
    xjson_object_set_new(capabilities, "textDocumentSync", textDocSync);

    // Completion
    if (server->capabilities.completion) {
        XrJsonValue *completion = xjson_new_object();
        XrJsonValue *triggerChars = xjson_new_array();
        xjson_array_push(triggerChars, xjson_new_string("."));
        xjson_object_set_new(completion, "triggerCharacters", triggerChars);
        xjson_object_set_new(completion, "resolveProvider", xjson_new_bool(true));
        xjson_object_set_new(capabilities, "completionProvider", completion);
        lsp_log("completionProvider enabled with trigger '.'");
    } else {
        lsp_log("completionProvider DISABLED");
    }

    // Hover
    if (server->capabilities.hover) {
        xjson_object_set_new(capabilities, "hoverProvider", xjson_new_bool(true));
    }

    // Definition
    if (server->capabilities.definition) {
        xjson_object_set_new(capabilities, "definitionProvider", xjson_new_bool(true));
    }

    // References
    if (server->capabilities.references) {
        xjson_object_set_new(capabilities, "referencesProvider", xjson_new_bool(true));
    }

    // Document symbol
    if (server->capabilities.document_symbol) {
        xjson_object_set_new(capabilities, "documentSymbolProvider", xjson_new_bool(true));
    }

    // Rename
    if (server->capabilities.rename) {
        XrJsonValue *rename = xjson_new_object();
        xjson_object_set_new(rename, "prepareProvider", xjson_new_bool(true));
        xjson_object_set_new(capabilities, "renameProvider", rename);
    }

    // Formatting
    if (server->capabilities.formatting) {
        xjson_object_set_new(capabilities, "documentFormattingProvider", xjson_new_bool(true));

        // On-type formatting (auto-indent on } and newline)
        XrJsonValue *onType = xjson_new_object();
        xjson_object_set_new(onType, "firstTriggerCharacter", xjson_new_string("}"));
        XrJsonValue *moreTriggers = xjson_new_array();
        xjson_array_push(moreTriggers, xjson_new_string("\n"));
        xjson_array_push(moreTriggers, xjson_new_string(";"));
        xjson_object_set_new(onType, "moreTriggerCharacter", moreTriggers);
        xjson_object_set_new(capabilities, "documentOnTypeFormattingProvider", onType);
    }

    // Signature help
    XrJsonValue *sigHelp = xjson_new_object();
    XrJsonValue *sigTriggers = xjson_new_array();
    xjson_array_push(sigTriggers, xjson_new_string("("));
    xjson_array_push(sigTriggers, xjson_new_string(","));
    xjson_object_set_new(sigHelp, "triggerCharacters", sigTriggers);
    xjson_object_set_new(capabilities, "signatureHelpProvider", sigHelp);

    // Semantic tokens
    XrJsonValue *semTokens = xjson_new_object();
    xjson_object_set_new(semTokens, "legend", xlsp_semantic_tokens_legend());
    XrJsonValue *semFull = xjson_new_object();
    xjson_object_set_new(semFull, "delta", xjson_new_bool(true));
    xjson_object_set_new(semTokens, "full", semFull);
    xjson_object_set_new(semTokens, "range", xjson_new_bool(true));
    xjson_object_set_new(capabilities, "semanticTokensProvider", semTokens);

    // Inlay hints
    xjson_object_set_new(capabilities, "inlayHintProvider", xjson_new_bool(true));

    // CodeLens
    xjson_object_set_new(capabilities, "codeLensProvider",
                         xjson_new_object());  // empty object = supported, no resolve

    // Folding range
    xjson_object_set_new(capabilities, "foldingRangeProvider", xjson_new_bool(true));

    // Code actions
    XrJsonValue *codeAction = xjson_new_object();
    XrJsonValue *codeActionKinds = xjson_new_array();
    xjson_array_push(codeActionKinds, xjson_new_string("quickfix"));
    xjson_array_push(codeActionKinds, xjson_new_string("source.organizeImports"));
    xjson_object_set_new(codeAction, "codeActionKinds", codeActionKinds);
    xjson_object_set_new(capabilities, "codeActionProvider", codeAction);

    // Document highlight
    xjson_object_set_new(capabilities, "documentHighlightProvider", xjson_new_bool(true));

    // Workspace symbol
    xjson_object_set_new(capabilities, "workspaceSymbolProvider", xjson_new_bool(true));

    // Selection range
    xjson_object_set_new(capabilities, "selectionRangeProvider", xjson_new_bool(true));

    // Document link
    xjson_object_set_new(capabilities, "documentLinkProvider", xjson_new_bool(true));

    // Call hierarchy
    xjson_object_set_new(capabilities, "callHierarchyProvider", xjson_new_bool(true));

    // Type hierarchy
    xjson_object_set_new(capabilities, "typeHierarchyProvider", xjson_new_bool(true));

    // Implementation
    xjson_object_set_new(capabilities, "implementationProvider", xjson_new_bool(true));

    // Workspace capabilities
    XrJsonValue *workspace = xjson_new_object();
    XrJsonValue *workspaceFolders = xjson_new_object();
    xjson_object_set_new(workspaceFolders, "supported", xjson_new_bool(true));
    xjson_object_set_new(workspaceFolders, "changeNotifications", xjson_new_bool(true));
    xjson_object_set_new(workspace, "workspaceFolders", workspaceFolders);
    xjson_object_set_new(capabilities, "workspace", workspace);

    // Position encoding (LSP 3.17 general capability).
    // We operate on UTF-16 code units throughout xlsp_position_to_offset /
    // xlsp_offset_to_position, so declare that explicitly — clients that
    // advertised UTF-8 / UTF-32 then fall back to UTF-16, and we avoid
    // silent mismatches for files containing astral-plane characters.
    xjson_object_set_new(capabilities, "positionEncoding", xjson_new_string("utf-16"));

    xjson_object_set_new(result, "capabilities", capabilities);

    // Server info
    XrJsonValue *serverInfo = xjson_new_object();
    xjson_object_set_new(serverInfo, "name", xjson_new_string("xray-lsp"));
    xjson_object_set_new(serverInfo, "version", xjson_new_string(XRAY_VERSION_STRING));
    xjson_object_set_new(result, "serverInfo", serverInfo);

    server->initialized = true;

    return result;
}

void xlsp_handle_lc_initialized(XrLspServer *server, XrJsonValue *params) {
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

XrJsonValue *xlsp_handle_lc_shutdown(XrLspServer *server, XrJsonValue *params) {
    (void) params;
    lsp_log("Shutdown requested");
    // Per LSP spec: shutdown is a request, not an exit trigger. We
    // flush state, reply with null, then wait for the mandatory
    // `exit` notification. After this point handle_message will
    // reject everything except `shutdown`/`exit` with -32002.
    server->shutdown_received = true;
    return xjson_new_null();
}

void xlsp_handle_lc_exit(XrLspServer *server, XrJsonValue *params) {
    (void) params;
    lsp_log("Exit notification received");
    // Only `exit` drops us out of xlsp_server_run. A process-wide
    // exit code of 0 vs 1 is decided later based on shutdown_received.
    server->exit_received = true;
}
