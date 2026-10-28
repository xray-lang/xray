/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * sdk_cleanup_consumer.c - Installed cleanup result ownership in C and C++
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"SDK cleanup %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#if defined(XR_CLEANUP_READER) || defined(XR_CLEANUP_SOURCE_SDK)
static FILE *input_file(const char *path) {
    FILE *file=NULL;
#if defined(_MSC_VER)
    if(fopen_s(&file,path,"rb"))return NULL;
#else
    file=fopen(path,"rb");
#endif
    return file;
}
#endif
#if defined(XR_CLEANUP_READER)
#include "toolchain/xr_xir_runtime_sdk.h"
int main(int argc,char **argv) {
    CHECK(argc==4 && !strcmp(argv[3],"status"));FILE *file=input_file(argv[2]);CHECK(file && !fseek(file,0,SEEK_END));
    long length=ftell(file);CHECK(length>0 && !fseek(file,0,SEEK_SET));char *bytes=(char *)malloc((size_t)length);
    CHECK(bytes && fread(bytes,1,(size_t)length,file)==(size_t)length && !fclose(file));
    XrCompileResourceLimits limits={.memory_mode=XR_COMPILE_MEMORY_ORDINARY};XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirRuntimeSdkRequest request={argv[1],bytes,(size_t)length,resources};XrXirRuntimeSdk *sdk=NULL;
    XrXirRuntimeSdkStatus status=xr_xir_runtime_sdk_load(&request,&sdk);free(bytes);xr_compile_resources_release(resources);
    printf("%u",(unsigned)status);
    if(sdk){const XrXirRuntimeSdkFacts *facts=xr_xir_runtime_sdk_facts(sdk);CHECK(facts && facts->call_abi==37 && facts->semantic==105);
        printf(" %u ",facts->file_count);for(size_t i=0;i<32;++i)printf("%02x",facts->identity[i]);}
    puts("");xr_xir_runtime_sdk_free(sdk);return 0;
}
#elif defined(XR_CLEANUP_SOURCE_SDK)
#include "xray_source_entry.h"
typedef struct Trace {char bytes[8192];size_t size;} Trace;
static XrSourceWriteStatus write_bytes(void *context,XrSourceOutputStream stream,const char *bytes,size_t length) {
    Trace *trace=(Trace *)context;(void)stream;CHECK(length<=sizeof(trace->bytes)-trace->size);
    memcpy(trace->bytes+trace->size,bytes,length);trace->size+=length;return XR_SOURCE_WRITE_OK;
}
static int contains(const Trace *trace,const char *bytes,size_t length) {
    for(size_t i=0;length<=trace->size-i;++i)if(!memcmp(trace->bytes+i,bytes,length))return 1;
    return 0;
}
static XrSourceResult *invoke(XrSourceExecution *owner,XrSourceArtifact *artifact,const char *name,Trace *trace) {
    XrSourceInvocation request;CHECK(xray_source_invocation_init(&request,sizeof(request))==XR_SOURCE_OK);
    request.name=name;request.name_length=strlen(name);XrSourceCallOptions options;
    CHECK(xray_source_call_options_init(&options,sizeof(options))==XR_SOURCE_OK);options.write=write_bytes;options.context=trace;
    XrSourceResult *result=NULL;XrSourceEvent failure;
    CHECK(xray_source_invoke(owner,artifact,&request,&options,&result,&failure)==XR_SOURCE_OK && result);return result;
}
int main(int argc,char **argv) {
    CHECK(argc==3);FILE *file=input_file(argv[1]);CHECK(file && !fseek(file,0,SEEK_END));long length=ftell(file);
    CHECK(length>0 && !fseek(file,0,SEEK_SET));char *text=(char *)malloc((size_t)length+1);CHECK(text);
    CHECK(fread(text,1,(size_t)length,file)==(size_t)length && !fclose(file));text[length]=0;
    XrSourceInput input;memset(&input,0,sizeof(input));input.kind=XR_SOURCE_INPUT_TEXT;
    input.value.text.namespace_id="cold-cleanup";input.value.text.bytes=text;input.value.text.length=(size_t)length;
    XrSourceCompileOptions compiler=xray_source_compile_options_default();compiler.stdlib_path=argv[2];
    XrSourceArtifact *artifact=NULL;XrSourceFailure failure;XrSourceDiagnostic *diagnostic=NULL;
    CHECK(xray_source_build(&input,&compiler,&artifact,&failure,&diagnostic)==XR_SOURCE_OK && artifact);
    xray_source_diagnostic_free(diagnostic);memset(text,0xa5,(size_t)length);free(text);
    XrSourceExecutionOptions execution_options=xray_source_execution_options_default();XrSourceExecution *owner=NULL;
    CHECK(xray_source_execution_new(&execution_options,&owner)==XR_SOURCE_OK);Trace output;memset(&output,0,sizeof(output));
    XrSourceResult *single=invoke(owner,artifact,"single446",&output);XrSourceEvent event;
    CHECK(xray_source_result_event(single,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_PANICKED);
    memset(&output,0,sizeof(output));XrSourceResult *fatal=invoke(owner,artifact,"fatal446",&output),*copy=NULL;
    CHECK(xray_source_result_event(fatal,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_CLEANUP_FAILED);
    CHECK(xray_source_result_copy(fatal,&copy)==XR_SOURCE_OK);xray_source_result_free(fatal);
    memset(&output,0,sizeof(output));XrSourceResult *next=invoke(owner,artifact,"afterFailure",&output);int64_t number=0;
    CHECK(xray_source_result_i64(next,&number)==XR_SOURCE_OK && number==42);xray_source_result_free(next);
    xray_source_artifact_free(artifact);xray_source_execution_free(owner);
    static const char old[]="old é\0界",fresh[]="new é\0界",prior[]="prior é\0界";
    memset(&output,0,sizeof(output));CHECK(xray_source_result_format(single,write_bytes,&output)==XR_SOURCE_OK);
    CHECK(contains(&output,old,sizeof(old)-1) && contains(&output,fresh,sizeof(fresh)-1) && contains(&output,"[Replaced Error]",16));
    memset(&output,0,sizeof(output));CHECK(xray_source_result_format(copy,write_bytes,&output)==XR_SOURCE_OK);
    CHECK(contains(&output,fresh,sizeof(fresh)-1) && contains(&output,prior,sizeof(prior)-1) && contains(&output,"[Fatal Cleanup]",15));
    xray_source_result_free(single);xray_source_result_free(copy);
    puts("Installed Source cleanup: dual owned Error, fatal prior and retired producers PASS");return 0;
}
#else
#if defined(__cplusplus)
extern "C" {
#endif
#include "xir/xxir_call.h"
#if defined(__cplusplus)
static_assert(XR_XIR_CALL_ABI_VERSION==37 && sizeof(XrXirPanicPayload)==72 && sizeof(XrXirCallResult)==184,"Call37 physical layout");
#else
_Static_assert(XR_XIR_CALL_ABI_VERSION==37 && sizeof(XrXirPanicPayload)==72 && sizeof(XrXirCallResult)==184,"Call37 physical layout");
#endif
typedef struct Context {XrXirValue message;unsigned releases;} Context;
static XrXirAction root_resume(XrXirCallView *view) {
    unsigned *state=(unsigned *)view->state;XrXirAction action;memset(&action,0,sizeof(action));
    if(view->phase==XR_XIR_CALL_EXIT) {
        if(*state==1){*state=2;action.kind=XR_XIR_ACTION_CALL;action.callee=1;action.flags=XR_XIR_ACTION_CLEANUP;return action;}
        CHECK(xr_xir_call_cleanup_frontier(view,0)==XR_XIR_CALL_READY);action.kind=XR_XIR_ACTION_EXIT_DONE;return action;
    }
    *state=1;CHECK(xr_xir_call_cleanup_frontier(view,1)==XR_XIR_CALL_READY);return xr_xir_call_numeric_fault(XR_XIR_RUN_DIVIDE_BY_ZERO,false);
}
static XrXirAction cleanup_resume(XrXirCallView *view) {return xr_xir_call_assertion(&((Context *)view->environment)->message);}
static void release_frame(XrXirCallView *view,XrXirCallStatus reason) {(void)reason;++((Context *)view->environment)->releases;}
#if defined(__cplusplus)
}
#endif
int main(void) {
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1024*1024,&domain)==XR_XIR_VALUE_OK);Context context;memset(&context,0,sizeof(context));
    static const char text[]="cleanup é\0界";CHECK(xr_xir_string_new(domain,text,sizeof(text)-1,&context.message)==XR_XIR_VALUE_OK);
    XrXirCallEntry entries[2];memset(entries,0,sizeof(entries));
    for(unsigned i=0;i<2;++i){entries[i].abi_version=37;entries[i].resume=i?cleanup_resume:root_resume;entries[i].release=release_frame;entries[i].environment=&context;}
    entries[0].state_bytes=sizeof(unsigned);entries[0].result=XR_XIR_I64;entries[0].flags=XR_XIR_ENTRY_EXIT;entries[1].cleanup_owner=1;
    XrXirCallAccounting accounting;memset(&accounting,0,sizeof(accounting));
    XrXirCallConfig config;CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);config.entries=entries;config.entry_count=2;config.accounting=&accounting;
    config.admission.domain=domain;config.admission.work=100000;config.admission.scratch_bytes=100000;
    XrXirCall *call=NULL;config.abi_version=36;CHECK(xr_xir_call_new(&config,0,NULL,0,&call)==XR_XIR_CALL_BAD_ABI && !call);config.abi_version=37;
    CHECK(xr_xir_call_new(&config,0,NULL,0,&call)==XR_XIR_CALL_READY);XrXirCallResult result;memset(&result,0,sizeof(result));
    XrXirCallResult observed;do{observed=xr_xir_call_poll_bounded(call,1);}while(observed.status==XR_XIR_CALL_READY);
    CHECK(observed.status==XR_XIR_CALL_CLEANUP_FAILED && xr_xir_call_result_copy(&observed,&result)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY && context.releases==2);xr_xir_value_drop(&context.message);xr_xir_domain_drop(domain);
    CHECK(result.panic.detail.code==445 && result.prior_status==XR_XIR_CALL_DIVIDE_BY_ZERO && result.prior_panic.detail.code==420);
    XrXirCallResult copied,moved;memset(&copied,0,sizeof(copied));memset(&moved,0,sizeof(moved));
    CHECK(xr_xir_call_result_copy(&result,&copied)==XR_XIR_VALUE_OK);xr_xir_call_result_move(&copied,&moved);CHECK(xr_xir_call_result_empty(&copied));
    xr_xir_call_result_drop(&result);const char *actual=NULL;size_t length=0;
    CHECK(xr_xir_string_view(&moved.panic.message,&actual,&length) && length==sizeof(text)-1 && !memcmp(actual,text,length));
    xr_xir_call_result_drop(&moved);puts("Installed Runtime Call37: owned fatal, moved result, retired Domain and old ABI refusal PASS");return 0;
}
#endif
