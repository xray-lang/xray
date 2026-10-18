/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_token_update.c - Atomic adoption of a successfully published token cache
 */
#include "xlsp_token_update.h"
#include "xlsp_source_query.h"
#include <string.h>
struct XlspTokenUpdate {
    char *uri;
    int version;
    uint32_t previous_result_id;
    XlspSourceTokens *tokens;
};
static XrXirStatus token_update_resource(XrCompileResourceStatus status) {
    return status == XR_COMPILE_RESOURCE_OK ? XR_XIR_OK :
        status == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET :
        status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BAD_STRUCTURE;
}
XR_FUNC XrXirStatus xlsp_token_update_new(const XlspTokenUpdateInput *input, XlspTokenUpdate **out) {
    if (!input || !input->resources || !input->uri || !input->tokens || !*input->tokens ||
        !out || *out || input->uri_length == SIZE_MAX || input->previous_result_id == UINT32_MAX)
        return XR_XIR_BAD_STRUCTURE;
    XlspTokenUpdate *update = NULL;
    XrXirStatus status = token_update_resource(xr_compile_resources_calloc(input->resources, 1, sizeof(*update), (void **)&update));
    if (status != XR_XIR_OK) return status;
    status = token_update_resource(xr_compile_resources_work(input->resources, input->uri_length));
    if (status != XR_XIR_OK) goto failed;
    if (memchr(input->uri, 0, input->uri_length)) { status = XR_XIR_BAD_STRUCTURE; goto failed; }
    status = token_update_resource(xr_compile_resources_alloc(input->resources, input->uri_length + 1, (void **)&update->uri));
    if (status != XR_XIR_OK) goto failed;
    status = token_update_resource(xr_compile_resources_work(input->resources, input->uri_length + 1));
    if (status != XR_XIR_OK) goto failed;
    memcpy(update->uri, input->uri, input->uri_length); update->uri[input->uri_length] = 0;
    update->version = input->version; update->previous_result_id = input->previous_result_id;
    update->tokens = *input->tokens; *input->tokens = NULL; *out = update;
    return XR_XIR_OK;
failed:
    xlsp_token_update_free(update); return status;
}
XR_FUNC bool xlsp_token_update_matches(XrLspServer *server, const XlspTokenUpdate *update) {
    if (!server || !update || !update->tokens || !update->uri || update->previous_result_id == UINT32_MAX) return false;
    XrLspDocument *doc = xlsp_document_store_get(server->doc_table, update->uri);
    return doc && doc->server == server && doc->version == update->version &&
        doc->sem_token_result_id == update->previous_result_id;
}
XR_FUNC bool xlsp_token_update_commit(XrLspServer *server, XlspTokenUpdate *update) {
    if (!xlsp_token_update_matches(server, update)) return false;
    XrLspDocument *doc = xlsp_document_store_get(server->doc_table, update->uri);
    XlspSourceTokens *old = doc->source_tokens;
    doc->source_tokens = update->tokens; update->tokens = NULL;
    doc->sem_token_result_id = update->previous_result_id + 1;
    xlsp_source_tokens_free(old); return true;
}
XR_FUNC void xlsp_token_update_free(XlspTokenUpdate *update) {
    if (!update) return;
    xlsp_source_tokens_free(update->tokens); xr_compile_resources_free(update->uri); xr_compile_resources_free(update);
}
