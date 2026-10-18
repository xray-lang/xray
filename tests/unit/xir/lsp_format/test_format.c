/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_source_format.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_sdk_resource_test.h"
#include "lsp_open_json_owner.h"

static const char emoji_source[]="fn value()->string{return \"\xf0\x9f\x98\x80\"}";
static const char *const expected[]={"fn value() -> string {\n    return \"\xf0\x9f\x98\x80\"\n}\n",
    "fn value() -> string {\n  return \"\xf0\x9f\x98\x80\"\n}\n",
    "fn value() -> string {\n\treturn \"\xf0\x9f\x98\x80\"\n}\n",
    "fn value() -> i64 {\n    return 1\n}\n"};
static const XrCompileResourceLimits limits={UINT64_C(1073741824),UINT64_C(268435456),UINT64_C(8589934592)};
static void facts(XrJsonValue *result,unsigned mode) {
    CHECK(result&&result->type==XR_JSON_ARRAY&&result->as.array.count==((mode<4||mode==7)?1:0));if(mode>=4&&mode!=7)return;
    XrJsonValue *edit=xjson_array_get(result,0),*range=xjson_get_object(edit,"range"),*start=xjson_get_object(range,"start"),*end=xjson_get_object(range,"end");
    CHECK(start&&end&&xjson_get_int(start,"line")==0&&xjson_get_int(start,"character")==0);
    CHECK(xjson_get_int(end,"line")== ((mode==3||mode==7)?3:0)&&xjson_get_int(end,"character")== ((mode==3||mode==7)?0:31));
    XrJsonValue *text=xjson_get(edit,"newText");CHECK(text&&text->type==XR_JSON_STRING&&text->string_len==strlen(expected[mode==7?3:mode])&&!memcmp(text->as.string,expected[mode==7?3:mode],text->string_len));
}
static XrXirStatus once(unsigned mode,XrCompileResourceLimits caps,XrCompileResourceStats *stats,size_t fault,bool *injected) {
    XrCompileResources *resources=NULL;XrJsonValue *result=NULL,*again=NULL;
    XrCompileResourceStatus made=xr_compile_resources_new(&caps,&resources);
    if(made!=XR_COMPILE_RESOURCE_OK)return made==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    const char *literal=mode<3?emoji_source:mode==3?"fn value()->i64{\r\nreturn 1\r\n}\r\n":mode==4?expected[0]:mode==5?"":mode==7?"fn value()->i64{\rreturn 1\r}\r":"fn broken(";
    char text[256],uri[]="file:///format.xr";size_t length=strlen(literal);CHECK(length<sizeof(text));memcpy(text,literal,length+1);
    XlspSourceDocument input={uri,text,sizeof(uri)-1,length,7};XrFmtConfig config=xfmt_default_config;
    if(mode==1)config.indent_size=2;if(mode==2)config.use_tabs=1;
    lsp_json_calls=0;lsp_json_fail=fault;lsp_json_injected=false;
    XrXirStatus status=xlsp_source_format_json(resources,&input,&config,&result);if(stats&&status!=XR_XIR_OK)fprintf(stderr,"Formatting diagnostic mode=%u status=%u\n",mode,(unsigned)status);memset(text,'?',sizeof(text));memset(uri,'?',sizeof(uri));
    if(status==XR_XIR_OK) {
        facts(result,mode);XrJsonValue *occupied=result;size_t before=runtime_attempts,json_before=lsp_json_calls;
        CHECK(xlsp_source_format_json(resources,&input,&config,&occupied)==XR_XIR_BAD_STRUCTURE&&occupied==result&&runtime_attempts==before&&lsp_json_calls==json_before);
        if(mode<4||mode==7) {
            XrJsonValue *formatted=xjson_get(xjson_array_get(result,0),"newText");
            XlspSourceDocument next={"file:///format.xr",formatted->as.string,17,formatted->string_len,8};
            status=xlsp_source_format_json(resources,&next,&config,&again);
            if(status==XR_XIR_OK)CHECK(again&&again->as.array.count==0);else CHECK(!again);
        }
    } else CHECK(!result);
    if(injected)*injected=lsp_json_injected;lsp_json_fail=SIZE_MAX;lsp_json_injected=false;
    if(stats)*stats=sdk_stats(resources);xr_compile_resources_release(resources);CHECK(!runtime_live&&!runtime_bytes);
    if(status==XR_XIR_OK)facts(result,mode);xjson_free(result);xjson_free(again);CHECK(!lsp_json_live&&!lsp_json_bytes);return status;
}
static void invalid_inputs(void) {
    XrCompileResources *resources=sdk_ledger(&limits);XrJsonValue *result=NULL;XrFmtConfig config=xfmt_default_config;
    XlspSourceDocument input={"file:///format.xr","\x80",17,1,1};
    CHECK(xlsp_source_format_json(resources,&input,&config,&result)==XR_XIR_BAD_STRUCTURE&&!result);
    input.text="\0";CHECK(xlsp_source_format_json(resources,&input,&config,&result)==XR_XIR_BAD_STRUCTURE&&!result);
    input.text="";input.length=0;config.indent_size=-1;size_t before=runtime_attempts;
    CHECK(xlsp_source_format_json(resources,&input,&config,&result)==XR_XIR_BAD_STRUCTURE&&!result&&runtime_attempts==before);
    config=xfmt_default_config;input.uri="file:\0bad";input.uri_length=9;
    CHECK(xlsp_source_format_json(resources,&input,&config,&result)==XR_XIR_BAD_STRUCTURE&&!result);
    xr_compile_resources_release(resources);CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes);
}
int main(int argc,char **argv) {
    (void)sdk_fixture_malloc;(void)sdk_fixture_free;(void)sdk_unlimited;
    CHECK(argc==1||(argc==2&&!strcmp(argv[1],"--normal")));bool full=argc==1;
    for(unsigned mode=0;mode<8;++mode) {
        XrCompileResourceStats stats={0};runtime_attempts=0;runtime_fail_at=SIZE_MAX;
        CHECK(once(mode,limits,&stats,SIZE_MAX,NULL)==XR_XIR_OK);size_t sites=runtime_attempts,json_sites=lsp_json_calls;CHECK(sites&&json_sites);
        if(full) {
            for(size_t i=0;i<sites;++i){runtime_attempts=0;runtime_fail_at=i;CHECK(once(mode,limits,NULL,SIZE_MAX,NULL)==XR_XIR_OUT_OF_MEMORY&&runtime_attempts>i);}
            runtime_fail_at=SIZE_MAX;
            for(size_t i=0;i<json_sites;++i){bool injected=false;CHECK(once(mode,limits,NULL,i,&injected)==XR_XIR_OUT_OF_MEMORY&&injected);}
            for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
                XrCompileResourceLimits exact={stats.allocated_bytes,stats.peak_bytes,stats.work};uint64_t *n=axis==0?&exact.allocated_bytes:axis==1?&exact.live_bytes:&exact.work;
                CHECK(*n&&*n<INT64_MAX);*n=(uint64_t)((int64_t)*n+delta);CHECK(once(mode,exact,NULL,SIZE_MAX,NULL)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));
            }
        }
        printf("Source formatting mode=%u SourceFI=%zu JSONFI=%zu full=%u actualUTF16EOF/idempotence/producer death physical=0/0\n",mode,sites,json_sites,full?1u:0u);
    }
    invalid_inputs();return 0;
}
