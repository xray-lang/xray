/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_initialize_result.c - Complete capability response ownership
 *
 * KEY CONCEPT:
 *   Every child is either adopted or freed, including key-allocation failure
 *   after adoption. A complete response precedes server initialization state.
 */
#include "xlsp_initialize_result.h"
#include "xlsp_semantic_tokens.h"
#include "xlsp_index_json.h"
#include "xray_version.h"

static XrJsonValue *initialize_strings(const char *const *strings, size_t count) {
    XrJsonValue *array = xjson_new_array(); if (!array) return NULL;
    for (size_t i = 0; i < count; ++i) {
        XrJsonValue *value = xjson_new_string(strings[i]); if (!value) { xjson_free(array); return NULL; }
        int before = array->as.array.count; xjson_array_push(array, value);
        if (array->as.array.count != before + 1) { xjson_free(value); xjson_free(array); return NULL; }
    }
    return array;
}
static XrJsonValue *initialize_named_strings(const char *key, const char *const *strings, size_t count) {
    XrJsonValue *object = xjson_new_object(); if (!object) return NULL;
    if (!index_json_member(object, key, initialize_strings(strings, count))) { xjson_free(object); return NULL; }
    return object;
}
static XrJsonValue *initialize_sync(void) {
    XrJsonValue *object = xjson_new_object(); if (!object) return NULL;
    if (!index_json_member(object, "openClose", xjson_new_bool(true)) ||
        !index_json_member(object, "change", xjson_new_number(2))) { xjson_free(object); return NULL; }
    return object;
}
static XrJsonValue *initialize_completion(void) {
    const char *strings[] = {"."}; XrJsonValue *object = initialize_named_strings("triggerCharacters", strings, 1);
    if (object && !index_json_member(object, "resolveProvider", xjson_new_bool(true))) { xjson_free(object); object = NULL; }
    return object;
}
static XrJsonValue *initialize_option(const char *key) {
    XrJsonValue *object = xjson_new_object(); if (!object) return NULL;
    if (!index_json_member(object, key, xjson_new_bool(true))) { xjson_free(object); return NULL; }
    return object;
}
static XrJsonValue *initialize_on_type(void) {
    const char *strings[] = {"\n", ";"}; XrJsonValue *object = initialize_named_strings("moreTriggerCharacter", strings, 2);
    if (object && !index_json_member(object, "firstTriggerCharacter", xjson_new_string("}"))) { xjson_free(object); object = NULL; }
    return object;
}
static XrJsonValue *initialize_workspace(void) {
    XrJsonValue *folders = initialize_option("supported"); if (!folders) return NULL;
    if (!index_json_member(folders, "changeNotifications", xjson_new_bool(true))) { xjson_free(folders); return NULL; }
    XrJsonValue *workspace = xjson_new_object(); if (!workspace) { xjson_free(folders); return NULL; }
    if (!index_json_member(workspace, "workspaceFolders", folders)) { xjson_free(workspace); return NULL; }
    return workspace;
}
static XrJsonValue *initialize_capabilities(const XrLspServer *server, const XlspInitializeOptions *options) {
    XrJsonValue *cap = xjson_new_object(); if (!cap) return NULL;
    if (!index_json_member(cap, "textDocumentSync", initialize_sync())) goto failed;
    if (server->capabilities.completion && !index_json_member(cap, "completionProvider", initialize_completion())) goto failed;
    const char *optional[] = {"hoverProvider", "definitionProvider", "referencesProvider", "documentSymbolProvider"};
    const bool enabled[] = {server->capabilities.hover, server->capabilities.definition,
        server->capabilities.references, server->capabilities.document_symbol};
    for (size_t i = 0; i < sizeof(optional) / sizeof(*optional); ++i)
        if (enabled[i] && !index_json_member(cap, optional[i], xjson_new_bool(true))) goto failed;
    if (server->capabilities.rename && !index_json_member(cap, "renameProvider",
        options->prepare_rename ? initialize_option("prepareProvider") : xjson_new_bool(true))) goto failed;
    if (server->capabilities.formatting && (!index_json_member(cap, "documentFormattingProvider", xjson_new_bool(true)) ||
        !index_json_member(cap, "documentOnTypeFormattingProvider", initialize_on_type()))) goto failed;
    const char *sig[] = {"(", ","}, *actions[] = {"quickfix", "source.organizeImports"};
    if (!index_json_member(cap, "signatureHelpProvider", initialize_named_strings("triggerCharacters", sig, 2)) ||
        !index_json_member(cap, "semanticTokensProvider", xlsp_semantic_tokens_provider()) ||
        !index_json_member(cap, "codeLensProvider", xjson_new_object()) ||
        !index_json_member(cap, "codeActionProvider", options->code_action_literals ?
            initialize_named_strings("codeActionKinds", actions, 2) : xjson_new_bool(true)) ||
        !index_json_member(cap, "documentLinkProvider", xjson_new_object()) ||
        !index_json_member(cap, "workspace", initialize_workspace()) ||
        !index_json_member(cap, "positionEncoding", xjson_new_string("utf-16"))) goto failed;
    const char *required[] = {"inlayHintProvider", "foldingRangeProvider", "documentHighlightProvider", "workspaceSymbolProvider",
        "selectionRangeProvider", "callHierarchyProvider", "typeHierarchyProvider", "implementationProvider"};
    for (size_t i = 0; i < sizeof(required) / sizeof(*required); ++i)
        if (!index_json_member(cap, required[i], xjson_new_bool(true))) goto failed;
    return cap;
failed:
    xjson_free(cap); return NULL;
}
static XrJsonValue *initialize_info(void) {
    XrJsonValue *info = xjson_new_object(); if (!info) return NULL;
    if (!index_json_member(info, "name", xjson_new_string("xray-lsp")) ||
        !index_json_member(info, "version", xjson_new_string(XRAY_VERSION_STRING))) { xjson_free(info); return NULL; }
    return info;
}
XR_FUNC XrJsonValue *xlsp_initialize_result(const XrLspServer *server, const XlspInitializeOptions *options) {
    if (!server || !options) return NULL;
    XrJsonValue *result = xjson_new_object(); if (!result) return NULL;
    if (!index_json_member(result, "capabilities", initialize_capabilities(server, options)) ||
        !index_json_member(result, "serverInfo", initialize_info())) { xjson_free(result); return NULL; }
    return result;
}
