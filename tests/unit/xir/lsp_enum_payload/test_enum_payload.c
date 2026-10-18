/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "app/lsp/xlsp_server.h"
#include "app/lsp/xlsp_source_syntax.h"
#include "app/lsp/xlsp_completion.h"
#include "app/lsp/xlsp_index_json.h"
#include "app/lsp/xlsp_utils.h"
#include "frontend/parser/xast_nodes.h"
#include "frontend/parser/xtype_ref.h"
#include "base/xutf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"enum-payload:%d:%s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_sdk_resource_test.h"
#include "lsp_open_json_owner.h"
#include "enum_strategy_actual.inc.c"
/* The response wrapper receives the actual enum strategy through a unit
 * adapter. The full completion strategy dispatcher and server are not run. */
static XrLspDocument *selected_document;
XR_FUNC XrJsonValue *xlsp_analyze_completion(XrLspServer *server,XrLspDocument *doc,XrLspPosition position) {
    XrXirStatus outcome=XR_XIR_OK;
    return complete_enum_decl(server,doc,position,"Result",&outcome);
}
XR_FUNC XrLspDocument *xlsp_document_get(XrLspServer *server,const char *uri) {
    return selected_document&&selected_document->server==server&&uri&&!strcmp(uri,selected_document->uri)?selected_document:NULL;
}
#include "enum_response_actual.inc.c"
#include "test_lsp_rename_params.h"
XR_FUNC void lsp_log(const char *format,...) {(void)format;}
static const char constructor_text[]=
    "enum Result { Ok { value:i64, code:Array<i64> }, Err { value:string } }\nResult.\n";
static const char pattern_text[]=
    "enum Result { Ok { value:i64, code:Array<i64> }, Err { value:string } }\n"
    "var selected=match(made){\n Result.\n}\n";
static XrJsonValue *named(XrJsonValue *array,const char *label) {
    XrJsonValue *found=NULL;
    for(int i=0;i<array->as.array.count;++i){XrJsonValue *item=xjson_array_get(array,i);const char *name=xjson_get_string(item,"label");
        if(name&&!strcmp(name,label)){CHECK(!found);found=item;}}
    CHECK(found);return found;
}
static void facts(XrJsonValue *array,unsigned mode) {
    CHECK(array&&array->type==XR_JSON_ARRAY&&array->as.array.count==3);
    XrJsonValue *ok=named(array,"Ok"),*err=named(array,"Err");
    CHECK(!strcmp(xjson_get_string(ok,"detail"),"Ok { value: i64, code: Array<i64> }"));
    CHECK(!strcmp(xjson_get_string(err,"detail"),"Err { value: string }"));
    CHECK(!strcmp(xjson_get_string(ok,"insertText"),mode==1?"Ok { value, code }":"Ok { value: ${1}, code: ${2} }"));
    if(mode==1)CHECK(!xjson_get(ok,"insertTextFormat"));else CHECK(xjson_get_int(ok,"insertTextFormat")==2);
    CHECK(!strcmp(xjson_get_string(xjson_get_object(ok,"documentation"),"value"),"```xray\nResult.Ok\n```"));
    CHECK(!strcmp(xjson_get_string(named(array,"variants"),"detail"),"Result.variants: EnumVariants<Result>"));
}
static XrXirStatus once(unsigned mode,XrCompileResourceLimits limits,XrCompileResourceStats *stats,size_t *sites) {
    bool pattern=mode==1||mode==6;const char *literal=pattern?pattern_text:constructor_text;size_t length=strlen(literal);
    char *input=sdk_fixture_malloc(length+1);CHECK(input);memcpy(input,literal,length+1);
    XrCompileResources *resources=NULL;XrCompilerSession *session=NULL;XlspSyntaxSnapshot *syntax=NULL;XrJsonValue *result=NULL,*params=NULL;
    size_t requested_json_fail=lsp_json_fail;lsp_json_fail=SIZE_MAX;
    XrCompileResourceStatus made=xr_compile_resources_new(&limits,&resources);
    XrXirStatus status=made==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:made==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrCompileState *original=NULL;
    if(status==XR_XIR_OK){XrCompilerSessionStatus opened=xr_compile_session_new(resources,&session);status=opened==XR_COMPILER_SESSION_OK?XR_XIR_OK:opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;}
    if(status==XR_XIR_OK) {
        original=xr_compile_session_compile_state(session);
        XlspSourceDocument document={"file:///D:/enum-payload.xr",input,strlen("file:///D:/enum-payload.xr"),length,7};
        XrParseStatus parsed=xlsp_source_syntax_build(session,&document,&syntax);
        status=parsed==XR_PARSE_OK||parsed==XR_PARSE_RECOVERED?XR_XIR_OK:parsed==XR_PARSE_BUDGET?XR_XIR_BUDGET:parsed==XR_PARSE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
    }
    xr_compile_session_free(session);memset(input,'?',length);sdk_fixture_free(input);
    if(status==XR_XIR_OK) {
        CHECK(xlsp_source_syntax_state(syntax)==original);
        uint32_t offsets[16]={0};int count=1;for(size_t i=0;i<length;++i)if(literal[i]=='\n'){CHECK(count<16);offsets[count++]=(uint32_t)i+1;}
        XrLspServer server={0};XrLspDocument document={0};document.server=&server;document.content=(char *)literal;document.length=length;
        document.uri="file:///D:/enum-payload.xr";
        document.line_offsets=offsets;document.line_count=count;document.syntax_snapshot=syntax;document.ast=(AstNode *)xlsp_source_syntax_ast(syntax);
        if(mode==2)document.ast=(AstNode *)(uintptr_t)1;
        if(mode==3)CHECK(xr_compile_state_fail(original,XR_COMPILE_RESOURCE_BUDGET)==XR_COMPILE_RESOURCE_BUDGET);
        if(mode==4)document.syntax_snapshot=NULL;
        XrLspPosition position={pattern?2u:1u,pattern?8u:7u};
        if(mode>=5){params=rename_test_params(document.uri,position,NULL);CHECK(params);selected_document=&document;}
        lsp_json_calls=0;lsp_json_fail=requested_json_fail;
        if(mode>=5){if(mode==7)server.config.completion_max_items=2;result=xlsp_handle_td_completion(&server,params);status=server.source_failure.stage?(XrXirStatus)server.source_failure.status:XR_XIR_OK;selected_document=NULL;}
        else result=complete_enum_decl(&server,&document,position,"Result",&status);
        if(status!=XR_XIR_OK){CHECK(!result&&server.source_failure.stage==24&&server.source_failure.status==(unsigned)status);}
        else CHECK(result&&!server.source_failure.stage);
        memset(&document,0xdd,sizeof(document));memset(&server,0xdd,sizeof(server));
    }
    if(stats&&resources)*stats=sdk_stats(resources);
    if(sites)*sites=runtime_attempts;
    xjson_free(params);lsp_json_fail=requested_json_fail;
    xlsp_source_syntax_free(syntax);xr_compile_resources_release(resources);CHECK(!runtime_live&&!runtime_bytes);
    /* The result is independently owned after syntax/state/Session/input death. */
    if(result) {
        if(mode>=5) {
            CHECK(result->type==XR_JSON_OBJECT&&result->as.object.count==2);
            CHECK(xjson_get_bool(result,"isIncomplete")== (mode==7));
            XrJsonValue *items=xjson_get(result,"items");
            if(mode==7){CHECK(items&&items->as.array.count==2);CHECK(!strcmp(xjson_get_string(named(items,"Ok"),"detail"),"Ok { value: i64, code: Array<i64> }"));}
            else facts(items,pattern?1:0);
        } else facts(result,mode);
    }
    xjson_free(result);CHECK(!lsp_json_live&&!lsp_json_bytes);return status;
}
static void matrix(unsigned mode) {
    XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    XrCompileResourceStats required={0};size_t source=0;runtime_attempts=0;runtime_fail_at=SIZE_MAX;lsp_json_calls=0;
    CHECK(once(mode,limits,&required,&source)==XR_XIR_OK);size_t json=lsp_json_calls;CHECK(source&&json);
    for(size_t i=0;i<source;++i){runtime_attempts=0;runtime_fail_at=i;CHECK(once(mode,limits,NULL,NULL)==XR_XIR_OUT_OF_MEMORY&&runtime_attempts>i);}
    runtime_fail_at=SIZE_MAX;
    for(size_t i=0;i<json;++i){lsp_json_calls=0;lsp_json_fail=i;lsp_json_injected=false;CHECK(once(mode,limits,NULL,NULL)==XR_XIR_OUT_OF_MEMORY&&lsp_json_injected);}
    lsp_json_fail=SIZE_MAX;
    for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta){
        XrCompileResourceLimits bounds={required.allocated_bytes,required.peak_bytes,required.work};uint64_t *value=axis==0?&bounds.allocated_bytes:axis==1?&bounds.live_bytes:&bounds.work;
        CHECK(*value&&*value<UINT64_MAX);*value=(uint64_t)((int64_t)*value+delta);
        CHECK(once(mode,bounds,NULL,NULL)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));
    }
    printf("ENUM_PAYLOAD mode%u Source%zu JSON%zu shared_original_state=1 producer_death=1 axes=9 physical0/0\n",mode,source,json);
}
int main(void) {
    (void)sdk_unlimited;(void)sdk_ledger;matrix(0);matrix(1);matrix(5);matrix(6);
    XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    CHECK(once(2,limits,NULL,NULL)==XR_XIR_BAD_STRUCTURE);CHECK(once(3,limits,NULL,NULL)==XR_XIR_BUDGET);CHECK(once(4,limits,NULL,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(once(7,limits,NULL,NULL)==XR_XIR_OK);
    puts("Enum strategy: original state, payload syntax, complete JSON, foreign AST reject, sticky budget and ownership verified; full completion/server process not run");return 0;
}
