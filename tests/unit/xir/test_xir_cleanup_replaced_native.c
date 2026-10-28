/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_cleanup_replaced_native.c - Real generated C cleanup ownership
 */
#include "xir/xxir_program_internal.h"
#include "xir/xxir_output.h"
#include "xir/xxir_enum.h"
#include "xir/xxir_error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if XR_OS_WINDOWS
#include <fcntl.h>
#include <io.h>
#endif
#define CHECK(c) do {if(!(c)){fprintf(stderr,"native cleanup %d: %s\n",__LINE__,#c);exit(1);}}while(0)
extern const XrXirProgramSpec source_artifact_program;
typedef struct Output {char bytes[4096];size_t size;XrXirOutputSink sink;} Output;
static XrXirOutputStatus bytes(void *context,XrXirOutputStream stream,const char *text,size_t length) {
    Output *out=context;CHECK(stream==XR_XIR_STDOUT && length<=sizeof(out->bytes)-out->size);
    memcpy(out->bytes+out->size,text,length);out->size+=length;return XR_XIR_OUTPUT_OK;
}
static XrXirOutputStatus group(void *context,const XrXirOutputGroup *value) {
    return xr_xir_output_render(&((Output *)context)->sink,value);
}
static uint32_t named(const XrXirProgram *program,const char *name) {
    const XrXirModule *module=xr_xir_compile_artifact_module(program->permissions->input->artifact);
    for(uint32_t i=0;i<module->function_count;++i) if(module->functions[i].name_length==strlen(name) &&
        !memcmp(module->functions[i].name,name,strlen(name))) return i;
    CHECK(false);return UINT32_MAX;
}
static XrXirCallResult execute(XrXirProgram *program,uint32_t entry,Output *output,bool cancel) {
    output->sink=(XrXirOutputSink){XR_XIR_CALL_ABI_VERSION,0,bytes,output,sizeof(output->bytes)};
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,group,output};XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirCallResult observed={0};
    if(cancel) {
        size_t polls=0;
        do {CHECK(++polls<100000);observed=xr_xir_instance_poll_bounded(instance,1).outcome;CHECK(observed.status==XR_XIR_CALL_READY);}
        while(output->size<6 || memcmp(output->bytes+output->size-6,"armed\n",6));
        CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED);
    }
    do observed=xr_xir_instance_poll_bounded(instance,256).outcome;while(observed.status==XR_XIR_CALL_READY);
    XrXirCallResult owner={0};CHECK(xr_xir_call_result_copy(&observed,&owner)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_instance_free(instance)==(owner.status==XR_XIR_CALL_CLEANUP_FAILED?XR_XIR_CALL_CLEANUP_FAILED:XR_XIR_CALL_READY));
    return owner;
}
static void error(const XrXirValue *value,const char *text,size_t length,int64_t number) {
    XrXirValue concrete={0};CHECK(xr_xir_error_borrow(value,&concrete));uint32_t tag=UINT32_MAX;
    CHECK(xr_xir_enum_variant(&concrete,&tag)==XR_XIR_VALUE_OK && tag==0);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1024*1024,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={xr_xir_value_arena(value),domain,NULL,NULL,100000,100000};XrXirValue field={0};
    CHECK(xr_xir_enum_get(&concrete,0,0,&admission,&field)==XR_XIR_VALUE_OK);
    const char *actual=NULL;size_t size=0;CHECK(xr_xir_string_view(&field,&actual,&size) && size==length && !memcmp(actual,text,length));
    xr_xir_value_drop(&field);CHECK(xr_xir_enum_get(&concrete,0,1,&admission,&field)==XR_XIR_VALUE_OK && field.payload==number);
    xr_xir_value_drop(&field);xr_xir_domain_drop(domain);
}
int main(void) {
#if XR_OS_WINDOWS
    CHECK(_setmode(_fileno(stdout),_O_BINARY)!=-1);
#endif
    XrCompileResourceLimits limits={.memory_mode=XR_COMPILE_MEMORY_ORDINARY,.profile_allocations=true};
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);XrCompileResourceStats initial;
    CHECK(xr_compile_resources_stats(context.resources,&initial)==XR_COMPILE_RESOURCE_OK);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&source_artifact_program,&program)==XR_XIR_OK);
    Output primary={0};XrXirCallResult result=execute(program,source_artifact_program.declarations->entry_function,&primary,false);
    static const char expected[]="true\ntrue\n420\ntrue\n420\n42\ntrue\n446\n";
    CHECK(result.status==XR_XIR_CALL_RETURNED && primary.size==sizeof(expected)-1 && !memcmp(primary.bytes,expected,primary.size));
    xr_xir_call_result_drop(&result);Output out={0};
    XrXirCallResult single=execute(program,named(program,"single446"),&out,false);out=(Output){0};
    XrXirCallResult fatal=execute(program,named(program,"fatal446"),&out,false);out=(Output){0};
    XrXirCallResult task=execute(program,named(program,"fatalTask"),&out,false);out=(Output){0};
    XrXirCallResult cancelled=execute(program,named(program,"cancellable"),&out,true);out=(Output){0};
    CHECK(single.status==XR_XIR_CALL_RUNTIME_PANIC && single.panic.detail.code==446);
    CHECK(fatal.status==XR_XIR_CALL_CLEANUP_FAILED && fatal.panic.detail.code==446 && fatal.prior_panic.detail.code==446);
    CHECK(task.status==XR_XIR_CALL_CLEANUP_FAILED && task.panic.detail.code==421 && task.prior_panic.detail.code==420);
    CHECK(cancelled.status==XR_XIR_CALL_CLEANUP_FAILED && cancelled.prior_status==XR_XIR_CALL_CANCELLED && xr_xir_panic_empty(&cancelled.prior_panic));
    result=execute(program,named(program,"afterFailure"),&out,false);CHECK(result.status==XR_XIR_CALL_RETURNED && result.value.payload==42);
    xr_xir_call_result_drop(&result);xr_xir_compile_program_drop(program);
    static const char old[]="old é\0界",next[]="new é\0界",prior[]="prior é\0界";
    error(&single.panic.error,next,sizeof(next)-1,31);error(&single.panic.replaced_error,old,sizeof(old)-1,23);
    error(&fatal.panic.error,next,sizeof(next)-1,31);error(&fatal.prior_panic.error,prior,sizeof(prior)-1,47);
    XrXirCallResult copy={0},moved={0};CHECK(xr_xir_call_result_copy(&fatal,&copy)==XR_XIR_VALUE_OK);
    xr_xir_call_result_move(&copy,&moved);CHECK(xr_xir_call_result_empty(&copy));
    xr_xir_call_result_drop(&fatal);error(&moved.prior_panic.error,prior,sizeof(prior)-1,47);
    xr_xir_call_result_drop(&single);xr_xir_call_result_drop(&moved);xr_xir_call_result_drop(&task);xr_xir_call_result_drop(&cancelled);
    XrCompileResourceStats final;CHECK(xr_compile_resources_stats(context.resources,&final)==XR_COMPILE_RESOURCE_OK && final.held_bytes==initial.held_bytes);
    xr_compile_resources_release(context.resources);CHECK(fwrite(primary.bytes,1,primary.size,stdout)==primary.size);
    return 0;
}
