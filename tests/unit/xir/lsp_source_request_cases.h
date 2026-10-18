/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef LSP_SOURCE_REQUEST_CASES_H
#define LSP_SOURCE_REQUEST_CASES_H
#include "app/lsp/xlsp_source_request.h"
static void request_facts(XrJsonValue *json,const char *root_uri,const char *lib_uri,unsigned mode) {
    CHECK(json);
    if(mode==0) {
        CHECK(!strcmp(xjson_get_string(json,"uri"),lib_uri));
        XrJsonValue *range=xjson_get_object(json,"range"),*start=xjson_get_object(range,"start"),*end=xjson_get_object(range,"end");
        CHECK(xjson_get_int(start,"line")==0&&xjson_get_int(start,"character")==19&&xjson_get_int(end,"character")==24);
    } else if(mode==1) {
        CHECK(json->type==XR_JSON_ARRAY&&json->as.array.count==1);
        CHECK(!strcmp(xjson_get_string(json->as.array.items[0],"uri"),root_uri));
    } else if(mode==2) {
        CHECK(json->type==XR_JSON_ARRAY&&json->as.array.count==1);
        CHECK(xjson_get_int(json->as.array.items[0],"kind")==2);
    } else {
        XrJsonValue *contents=xjson_get_object(json,"contents");
        CHECK(contents&&!strcmp(xjson_get_string(contents,"kind"),"plaintext"));
        CHECK(strstr(xjson_get_string(contents,"value"),"value")&&strstr(xjson_get_string(contents,"value"),"i64"));
    }
}
static XrXirStatus request_once(const char *root_uri,const char *lib_uri,unsigned mode,bool cancel,
    XrCompileResourceLimits limits,XrCompileResourceStats *stats) {
    XrCompileResources *resources=NULL;XlspSourceRequest *request=NULL;XlspSourceRequestResult result={0};bool caller_lease=true;
    XrCompileResourceStatus created=xr_compile_resources_new(&limits,&resources);
    if(created!=XR_COMPILE_RESOURCE_OK)return created==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    char root[sizeof(open_root)],lib[sizeof(open_library)];memcpy(root,open_root,sizeof(root));memcpy(lib,open_library,sizeof(lib));
    char *first=xr_strdup(root_uri),*second=xr_strdup(lib_uri);CHECK(first&&second);
    XlspSourceDocument documents[2]={{first,root,strlen(first),sizeof(root)-1,1},{second,lib,strlen(second),sizeof(lib)-1,7}};
    XlspSourceRequestInput input={&context,documents,2,0,{1,43},mode,false};
    XrXirStatus status=xlsp_source_request_new(&input,&request);
    memset(first,'?',strlen(first));memset(second,'?',strlen(second));xr_free(first);xr_free(second);
    memset(root,'?',sizeof(root));memset(lib,'?',sizeof(lib));memset(documents,0,sizeof(documents));memset(&input,0,sizeof(input));
    if(status!=XR_XIR_OK){CHECK(!request);goto done;}
    XlspSourceRequest *occupied=request;size_t attempts=runtime_attempts;
    CHECK(xlsp_source_request_new(&input,&occupied)==XR_XIR_BAD_STRUCTURE&&occupied==request&&attempts==runtime_attempts);
    xr_compile_resources_release(resources);caller_lease=false;memset(&context,0,sizeof(context));
    _Atomic bool token;atomic_init(&token,cancel);
    XrJsonValue marker={0};XlspSourceRequestResult occupied_result={0};occupied_result.json=&marker;
    attempts=runtime_attempts;size_t json_before=lsp_json_calls;
    CHECK(xlsp_source_request_run(request,&token,&occupied_result)==XR_XIR_BAD_STRUCTURE&&
        occupied_result.json==&marker&&attempts==runtime_attempts&&json_before==lsp_json_calls);
    lsp_json_calls=0;
    CHECK(xlsp_source_request_run(request,&token,&result)==XR_XIR_OK);status=result.status;
    if(cancel){CHECK(result.cancelled&&!result.json&&result.stage==0&&status==XR_XIR_OK);}
    else if(status==XR_XIR_OK){CHECK(!result.stage&&!result.cancelled);request_facts(result.json,root_uri,lib_uri,mode);}
    else CHECK(!result.json);
    XlspSourceRequestResult repeated={0};attempts=runtime_attempts;
    CHECK(xlsp_source_request_run(request,&token,&repeated)==XR_XIR_BAD_STRUCTURE&&attempts==runtime_attempts&&!repeated.json);
done:
    if(stats)*stats=sdk_stats(resources);
    if(caller_lease)xr_compile_resources_release(resources);xlsp_source_request_free(request);CHECK(!runtime_live&&!runtime_bytes);
    if(result.json)request_facts(result.json,root_uri,lib_uri,mode);
    xjson_free(result.json);CHECK(!lsp_json_live&&!lsp_json_bytes);return status;
}
static void source_request_cases(const char *root_uri,const char *lib_uri,bool faults) {
    XrCompileResourceLimits limits=xr_source_compile_default_resource_limits();
    for(unsigned mode=0;mode<4;++mode) {
        size_t sites=0;XrCompileResourceStats stats={0};
        for(size_t probe=0;probe<=sites;++probe) {
            runtime_attempts=0;runtime_fail_at=probe?probe-1:SIZE_MAX;
            XrXirStatus status=request_once(root_uri,lib_uri,mode,false,limits,probe?NULL:&stats);
            if(!probe){CHECK(status==XR_XIR_OK);if(faults)sites=runtime_attempts;}
            else CHECK(status==XR_XIR_OUT_OF_MEMORY&&runtime_attempts>=probe);
        }
        runtime_fail_at=SIZE_MAX;
        if(faults) {
            size_t json_sites=0;
            for(size_t probe=0;probe<=json_sites;++probe) {
                lsp_json_fail=probe?probe-1:SIZE_MAX;lsp_json_injected=false;
                XrXirStatus status=request_once(root_uri,lib_uri,mode,false,limits,NULL);
                if(!probe){CHECK(status==XR_XIR_OK);json_sites=lsp_json_calls;CHECK(json_sites);}
                else CHECK(status==XR_XIR_OUT_OF_MEMORY&&lsp_json_injected);
            }
            lsp_json_fail=SIZE_MAX;lsp_json_injected=false;
            printf("LSP owned Source request mode=%u actual JSON sites=%zu, atomic output, physical=0/0\n",mode,json_sites);
        }
        if(faults)for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
            XrCompileResourceLimits exact={stats.allocated_bytes,stats.peak_bytes,stats.work};
            uint64_t *n=axis==0?&exact.allocated_bytes:axis==1?&exact.live_bytes:&exact.work;CHECK(*n&&*n<INT64_MAX);
            *n=(uint64_t)((int64_t)*n+delta);
            CHECK(request_once(root_uri,lib_uri,mode,false,exact,NULL)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));
        }
        CHECK(request_once(root_uri,lib_uri,mode,true,limits,NULL)==XR_XIR_OK);
        printf("LSP owned Source request mode=%u sites=%zu, one-shot/cancel/producer death, physical=0/0\n",mode,sites);
    }
}
#endif // LSP_SOURCE_REQUEST_CASES_H
