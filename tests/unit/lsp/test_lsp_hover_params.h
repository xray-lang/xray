/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef TEST_LSP_HOVER_PARAMS_H
#define TEST_LSP_HOVER_PARAMS_H
#include "../../../src/app/lsp/xlsp_server.h"
#include "test_lsp_rename_params.h"
/* Exercise the public request boundary with independently owned parameters. */
static XrJsonValue *hover_test_at(XrLspServer *server,XrLspDocument *document,XrLspPosition position) {
    if(!document)return xlsp_handle_td_hover(server,NULL);
    XrJsonValue *params=rename_test_params(document->uri,position,NULL);
    if(!params)return NULL;
    XrJsonValue *result=xlsp_handle_td_hover(server,params);
    xjson_free(params);return result;
}
#endif // TEST_LSP_HOVER_PARAMS_H
