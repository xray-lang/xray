/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_document_owner.h"
#include "app/lsp/xlsp_document_store.h"
#include "app/lsp/xlsp_source_open.h"
#include "app/lsp/xlsp_cache.h"
#include "app/lsp/xlsp_imports.h"
#include "base/xwindows_utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d:%s\n",__LINE__,#c); exit(2); } } while (0)
#include "xir_sdk_resource_test.h"
#include "lsp_open_json_owner.h"
#pragma push_macro("xr_free")
#undef xr_free
#define xr_free(p) lsp_json_free(p)
#include "app/lsp/xlsp_protocol_output.c"
#pragma pop_macro("xr_free")
typedef struct Block { void *pointer; size_t bytes; } Block;
static Block blocks[128];
static size_t app_attempts, app_fail=SIZE_MAX, app_live, app_bytes, rollbacks;
XR_FUNC void baseline_destroy(XrLspDocument *);
XR_FUNC void baseline_clear_diagnostics(XrLspServer *);
XR_FUNC void *document_test_malloc(size_t bytes) {
    if(app_attempts++==app_fail)return NULL;
    void *p=sdk_fixture_malloc(bytes);if(!p)return NULL;
    for(size_t i=0;i<128;++i)if(!blocks[i].pointer){blocks[i]=(Block){p,bytes};++app_live;app_bytes+=bytes;return p;}
    CHECK(false);return NULL;
}
XR_FUNC void *document_test_calloc(size_t count,size_t bytes) {
    CHECK(!count||bytes<=SIZE_MAX/count);void *p=document_test_malloc(count*bytes);
    if(p)memset(p,0,count*bytes);return p;
}
XR_FUNC void document_test_free(void *p) {
    if(!p)return;
    for(size_t i=0;i<128;++i)if(blocks[i].pointer==p){--app_live;app_bytes-=blocks[i].bytes;blocks[i]=(Block){0};sdk_fixture_free(p);return;}
    CHECK(false);
}
XR_FUNC void *document_test_realloc(void *p,size_t bytes) {
    if(!p)return document_test_malloc(bytes);
    if(app_attempts++==app_fail)return NULL;
    for(size_t i=0;i<128;++i)if(blocks[i].pointer==p){
        void *next=xr_realloc(p,bytes);if(!next)return NULL;
        app_bytes-=blocks[i].bytes;app_bytes+=bytes;blocks[i]=(Block){next,bytes};return next;
    }
    CHECK(false);return NULL;
}
XR_FUNC void lsp_log(const char *format,...) { (void)format; }
