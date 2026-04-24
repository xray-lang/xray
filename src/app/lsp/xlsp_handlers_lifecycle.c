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
#include "xlsp_json.h"
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
    XrJsonValue *folders = xlsp_json_get_array(params, "workspaceFolders");
    if (folders) {
        int count = xlsp_json_array_len(folders);
        for (int i = 0; i < count && i < MAX_WORKSPACE_FOLDERS; i++) {
            XrJsonValue *folder = xlsp_json_array_get(folders, i);
            const char *uri = xlsp_json_get_string(folder, "uri");
            const char *name = xlsp_json_get_string(folder, "name");
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
        const char *root_uri = xlsp_json_get_string(params, "rootUri");
        const char *root_path = xlsp_json_get_string(params, "rootPath");
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
                     server->workspace_folders[0].path ? server->workspace_folders[0].path : "(null)");
        }
    }

    // Load project-specific config from xray.toml (first folder wins)
    for (int i = 0; i < server->workspace_folder_count; i++) {
        if (server->workspace_folders[i].path) {
            if (xlsp_config_load_from_toml(&server->config,
                                           server->workspace_folders[i].path)) {
                server->workspace_folders[i].config_loaded = true;
                lsp_log("Loaded LSP config from xray.toml in: %s",
                        server->workspace_folders[i].path);
                break;
            }
        }
    }

    const char *display_root = server->workspace_folder_count > 0
        ? server->workspace_folders[0].path : "(none)";
    lsp_log("Initializing with root: %s", display_root);

    // Build capabilities response
    XrJsonValue *result = xlsp_json_new_object();
    XrJsonValue *capabilities = xlsp_json_new_object();

    // Text document sync (incremental)
    XrJsonValue *textDocSync = xlsp_json_new_object();
    xlsp_json_object_set_new(textDocSync, "openClose", xlsp_json_new_bool(true));
    xlsp_json_object_set_new(textDocSync, "change", xlsp_json_new_number(LSP_SYNC_INCREMENTAL));
    xlsp_json_object_set_new(capabilities, "textDocumentSync", textDocSync);

    // Completion
    if (server->capabilities.completion) {
        XrJsonValue *completion = xlsp_json_new_object();
        XrJsonValue *triggerChars = xlsp_json_new_array();
        xlsp_json_array_push(triggerChars, xlsp_json_new_string("."));
        xlsp_json_object_set_new(completion, "triggerCharacters", triggerChars);
        xlsp_json_object_set_new(completion, "resolveProvider", xlsp_json_new_bool(true));
        xlsp_json_object_set_new(capabilities, "completionProvider", completion);
        lsp_log("completionProvider enabled with trigger '.'");
    } else {
        lsp_log("completionProvider DISABLED");
    }

    // Hover
    if (server->capabilities.hover) {
        xlsp_json_object_set_new(capabilities, "hoverProvider", xlsp_json_new_bool(true));
    }

    // Definition
    if (server->capabilities.definition) {
        xlsp_json_object_set_new(capabilities, "definitionProvider", xlsp_json_new_bool(true));
    }

    // References
    if (server->capabilities.references) {
        xlsp_json_object_set_new(capabilities, "referencesProvider", xlsp_json_new_bool(true));
    }

    // Document symbol
    if (server->capabilities.document_symbol) {
        xlsp_json_object_set_new(capabilities, "documentSymbolProvider", xlsp_json_new_bool(true));
    }

    // Rename
    if (server->capabilities.rename) {
        XrJsonValue *rename = xlsp_json_new_object();
        xlsp_json_object_set_new(rename, "prepareProvider", xlsp_json_new_bool(true));
        xlsp_json_object_set_new(capabilities, "renameProvider", rename);
    }

    // Formatting
    if (server->capabilities.formatting) {
        xlsp_json_object_set_new(capabilities, "documentFormattingProvider", xlsp_json_new_bool(true));

        // On-type formatting (auto-indent on } and newline)
        XrJsonValue *onType = xlsp_json_new_object();
        xlsp_json_object_set_new(onType, "firstTriggerCharacter", xlsp_json_new_string("}"));
        XrJsonValue *moreTriggers = xlsp_json_new_array();
        xlsp_json_array_push(moreTriggers, xlsp_json_new_string("\n"));
        xlsp_json_array_push(moreTriggers, xlsp_json_new_string(";"));
        xlsp_json_object_set_new(onType, "moreTriggerCharacter", moreTriggers);
        xlsp_json_object_set_new(capabilities, "documentOnTypeFormattingProvider", onType);
    }

    // Signature help
    XrJsonValue *sigHelp = xlsp_json_new_object();
    XrJsonValue *sigTriggers = xlsp_json_new_array();
    xlsp_json_array_push(sigTriggers, xlsp_json_new_string("("));
    xlsp_json_array_push(sigTriggers, xlsp_json_new_string(","));
    xlsp_json_object_set_new(sigHelp, "triggerCharacters", sigTriggers);
    xlsp_json_object_set_new(capabilities, "signatureHelpProvider", sigHelp);

    // Semantic tokens
    XrJsonValue *semTokens = xlsp_json_new_object();
    xlsp_json_object_set_new(semTokens, "legend", xlsp_semantic_tokens_legend());
    XrJsonValue *semFull = xlsp_json_new_object();
    xlsp_json_object_set_new(semFull, "delta", xlsp_json_new_bool(true));
    xlsp_json_object_set_new(semTokens, "full", semFull);
    xlsp_json_object_set_new(semTokens, "range", xlsp_json_new_bool(true));
    xlsp_json_object_set_new(capabilities, "semanticTokensProvider", semTokens);

    // Inlay hints
    xlsp_json_object_set_new(capabilities, "inlayHintProvider", xlsp_json_new_bool(true));

    // CodeLens
    xlsp_json_object_set_new(capabilities, "codeLensProvider",
        xlsp_json_new_object());  // empty object = supported, no resolve

    // Folding range
    xlsp_json_object_set_new(capabilities, "foldingRangeProvider", xlsp_json_new_bool(true));

    // Code actions
    XrJsonValue *codeAction = xlsp_json_new_object();
    XrJsonValue *codeActionKinds = xlsp_json_new_array();
    xlsp_json_array_push(codeActionKinds, xlsp_json_new_string("quickfix"));
    xlsp_json_array_push(codeActionKinds, xlsp_json_new_string("source.organizeImports"));
    xlsp_json_object_set_new(codeAction, "codeActionKinds", codeActionKinds);
    xlsp_json_object_set_new(capabilities, "codeActionProvider", codeAction);

    // Document highlight
    xlsp_json_object_set_new(capabilities, "documentHighlightProvider", xlsp_json_new_bool(true));

    // Workspace symbol
    xlsp_json_object_set_new(capabilities, "workspaceSymbolProvider", xlsp_json_new_bool(true));

    // Selection range
    xlsp_json_object_set_new(capabilities, "selectionRangeProvider", xlsp_json_new_bool(true));

    // Document link
    xlsp_json_object_set_new(capabilities, "documentLinkProvider", xlsp_json_new_bool(true));

    // Call hierarchy
    xlsp_json_object_set_new(capabilities, "callHierarchyProvider", xlsp_json_new_bool(true));

    // Type hierarchy
    xlsp_json_object_set_new(capabilities, "typeHierarchyProvider", xlsp_json_new_bool(true));

    // Implementation
    xlsp_json_object_set_new(capabilities, "implementationProvider", xlsp_json_new_bool(true));

    // Workspace capabilities
    XrJsonValue *workspace = xlsp_json_new_object();
    XrJsonValue *workspaceFolders = xlsp_json_new_object();
    xlsp_json_object_set_new(workspaceFolders, "supported", xlsp_json_new_bool(true));
    xlsp_json_object_set_new(workspaceFolders, "changeNotifications", xlsp_json_new_bool(true));
    xlsp_json_object_set_new(workspace, "workspaceFolders", workspaceFolders);
    xlsp_json_object_set_new(capabilities, "workspace", workspace);

    // Position encoding (LSP 3.17 general capability).
    // We operate on UTF-16 code units throughout xlsp_position_to_offset /
    // xlsp_offset_to_position, so declare that explicitly — clients that
    // advertised UTF-8 / UTF-32 then fall back to UTF-16, and we avoid
    // silent mismatches for files containing astral-plane characters.
    xlsp_json_object_set_new(capabilities, "positionEncoding",
                             xlsp_json_new_string("utf-16"));

    xlsp_json_object_set_new(result, "capabilities", capabilities);

    // Server info
    XrJsonValue *serverInfo = xlsp_json_new_object();
    xlsp_json_object_set_new(serverInfo, "name", xlsp_json_new_string("xray-lsp"));
    xlsp_json_object_set_new(serverInfo, "version", xlsp_json_new_string(XRAY_VERSION_STRING));
    xlsp_json_object_set_new(result, "serverInfo", serverInfo);

    server->initialized = true;

    return result;
}

void xlsp_handle_lc_initialized(XrLspServer *server, XrJsonValue *params) {
    (void)params;
    lsp_log("Client initialized");

    // Register for file watching via client/registerCapability
    // Watch for .xr files changes (create, modify, delete)
    XrJsonValue *reg_params = xlsp_json_new_object();
    XrJsonValue *registrations = xlsp_json_new_array();

    XrJsonValue *registration = xlsp_json_new_object();
    xlsp_json_object_set_new(registration, "id", xlsp_json_new_string("xray-file-watcher"));
    xlsp_json_object_set_new(registration, "method", xlsp_json_new_string("workspace/didChangeWatchedFiles"));

    XrJsonValue *reg_options = xlsp_json_new_object();
    XrJsonValue *watchers = xlsp_json_new_array();

    // Watch all .xr files
    XrJsonValue *watcher = xlsp_json_new_object();
    xlsp_json_object_set_new(watcher, "globPattern", xlsp_json_new_string("**/*.xr"));
    xlsp_json_object_set_new(watcher, "kind", xlsp_json_new_number(LSP_WATCH_ALL));
    xlsp_json_array_push(watchers, watcher);

    xlsp_json_object_set_new(reg_options, "watchers", watchers);
    xlsp_json_object_set_new(registration, "registerOptions", reg_options);
    xlsp_json_array_push(registrations, registration);
    xlsp_json_object_set_new(reg_params, "registrations", registrations);

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
    (void)params;
    lsp_log("Shutdown requested");
    // Per LSP spec: shutdown is a request, not an exit trigger. We
    // flush state, reply with null, then wait for the mandatory
    // `exit` notification. After this point handle_message will
    // reject everything except `shutdown`/`exit` with -32002.
    server->shutdown_received = true;
    return xlsp_json_new_null();
}

void xlsp_handle_lc_exit(XrLspServer *server, XrJsonValue *params) {
    (void)params;
    lsp_log("Exit notification received");
    // Only `exit` drops us out of xlsp_server_run. A process-wide
    // exit code of 0 vs 1 is decided later based on shutdown_received.
    server->exit_received = true;
}

