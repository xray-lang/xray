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
static void lsp_json_location_facts(XrJsonValue *location,const char *uri,uint32_t row,uint32_t begin,uint32_t end) {
    CHECK(location&&location->type==XR_JSON_OBJECT&&location->as.object.count==2);
    CHECK(!strcmp(xjson_get_string(location,"uri"),uri));
    XrJsonValue *range=xjson_get_object(location,"range");CHECK(range);
    XrJsonValue *start=xjson_get_object(range,"start"),*finish=xjson_get_object(range,"end");CHECK(start&&finish);
    CHECK(xjson_get_int(start,"line")==(int64_t)row&&xjson_get_int(finish,"line")==(int64_t)row);
    CHECK(xjson_get_int(start,"character")==(int64_t)begin&&xjson_get_int(finish,"character")==(int64_t)end);
}
static XrXirStatus lsp_json_run(const XlspSourceSnapshot *snapshot,const char *root_uri,const char *library_uri) {
    XrJsonValue *output=NULL;
    XrXirStatus status=xlsp_source_definition_json(snapshot,root_uri,strlen(root_uri),(XrLspPosition){1,43},&output);
    if(status!=XR_XIR_OK) {CHECK(!output);return status;}
    lsp_json_location_facts(output,library_uri,0,19,24);xjson_free(output);output=NULL;
    status=xlsp_source_references_json(snapshot,library_uri,strlen(library_uri),(XrLspPosition){0,20},true,&output);
    if(status!=XR_XIR_OK) {CHECK(!output);return status;}
    CHECK(output->type==XR_JSON_ARRAY&&output->as.array.count==2);
    lsp_json_location_facts(output->as.array.items[0],library_uri,0,19,24);
    lsp_json_location_facts(output->as.array.items[1],root_uri,1,42,47);
    xjson_free(output);return XR_XIR_OK;
}
static void lsp_json_cases(const char *root) {
    XrCompileResourceLimits limits=lsp_limits();XrXirCompileContext context={0};XrCompilerSession *session=NULL;
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILER_SESSION_OK);
    char root_uri[XR_TEST_PATH_MAX],library_uri[XR_TEST_PATH_MAX];
    lsp_uri(root_uri,sizeof(root_uri),root,"root.xr");lsp_uri(library_uri,sizeof(library_uri),root,"lib.xr");
    XlspSourceDocument documents[2]={
        {root_uri,source_root_text,strlen(root_uri),sizeof(source_root_text)-1,1},
        {library_uri,source_library_text,strlen(library_uri),sizeof(source_library_text)-1,7}};
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    XrXirSourceRequest request={session,NULL,&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XlspSourceSnapshot *snapshot=NULL;CHECK(xlsp_source_snapshot_build(&request,documents,2,0,&snapshot,NULL,NULL)==XR_XIR_OK);
    xr_compile_session_free(session);xr_compile_resources_release(context.resources);
    size_t sites=0;
    for(size_t probe=0;probe<=sites;++probe) {
        CHECK(!lsp_json_live&&!lsp_json_bytes);lsp_json_calls=0;lsp_json_fail=probe?probe-1:SIZE_MAX;lsp_json_injected=false;
        XrXirStatus status=lsp_json_run(snapshot,root_uri,library_uri);
        if(!probe){CHECK(status==XR_XIR_OK);sites=lsp_json_calls;CHECK(sites);}
        else CHECK(status==XR_XIR_OUT_OF_MEMORY&&lsp_json_injected&&lsp_json_calls==probe);
        CHECK(!lsp_json_live&&!lsp_json_bytes);
    }
    lsp_json_fail=SIZE_MAX;lsp_json_injected=false;
    XrJsonValue *occupied=xjson_new_number(73),*saved=occupied;CHECK(occupied);size_t before=lsp_json_calls;
    CHECK(xlsp_source_definition_json(snapshot,root_uri,strlen(root_uri),(XrLspPosition){1,43},&occupied)==XR_XIR_BAD_STRUCTURE&&occupied==saved);
    CHECK(xlsp_source_references_json(snapshot,root_uri,strlen(root_uri),(XrLspPosition){1,43},false,&occupied)==XR_XIR_BAD_STRUCTURE&&occupied==saved);
    CHECK(before==lsp_json_calls&&occupied->as.number==73);xjson_free(occupied);
    XrJsonValue *output=NULL;
    CHECK(xlsp_source_definition_json(snapshot,root_uri,strlen(root_uri),(XrLspPosition){1,30},&output)==XR_XIR_BAD_STRUCTURE&&!output);
    CHECK(xlsp_source_definition_json(snapshot,root_uri,strlen(root_uri),(XrLspPosition){1,43},&output)==XR_XIR_OK);
    xlsp_source_snapshot_free(snapshot);lsp_zero();
    /* JSON has its own URI/string ownership after every Source owner has died. */
    lsp_json_location_facts(output,library_uri,0,19,24);xjson_free(output);
    CHECK(!lsp_json_live&&!lsp_json_bytes);
    printf("LSP real xjson FI sites=%zu, occupied-output and producer death; both owners physical=0/0\n",sites);
}
