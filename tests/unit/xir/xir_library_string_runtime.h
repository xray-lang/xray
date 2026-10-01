/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_string_runtime.h - Owned imported strings across producer lifetimes
 *
 * KEY CONCEPT:
 *   Independent byte expectations survive Program destruction on every backend.
 */
#ifndef XIR_LIBRARY_STRING_RUNTIME_H
#define XIR_LIBRARY_STRING_RUNTIME_H
#include "xir/xxir.h"
XR_FUNC size_t *xr_test_library_string_runtime_counter(unsigned index);
XR_FUNC void xr_test_library_string_run(XrXirArtifact *owned);
#ifndef XR_LIBRARY_STRING_RUNTIME_IMPLEMENTATION
#define runtime_attempts (*xr_test_library_string_runtime_counter(0))
#define runtime_fail_at (*xr_test_library_string_runtime_counter(1))
#define runtime_live (*xr_test_library_string_runtime_counter(2))
#define runtime_bytes (*xr_test_library_string_runtime_counter(3))
#else
#define LIBRARY_STRING_EXPORTS 10
static const char *const library_string_names[LIBRARY_STRING_EXPORTS]={
    "left","right","explicitLeft","explicitRight","rootLiteral","empty","longText","embeddedNul","wireLong","wireEmpty"};
static void library_string_expect(const XrXirValue *value,uint32_t entry) {
    const char *bytes=NULL;size_t size=0;CHECK(value->type==XR_XIR_STRING);
    CHECK(xr_xir_string_view(value,&bytes,&size));
    const char *expected[]={"\xe7\x94\xb2-owned!","\xe4\xb9\x99-owned?","explicit!","explicit?","root-owned",""};
    const size_t lengths[]={10,10,9,9,10,0};
    if(entry<6){CHECK(size==lengths[entry]);CHECK(!size||!memcmp(bytes,expected[entry],size));}
    else if(entry==6){CHECK(size==703&&!memcmp(bytes,"\xe9\x95\xbf",3));for(size_t i=3;i<size;++i)CHECK(bytes[i]=='a');}
    else if(entry==8){CHECK(size==703);for(size_t i=0;i<699;i+=3)CHECK(!memcmp(bytes+i,"\xe6\xb1\x89",3));CHECK(!memcmp(bytes+699,"tail",4));}
    else if(entry==9){CHECK(!size);}
    else {static const char nul[]={'a',0,'b'};CHECK(entry==7&&size==sizeof(nul)&&!memcmp(bytes,nul,sizeof(nul)));}
}
#if CONSUMER_KIND==0 || CONSUMER_KIND==3
static void library_string_ids(const XrXirModule *module,uint32_t ids[LIBRARY_STRING_EXPORTS]) {
    for(uint32_t e=0;e<LIBRARY_STRING_EXPORTS;++e){ids[e]=UINT32_MAX;
        for(uint32_t f=0;f<module->function_count;++f){const XrXirFunction *fn=&module->functions[f];
            if(module->declarations->functions[f].module==module->declarations->root_module && fn->name_length==strlen(library_string_names[e])&&!memcmp(fn->name,library_string_names[e],fn->name_length)){
                CHECK(ids[e]==UINT32_MAX&&module->declarations->functions[f].exported);ids[e]=f;}}
        CHECK(ids[e]!=UINT32_MAX);
    }
}
#endif
static void library_string_drop(XrXirValue held[2][LIBRARY_STRING_EXPORTS]) {
    for(uint32_t i=0;i<2;++i)for(uint32_t e=0;e<LIBRARY_STRING_EXPORTS;++e)xr_xir_value_drop(&held[i][e]);
}
static bool library_string_pair(XrXirProgram *program,const uint32_t ids[LIBRARY_STRING_EXPORTS],
    XrXirValue held[2][LIBRARY_STRING_EXPORTS]) {
    for(uint32_t i=0;i<2;++i){XrXirInstance *instance=NULL;XrXirInstanceConfig config=xr_xir_instance_defaults();
        XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
        for(uint32_t e=0;e<LIBRARY_STRING_EXPORTS&&status!=XR_XIR_CALL_OOM;++e){
            CHECK(status==XR_XIR_CALL_READY||status==XR_XIR_CALL_RETURNED);
            status=xr_xir_instance_start(instance,ids[e],NULL,0);
            if(status==XR_XIR_CALL_READY)status=xr_xir_instance_poll(instance).outcome.status;
            if(status==XR_XIR_CALL_RETURNED){status=xr_xir_instance_take_result(instance,&held[i][e]);
                if(status==XR_XIR_CALL_RETURNED)library_string_expect(&held[i][e],e);}
            CHECK(status==XR_XIR_CALL_RETURNED||status==XR_XIR_CALL_OOM);
        }
        if(instance)CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
        if(status==XR_XIR_CALL_OOM)return false;
    }
    return true;
}
static void library_string_seal_faults(const XrXirProgramSpec *spec) {
    size_t live=runtime_live,bytes=runtime_bytes,sites=0;
    for(size_t p=0;p<=sites;++p){runtime_attempts=0;runtime_fail_at=p?p-1:SIZE_MAX;XrXirProgram *program=NULL;
        XrXirStatus status=xr_xir_program_seal(spec,(XrXirProgramBudget){1048576,1048576},&program);
        if(!p){CHECK(status==XR_XIR_OK&&program);sites=runtime_attempts;CHECK(sites);xr_xir_program_drop(program);}
        else CHECK(status==XR_XIR_OUT_OF_MEMORY&&!program);
        CHECK(runtime_live==live&&runtime_bytes==bytes);
    }
    runtime_fail_at=SIZE_MAX;printf("Library strings seal OOM=%zu physical baseline restored\n",sites);
}
static void library_string_runtime_faults(XrXirProgram *program,const uint32_t ids[LIBRARY_STRING_EXPORTS]) {
    size_t live=runtime_live,bytes=runtime_bytes,sites=0;
    for(size_t p=0;p<=sites;++p){XrXirValue held[2][LIBRARY_STRING_EXPORTS]={0};runtime_attempts=0;runtime_fail_at=p?p-1:SIZE_MAX;
        bool ok=library_string_pair(program,ids,held);
        if(!p){CHECK(ok);sites=runtime_attempts;CHECK(sites);}else CHECK(!ok);
        library_string_drop(held);CHECK(runtime_live==live&&runtime_bytes==bytes);
    }
    runtime_fail_at=SIZE_MAX;printf("Library strings runtime OOM=%zu physical baseline restored\n",sites);
}
static void library_string_finish(XrXirProgramSpec *spec,XrXirArtifact *lowered,
    const uint32_t ids[LIBRARY_STRING_EXPORTS]) {
    library_string_seal_faults(spec);XrXirProgram *program=NULL;
    CHECK(xr_xir_program_seal(spec,(XrXirProgramBudget){1048576,1048576},&program)==XR_XIR_OK);
    library_string_runtime_faults(program,ids);XrXirValue held[2][LIBRARY_STRING_EXPORTS]={0};
    CHECK(library_string_pair(program,ids,held));xr_xir_program_drop(program);xr_xir_artifact_free(lowered);
    runtime_attempts=0;runtime_fail_at=0;
    for(uint32_t i=0;i<2;++i)for(uint32_t e=0;e<LIBRARY_STRING_EXPORTS;++e)library_string_expect(&held[i][e],e);
    library_string_drop(held);CHECK(!runtime_attempts&&!runtime_live&&!runtime_bytes);runtime_fail_at=SIZE_MAX;
    printf("Library strings retained-reader allocation sites=0 failat0; twoInstances/10exports/703UTF8/NUL3/physicalzero\n");
}
#endif
#endif // XIR_LIBRARY_STRING_RUNTIME_H
