/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_task_result_native.c - Real generated TaskResult owners after producer retirement
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "base/xmalloc.h"
#include "task_result_bridge.h"
#if defined(__cplusplus)
extern "C" {
#endif
#include "xir/xxir_program.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_output.h"
#include "xir/xxir_task_result.h"
#include "xir/xxir_enum.h"
#include "xir/xxir_error.h"
#include "xir/xxir_nullable.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_types.h"
extern const XrXirProgramSpec source_artifact_program;
#if defined(__cplusplus)
}
#endif
#define CHECK(c) do {if(!(c)){fprintf(stderr,"native TaskResult %d: %s\n",__LINE__,#c);exit(1);}}while(0)
typedef struct Output {char bytes[8192];size_t length;} Output;
static XrXirOutputStatus capture(void *context,XrXirOutputStream stream,const char *bytes,size_t length) {
    Output *output=(Output *)context;CHECK(stream==XR_XIR_STDOUT && length<=sizeof(output->bytes)-output->length);
    memcpy(output->bytes+output->length,bytes,length);output->length+=length;return XR_XIR_OUTPUT_OK;
}
static uint32_t named(const XrXirModule *module,const char *name) {
    for(uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *body=&module->functions[f];
        if(module->declarations->functions[f].exported && body->name_length==strlen(name) &&
            !memcmp(body->name,name,body->name_length)) return f;
    }
    CHECK(0);return UINT32_MAX;
}
static XrXirValue execute(XrXirInstance *instance,uint32_t entry) {
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue result;memset(&result,0,sizeof(result));
    CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);return result;
}
static void exact_output(const Output *output,const char *path) {
    FILE *file=NULL;
#if defined(_MSC_VER)
    CHECK(!fopen_s(&file,path,"rb"));
#else
    file=fopen(path,"rb");
#endif
    CHECK(file);char expected[4096];size_t length=fread(expected,1,sizeof(expected)-1,file);
    CHECK(!ferror(file) && feof(file) && !fclose(file));expected[length]=0;
    const char *hex=strstr(expected,"\"stdoutHex\"");CHECK(hex);hex=strchr(hex,':');CHECK(hex);
    do {++hex;} while(*hex==' ' || *hex=='\t');CHECK(*hex++=='\"');size_t bytes=0;
    while(*hex!='\"') {
        const char *digits="0123456789abcdef",*high=strchr(digits,hex[0]),*low=hex[0]?strchr(digits,hex[1]):NULL;
        CHECK(high && low && *high && *low && bytes<output->length);
        CHECK((unsigned char)output->bytes[bytes++]==(unsigned char)((high-digits)*16+(low-digits)));hex+=2;
    }
    CHECK(bytes==output->length);
}
static void exact_error_text(const XrXirValue *error,const char *expected,size_t expected_length) {
    XrXirValue concrete={0};CHECK(error->type==XR_XIR_ERROR && xr_xir_error_borrow(error,&concrete));
    XrXirEnumBorrow payload={0};CHECK(xr_xir_enum_borrow(&concrete,&payload)==XR_XIR_VALUE_OK && payload.field_count==1);
    const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(&payload.fields[0],&bytes,&length) && length==expected_length && !memcmp(bytes,expected,length));
}
static void late_result(const XrXirValue *value,unsigned expected) {
    const XrXirTypeArena *arena=xr_xir_value_arena(value);
    const XrXirTypes *types=xr_xir_compile_type_arena_types(arena);XrXirTaskResultType shape;
    memset(&shape,0,sizeof(shape));CHECK(xr_xir_task_result_type_info(types,(XrXirType)value->type,&shape));
    uint32_t variant=UINT32_MAX;CHECK(xr_xir_enum_variant(value,&variant)==XR_XIR_VALUE_OK && variant==expected);
    XrXirEnumBorrow borrow;memset(&borrow,0,sizeof(borrow));
    CHECK(xr_xir_enum_borrow(value,&borrow)==XR_XIR_VALUE_OK && borrow.field_count==(expected==3?0u:1u));
    if(expected==XR_XIR_TASK_RESULT_CANCELLED)return;
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(UINT64_MAX,&domain)==XR_XIR_VALUE_OK);
    uint64_t baseline=xr_xir_domain_stats(domain).live_bytes;
    XrXirValueAdmission admission;memset(&admission,0,sizeof(admission));
    admission.domain=domain;admission.arena=arena;admission.work=UINT64_MAX;admission.scratch_bytes=UINT64_MAX;
    XrXirValue field;memset(&field,0,sizeof(field));CHECK(xr_xir_enum_get(value,expected,0,&admission,&field)==XR_XIR_VALUE_OK);
    if(expected==XR_XIR_TASK_RESULT_SUCCESS) {
        CHECK(xr_xir_value_argument(&field,arena,shape.element) || (shape.element==XR_XIR_UNIT && !field.type && !field.payload));
        if(shape.element==XR_XIR_I64)CHECK(field.payload==42);
    } else if(expected==XR_XIR_TASK_RESULT_ERROR) {
        XrXirValue concrete;memset(&concrete,0,sizeof(concrete));CHECK(field.type==XR_XIR_ERROR && xr_xir_error_borrow(&field,&concrete));
        CHECK(xr_xir_value_arena(&concrete)==arena && xr_xir_error_bound_accepts(types,(XrXirType)concrete.type,shape.error));
        XrXirEnumBorrow error;memset(&error,0,sizeof(error));CHECK(xr_xir_enum_borrow(&concrete,&error)==XR_XIR_VALUE_OK);
        CHECK(error.field_count==1 && error.fields[0].type==XR_XIR_STRING);
        const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&error.fields[0],&bytes,&length) && length && memchr(bytes,0,length));
    } else {
        XrXirFaultDetail detail;memset(&detail,0,sizeof(detail));CHECK(field.type==XR_XIR_PANIC_INFO && xr_xir_panic_info_detail(&field,&detail));
        CHECK(detail.code==446);XrXirValue message;memset(&message,0,sizeof(message));
        CHECK(xr_xir_panic_info_message(&field,&message)==XR_XIR_VALUE_OK);
        const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&message,&bytes,&length) && length>=13 && !memcmp(bytes,"try! failed: ",13));
        static const char primary[]="panic \303\251\0\347\225\214";bool found=false;
        for(size_t i=0;i<=length && sizeof(primary)-1<=length-i;++i)found|=!memcmp(bytes+i,primary,sizeof(primary)-1);
        CHECK(found);
        xr_xir_value_drop(&message);
        XrXirType optional_error=XR_XIR_UNIT;
        for(uint32_t i=0;i<types->count;++i)
            if(types->nodes[i].kind==XR_XIR_TYPE_NULLABLE && types->nodes[i].element==XR_XIR_ERROR)
                optional_error=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+i);
        CHECK(optional_error!=XR_XIR_UNIT);XrXirValue replaced={0},owned={0};
        CHECK(xr_xir_panic_info_replaced_error(&field,optional_error,&admission,&replaced)==XR_XIR_VALUE_OK);
        bool some=false;const XrXirValue *borrowed=NULL;
        CHECK(xr_xir_nullable_view(&replaced,&some,&borrowed) && some && borrowed);
        CHECK(xr_xir_value_copy(borrowed,&owned)==XR_XIR_VALUE_OK);xr_xir_value_drop(&replaced);
        static const char prior[]="replaced \303\251\0\347\225\214";
        exact_error_text(&owned,prior,sizeof(prior)-1);xr_xir_value_drop(&owned);
    }
    xr_xir_value_drop(&field);CHECK(xr_xir_domain_stats(domain).live_bytes==baseline);xr_xir_domain_drop(domain);
}
struct TestTaskResultOwner {XrCompileResources *resources;uint64_t baseline;};
void test_task_result_produce(const char *const names[4],const char *expected,
    XrXirValue copies[4],const XrXirTypes **types,TestTaskResultOwner **owner) {
    CHECK(names && expected && copies && types && owner && !*owner);uint32_t entries[4];
    XrCompileResourceLimits limits;memset(&limits,0,sizeof(limits));limits.memory_mode=XR_COMPILE_MEMORY_ORDINARY;
    XrXirCompileContext context;memset(&context,0,sizeof(context));context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline;memset(&baseline,0,sizeof(baseline));
    CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrXirArtifact *metadata=NULL;CHECK(xr_xir_compile_checked_read(&context,source_artifact_program.packet.bytes,
        source_artifact_program.packet.length,&metadata,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(metadata);
    for(unsigned i=0;i<4;++i)entries[i]=named(module,names[i]);xr_xir_compile_artifact_free(metadata);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&source_artifact_program,&program)==XR_XIR_OK);
    Output output;memset(&output,0,sizeof(output));XrXirOutputSink sink;memset(&sink,0,sizeof(sink));
    sink.abi_version=XR_XIR_CALL_ABI_VERSION;sink.write=capture;sink.context=&output;sink.byte_limit=sizeof(output.bytes);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output.abi_version=XR_XIR_CALL_ABI_VERSION;config.output.write=xr_xir_output_render;config.output.context=&sink;
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
    XrXirValue main_result=execute(instance,source_artifact_program.declarations->entry_function);xr_xir_value_drop(&main_result);exact_output(&output,expected);
    XrXirValue values[4];memset(values,0,sizeof(values));
    for(unsigned i=0;i<4;++i)CHECK(!copies[i].type && !copies[i].reserved && !copies[i].payload);
    for(unsigned i=0;i<4;++i){values[i]=execute(instance,entries[i]);CHECK(xr_xir_value_copy(&values[i],&copies[i])==XR_XIR_VALUE_OK);xr_xir_value_drop(&values[i]);}
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    for(unsigned i=0;i<4;++i)late_result(&copies[i],i);
    TestTaskResultOwner *audit=(TestTaskResultOwner *)xr_malloc(sizeof(*audit));CHECK(audit);
    audit->resources=context.resources;audit->baseline=baseline.held_bytes;
    *types=xr_xir_compile_type_arena_types(xr_xir_value_arena(&copies[0]));*owner=audit;
}
void test_task_result_observe(const XrXirValue values[4]) {
    for(unsigned i=0;i<4;++i)late_result(&values[i],i);
}
void test_task_result_finish(TestTaskResultOwner *owner) {
    CHECK(owner);XrCompileResourceStats stats;memset(&stats,0,sizeof(stats));
    CHECK(xr_compile_resources_stats(owner->resources,&stats)==XR_COMPILE_RESOURCE_OK && stats.held_bytes==owner->baseline);
    xr_compile_resources_release(owner->resources);xr_free(owner);
}
#if !defined(XR_TASK_RESULT_BRIDGE)
static XrXirInstanceResult step(XrXirInstance *instance) {
    XrXirInstanceResult event=xr_xir_instance_poll_bounded(instance,64);
    if(event.outcome.status==XR_XIR_CALL_SUSPENDED) {
        XrXirWaitRequest wait={0};
        CHECK(xr_xir_instance_wait_request(instance,event.epoch,event.outcome.wake,&wait)==XR_XIR_CALL_READY && wait.kind==XR_XIR_WAIT_YIELD);
        CHECK(xr_xir_instance_resume(instance,event.epoch,event.outcome.wake)==XR_XIR_CALL_READY);
    }
    return event;
}
static void terminal_boundary(const char *name,XrXirCallStatus expected) {
    XrCompileResourceLimits limits={0};limits.memory_mode=XR_COMPILE_MEMORY_ORDINARY;
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline={0},final={0};CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrXirArtifact *metadata=NULL;
    CHECK(xr_xir_compile_checked_read(&context,source_artifact_program.packet.bytes,source_artifact_program.packet.length,&metadata,NULL)==XR_XIR_OK);
    uint32_t entry=named(xr_xir_compile_artifact_module(metadata),name);xr_xir_compile_artifact_free(metadata);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&source_artifact_program,&program)==XR_XIR_OK);
    Output output={0};XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,capture,&output,sizeof(output.bytes)};
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sink};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    XrXirValue initialized=execute(instance,source_artifact_program.declarations->entry_function);xr_xir_value_drop(&initialized);output.length=0;
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult event={0};
    if(expected==XR_XIR_CALL_CANCELLED) {
        for(unsigned i=0;i<100;++i) {
            event=step(instance);
            CHECK(event.outcome.status==XR_XIR_CALL_READY || event.outcome.status==XR_XIR_CALL_SUSPENDED);
        }
        CHECK(!output.length && xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED);
    }
    unsigned count=0;
    do {
        if(++count==10000)fprintf(stderr,"terminal %s stalled status%u output%.*s\n",name,event.outcome.status,(int)output.length,output.bytes);
        CHECK(count<10000);event=step(instance);
    }
    while(event.outcome.status==XR_XIR_CALL_READY || event.outcome.status==XR_XIR_CALL_SUSPENDED);
    CHECK(event.outcome.status==expected && xr_xir_call_result_valid(&event.outcome));
    XrXirValue no_result={0};
    CHECK(xr_xir_instance_take_result(instance,&no_result)==(expected==XR_XIR_CALL_CANCELLED?XR_XIR_CALL_BAD_STATE:expected));
    CHECK(!no_result.type && !no_result.reserved && !no_result.payload);
    XrXirCallResult owned={0};CHECK(xr_xir_call_result_copy(&event.outcome,&owned)==XR_XIR_VALUE_OK);
    XrXirCallStatus closed=xr_xir_instance_free(instance);
    CHECK(closed==(expected==XR_XIR_CALL_CANCELLED?XR_XIR_CALL_READY:expected));xr_xir_compile_program_drop(program);
    CHECK(xr_xir_call_result_valid(&owned) && !owned.value.type && !owned.value.payload);
    if(expected==XR_XIR_CALL_CLEANUP_FAILED) {
        static const char text[]="fatal-started\nfatal-cleanup\n";
        CHECK(output.length==sizeof(text)-1 && !memcmp(output.bytes,text,output.length));
        CHECK(owned.panic.detail.code==420 && owned.prior_status==XR_XIR_CALL_CANCELLED);
    } else if(expected==XR_XIR_CALL_CANCELLED) {
        static const char text[]="parent-cleanup\nchild-cleanup\n";
        CHECK(output.length==sizeof(text)-1 && !memcmp(output.bytes,text,output.length));
    } else CHECK(!output.length);
    xr_xir_call_result_drop(&owned);
    CHECK(xr_compile_resources_stats(context.resources,&final)==XR_COMPILE_RESOURCE_OK && final.held_bytes==baseline.held_bytes);
    xr_compile_resources_release(context.resources);
}
int main(int argc,char **argv) {
    CHECK(argc==6);XrXirValue values[4];memset(values,0,sizeof(values));const XrXirTypes *types=NULL;TestTaskResultOwner *owner=NULL;
    const char *names[4]={argv[1],argv[2],argv[3],argv[4]};test_task_result_produce(names,argv[5],values,&types,&owner);CHECK(types);
    test_task_result_observe(values);for(unsigned i=0;i<4;++i)xr_xir_value_drop(&values[i]);test_task_result_finish(owner);
    terminal_boundary("unsafeConstruct",XR_XIR_CALL_BAD_STATE);
    terminal_boundary("ownAwaitResult",XR_XIR_CALL_CANCELLED);
    terminal_boundary("fatalResult",XR_XIR_CALL_CLEANUP_FAILED);
    puts("TaskResult generated C: four owned variants, exact full output and retired producers PASS");return 0;
}
#endif
