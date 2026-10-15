/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_comptime_limits.c - Actual Source result limits and receiving
 */
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1); } } while(0)
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_ctfe.h"
#include "xir/xxir_tuple.h"
#include "xir/xxir_array.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_types.h"
static const XrCompileResourceLimits limit_resources={67108864,8388608,128000000};
typedef struct LimitCase { const char *name,*text;uint64_t logical;unsigned kind; } LimitCase;
static const LimitCase cases[]={
    {"pair","fn pair()->(i64,i64){return (2,3)}\nexport fn run()->(i64,i64){return comptime pair()}",16,0},
    {"array","fn make()->Array<i8>{return [1,2,3,4,5,6,7,8,9]}\nexport fn run()->Array<i8>{return comptime make()}",9,1},
    {"fibonacci","fn make()->Array<i8>{var xs:Array<i8>=[0,1];var i=2;while(i<10){xs.push(xs[i-1]+xs[i-2]);i=i+1};return xs}\nexport fn run()->Array<i8>{return comptime make()}",10,2},
    {"empty","export fn run()->string{return comptime \"\"}",0,3},
    {"unit","export fn run(){comptime {}}",0,4},
    {"scalar","export fn run()->i64{return comptime 42}",8,5},
    {"runtime","export fn run()->(i64,i64){return (2,3)}",0,0}
};
static XrCompileResourceStats limit_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats result={0};CHECK(xr_compile_resources_stats(context->resources,&result)==XR_COMPILE_RESOURCE_OK);return result;
}
static XrXirStatus limit_source(const XrXirCompileContext *context,const char *text,
    const XrXirSourceComptimeLimits *limits,XrXirSourceResult *output,XrXirSourceDiagnostic *diagnostic) {
    char bytes[1024],name[]="source-result-limits";size_t length=strlen(text);CHECK(length<sizeof(bytes));memcpy(bytes,text,length+1);
    XrCompilerSession *session=NULL;XrCompilerSessionStatus opened=xr_compile_session_new(context->resources,&session);
    if(opened!=XR_COMPILER_SESSION_OK)return opened==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:
        opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,name,NULL};
    XrXirSourceRequest request={.session=session,.authority=&authority,.context=context,
        .linkage_kind=XR_XIR_PROGRAM,.comptime_limits=limits};
    XrXirSourceText input={NULL,bytes,length};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,output,diagnostic,NULL);
    xr_compile_session_free(session);memset(bytes,0xa5,sizeof(bytes));memset(name,0xa5,sizeof(name));return status;
}
static void limit_value(const XrXirValue *value,unsigned kind) {
    if(kind==0) {
        const XrXirTypeArena *arena=xr_xir_value_arena(value);CHECK(arena);
        const XrXirTypeNode *tuple=xr_xir_tuple_signature(xr_xir_compile_type_arena_types(arena),(XrXirType)value->type);
        CHECK(tuple && tuple->parameter_count==2);
        for(uint32_t i=0;i<2;++i) {
            XrXirValue field={0};CHECK(tuple->parameters[i].type==XR_XIR_I64);
            CHECK(xr_xir_tuple_get(value,i,&field)==XR_XIR_VALUE_OK && field.type==XR_XIR_I64 && field.payload==(int64_t)i+2);
            xr_xir_value_drop(&field);
        }
    }else if(kind<=2) {
        const int64_t fibonacci[]={0,1,1,2,3,5,8,13,21,34};int64_t length=0;
        XrXirValueAdmission admission={.arena=xr_xir_value_arena(value),.work=UINT64_MAX,.scratch_bytes=65536};
        CHECK(xr_xir_array_len(value,&admission,&length)==XR_XIR_VALUE_OK && length==(kind==1?9:10));
        for(int64_t i=0;i<length;++i) {
            XrXirValue field={0};XrXirFaultDetail fault={0};CHECK(xr_xir_array_get(value,i,&admission,&field,&fault)==XR_XIR_VALUE_OK);
            CHECK(field.type==XR_XIR_I8 && field.payload==(kind==1?i+1:fibonacci[i]));xr_xir_value_drop(&field);
        }
    }else if(kind==3) {
        const char *text=NULL;size_t length=1;CHECK(value->type==XR_XIR_STRING && xr_xir_string_view(value,&text,&length) && !length);
    }else if(kind==4)CHECK(!value->type && !value->reserved && !value->payload);
    else CHECK(value->type==XR_XIR_I64 && !value->reserved && value->payload==42);
}
static void limit_consume(const XrXirCompileContext *context,XrXirSourceResult *source,unsigned kind) {
    const XrXirModule *module=xr_xir_compile_artifact_module(source->checked);uint32_t entry=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f)if(module->functions[f].name_length==3 && !memcmp(module->functions[f].name,"run",3)) {
        CHECK(entry==UINT32_MAX);entry=f;
        for(uint32_t i=0;i<module->functions[f].instruction_count;++i)
            CHECK(module->functions[f].instructions[i].op!=XR_XIR_CALL && module->functions[f].instructions[i].op!=XR_XIR_CALL_INDIRECT);
    }
    CHECK(entry!=UINT32_MAX);XrXirCheckedPacket packet={0};XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_write(source->checked,&packet,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(source);
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    module=xr_xir_compile_artifact_module(lowered);entry=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f)if(module->functions[f].name_length==3 && !memcmp(module->functions[f].name,"run",3))entry=f;
    CHECK(entry!=UINT32_MAX);XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,1024).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);limit_value(&value,kind);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
    limit_value(&value,kind);xr_xir_value_drop(&value);
}
static void limit_normal(const LimitCase *selected) {
    const uint64_t requests[]={0,7,8,9,10,15,16,8388608};
    for(unsigned i=0;i<=sizeof(requests)/sizeof(*requests);++i) {
        XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&limit_resources,&context.resources)==XR_COMPILE_RESOURCE_OK);
        context.limits=xr_xir_compile_default_limits();XrCompileResources *same=context.resources;XrCompileResourceStats baseline=limit_stats(&context);
        XrXirSourceComptimeLimits policy={i?requests[i-1]:0};XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
        bool allowed=!i || policy.output_bytes>=selected->logical;
        XrXirStatus status=limit_source(&context,selected->text,i?&policy:NULL,&source,&diagnostic);
        CHECK(status==(allowed?XR_XIR_OK:XR_XIR_BUDGET));
        CHECK(allowed?(source.checked && source.snapshot):(!source.checked && !source.snapshot && diagnostic.line>0));
        policy.output_bytes=UINT64_MAX;
        if(allowed)limit_consume(&context,&source,selected->kind);
        else {
            CHECK(limit_stats(&context).live_bytes==baseline.live_bytes);
            XrCompileResourceStats failed=limit_stats(&context);policy.output_bytes=selected->logical;
            CHECK(limit_source(&context,selected->text,&policy,&source,&diagnostic)==XR_XIR_OK);
            limit_consume(&context,&source,selected->kind);XrCompileResourceStats retried=limit_stats(&context);
            CHECK(retried.work>=failed.work && retried.allocated_bytes>=failed.allocated_bytes && context.resources==same);
        }
        CHECK(limit_stats(&context).live_bytes==baseline.live_bytes);xr_compile_resources_release(context.resources);
        CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
    }
    printf("Source %s logical=%llu: default/eight requests, exact/minus-one, same-owner retry, independent Checked/packet/VM and physical0\n",selected->name,(unsigned long long)selected->logical);
}
static void limit_full_resources(void) {
    for(unsigned rejected=0;rejected<2;++rejected) {
        XrXirSourceComptimeLimits policy={rejected?8:16};XrXirStatus expected=rejected?XR_XIR_BUDGET:XR_XIR_OK;
        XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&limit_resources,&context.resources)==XR_COMPILE_RESOURCE_OK);
        context.limits=xr_xir_compile_default_limits();XrCompileResourceStats baseline=limit_stats(&context);
        XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};size_t start=source_fixture_compile_attempts;
        CHECK(limit_source(&context,cases[0].text,&policy,&result,&diagnostic)==expected);
        size_t sites=source_fixture_compile_attempts-start;XrCompileResourceStats required=limit_stats(&context);
        xr_xir_compile_source_result_free(&result);CHECK(limit_stats(&context).live_bytes==baseline.live_bytes);
        xr_compile_resources_release(context.resources);CHECK(sites && !source_fixture_compile_live && !source_fixture_compile_bytes);
        for(size_t fault=0;fault<sites;++fault) {
            context=(XrXirCompileContext){0};CHECK(xr_compile_resources_new(&limit_resources,&context.resources)==XR_COMPILE_RESOURCE_OK);
            context.limits=xr_xir_compile_default_limits();baseline=limit_stats(&context);XrCompileResources *same=context.resources;
            result=(XrXirSourceResult){0};diagnostic=(XrXirSourceDiagnostic){0};start=source_fixture_compile_attempts;
            source_fixture_compile_injected=false;source_fixture_compile_fail_at=start+fault;
            XrXirStatus status=limit_source(&context,cases[0].text,&policy,&result,&diagnostic);
            source_fixture_compile_fail_at=SIZE_MAX;
            CHECK(status==XR_XIR_OUT_OF_MEMORY && source_fixture_compile_injected && source_fixture_compile_attempts>start+fault);
            CHECK(!result.checked && !result.snapshot && limit_stats(&context).live_bytes==baseline.live_bytes);
            XrCompileResourceStats failed=limit_stats(&context);start=source_fixture_compile_attempts;
            CHECK(limit_source(&context,cases[0].text,&policy,&result,&diagnostic)==expected);
            CHECK(source_fixture_compile_attempts-start==sites && context.resources==same);
            xr_xir_compile_source_result_free(&result);XrCompileResourceStats after=limit_stats(&context);
            CHECK(after.live_bytes==baseline.live_bytes && after.work>=failed.work && after.allocated_bytes>=failed.allocated_bytes);
            xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
        }
        if(!rejected)for(unsigned axis=0;axis<3;++axis)for(unsigned shortfall=0;shortfall<2;++shortfall) {
            XrCompileResourceLimits limits=limit_resources;
            if(axis==0)limits.allocated_bytes=required.allocated_bytes-shortfall;
            if(axis==1)limits.live_bytes=required.peak_bytes-shortfall;
            if(axis==2)limits.work=required.work-shortfall;
            context=(XrXirCompileContext){0};CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
            context.limits=xr_xir_compile_default_limits();baseline=limit_stats(&context);result=(XrXirSourceResult){0};
            CHECK(limit_source(&context,cases[0].text,&policy,&result,&diagnostic)==(shortfall?XR_XIR_BUDGET:XR_XIR_OK));
            CHECK(shortfall?(!result.checked && !result.snapshot):(result.checked && result.snapshot));
            xr_xir_compile_source_result_free(&result);CHECK(limit_stats(&context).live_bytes==baseline.live_bytes);
            xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
        }
        printf("Source output request%llu: complete%zu physical faults, same original owner retry, original three-axis controls, physical0\n",(unsigned long long)policy.output_bytes,sites);
    }
}
static void limit_invalid(void) {
    XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&limit_resources,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"invalid-source-limit",NULL};
    XrXirSourceComptimeLimits policy={XR_XIR_CTFE_MAX_OUTPUT_BYTES+1};XrXirSourceText text={NULL,cases[0].text,strlen(cases[0].text)};
    XrXirSourceRequest request={.session=session,.authority=&authority,.context=&context,.linkage_kind=XR_XIR_PROGRAM,.comptime_limits=&policy};
    XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};XrCompileResourceStats before=limit_stats(&context);
    CHECK(xr_xir_compile_source_check_text(&request,&text,&output,&diagnostic,NULL)==XR_XIR_BAD_STRUCTURE);
    XrCompileResourceStats after=limit_stats(&context);CHECK(!memcmp(&before,&after,sizeof(before)) && !output.checked && !output.snapshot);
    policy.output_bytes=16;CHECK(xr_xir_compile_source_check_text(&request,&text,&output,&diagnostic,NULL)==XR_XIR_OK);
    XrXirSourceResult occupied=output;before=limit_stats(&context);
    CHECK(xr_xir_compile_source_check_text(&request,&text,&output,&diagnostic,NULL)==XR_XIR_BAD_STRUCTURE);
    after=limit_stats(&context);CHECK(!memcmp(&before,&after,sizeof(before)) && !memcmp(&output,&occupied,sizeof(output)));
    xr_compile_session_free(session);xr_xir_compile_source_result_free(&output);xr_compile_resources_release(context.resources);
    CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
    puts("Source ceiling-plus-one and occupied output preserve original owner/stats/output without charges; physical0");
}
int main(int argc,char **argv) {
    CHECK(argc==2);
    if(!strcmp(argv[1],"--resources")){limit_full_resources();return 0;}
    if(!strcmp(argv[1],"invalid")){limit_invalid();return 0;}
    for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i)if(!strcmp(argv[1],cases[i].name)){limit_normal(&cases[i]);return 0;}
    CHECK(false);return 1;
}
