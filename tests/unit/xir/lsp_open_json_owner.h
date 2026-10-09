/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_source_navigation.h"
typedef struct LspJsonBlock {void *pointer;size_t bytes;} LspJsonBlock;
static LspJsonBlock lsp_json_blocks[512];
static size_t lsp_json_calls,lsp_json_fail=SIZE_MAX,lsp_json_live,lsp_json_bytes;
static bool lsp_json_injected;
static size_t lsp_json_slot(void *pointer) {
    size_t i=0;while(i<512&&lsp_json_blocks[i].pointer!=pointer)++i;CHECK(i<512);return i;
}
static void *lsp_json_track(void *pointer,size_t bytes) {
    if(!pointer)return NULL;
    size_t i=lsp_json_slot(NULL);lsp_json_blocks[i]=(LspJsonBlock){pointer,bytes};
    ++lsp_json_live;lsp_json_bytes+=bytes;return pointer;
}
static bool lsp_json_fault(void) {
    if(lsp_json_calls++!=lsp_json_fail)return false;
    lsp_json_injected=true;return true;
}
static void *lsp_json_malloc(size_t bytes) {
    if(lsp_json_fault())return NULL;
    return lsp_json_track(xr_malloc(bytes),bytes);
}
static void *lsp_json_calloc(size_t count,size_t bytes) {
    if(lsp_json_fault())return NULL;
    CHECK(!count||bytes<=SIZE_MAX/count);
    return lsp_json_track(xr_calloc(count,bytes),count*bytes);
}
static void *lsp_json_realloc(void *pointer,size_t bytes) {
    if(lsp_json_fault())return NULL;
    CHECK(bytes);
    bool had_pointer=pointer!=NULL;size_t slot=had_pointer?lsp_json_slot(pointer):0;
    void *next=xr_realloc(pointer,bytes);if(!next)return NULL;
    if(!had_pointer)return lsp_json_track(next,bytes);
    lsp_json_bytes-=lsp_json_blocks[slot].bytes;lsp_json_bytes+=bytes;
    lsp_json_blocks[slot]=(LspJsonBlock){next,bytes};return next;
}
static char *lsp_json_strdup(const char *text) {
    if(lsp_json_fault())return NULL;
    /* Forward to the real inline helper, which calls one real allocator.
     * No duplicate allocation or surrogate JSON value is constructed here. */
    return lsp_json_track(xr_strdup(text),strlen(text)+1);
}
static void lsp_json_free(void *pointer) {
    if(!pointer)return;
    size_t i=lsp_json_slot(pointer);CHECK(lsp_json_live&&lsp_json_bytes>=lsp_json_blocks[i].bytes);
    --lsp_json_live;lsp_json_bytes-=lsp_json_blocks[i].bytes;lsp_json_blocks[i]=(LspJsonBlock){0};xr_free(pointer);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_calloc")
#pragma push_macro("xr_realloc")
#pragma push_macro("xr_free")
#pragma push_macro("xr_strdup")
#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#undef xr_strdup
#define xr_malloc(n) lsp_json_malloc(n)
#define xr_calloc(n,s) lsp_json_calloc(n,s)
#define xr_realloc(p,n) lsp_json_realloc(p,n)
#define xr_free(p) lsp_json_free(p)
#define xr_strdup(s) lsp_json_strdup(s)
#include "base/xjson.c"
#pragma pop_macro("xr_strdup")
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_realloc")
#pragma pop_macro("xr_calloc")
#pragma pop_macro("xr_malloc")
