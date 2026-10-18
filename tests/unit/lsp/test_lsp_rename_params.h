/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lsp_rename_params.h - Owned parameters for actual rename handlers
 *
 * KEY CONCEPT:
 *   Original binding assertions exercise the same request authority as clients.
 */
#ifndef TEST_LSP_RENAME_PARAMS_H
#define TEST_LSP_RENAME_PARAMS_H
#include "../../../src/app/lsp/xlsp_handlers_textdoc.h"

// Consume the child on either outcome so fixture construction is failure atomic.
static bool rename_test_member(XrJsonValue *object,const char *key,XrJsonValue *child) {
    if(!child)return false;
    xjson_object_set(object,key,child);
    if(xjson_get(object,key)==child)return true;
    xjson_free(child);return false;
}
static XrJsonValue *rename_test_integer(uint32_t number) {
    XrJsonValue *value=xjson_new_number(0);
    if(value){value->is_integer=true;value->as.integer=number;}
    return value;
}
static XrJsonValue *rename_test_params(const char *uri,XrLspPosition position,const char *name) {
    XrJsonValue *params=xjson_new_object(),*document=xjson_new_object(),*point=xjson_new_object();
    if(!params||!document||!point)goto failed;
    if(!rename_test_member(document,"uri",xjson_new_string(uri))||
       !rename_test_member(point,"line",rename_test_integer(position.line))||
       !rename_test_member(point,"character",rename_test_integer(position.character)))goto failed;
    bool accepted=rename_test_member(params,"textDocument",document);document=NULL;
    if(!accepted)goto failed;
    accepted=rename_test_member(params,"position",point);point=NULL;
    if(!accepted||(name&&!rename_test_member(params,"newName",xjson_new_string(name))))goto failed;
    return params;
failed:
    xjson_free(document);xjson_free(point);xjson_free(params);return NULL;
}
#endif // TEST_LSP_RENAME_PARAMS_H
