/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_derived_constants.c - Ordinary constant table recipes and receiving
 *
 * KEY CONCEPT:
 *   Compiler-owned Tuple results become ordinary checked recipes. Descriptions
 *   preserve logical field order without granting execution or member access.
 */
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1); } } while (0)
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_ctfe.h"
#include "xir/xxir_type_description.h"
#include "xir/xxir_types.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_tuple.h"
#include "xir/xxir_vm.h"

static const XrCompileResourceLimits constant_limits={67108864,8388608,128000000};
static const char constant_table_source[]=
    "fn primes()->(i16,i16,i16,i16){return (2,3,5,7)}\n"
    "export fn table()->(i16,i16,i16,i16){return comptime primes()}\n"
    "export fn checksum()->i64{const (a,b,c,d)=table();return (a as i64)+(b as i64)*10+(c as i64)*100+(d as i64)*1000}\n";
static XrXirStatus constant_source(const XrXirCompileContext *context,const char *text,
    XrXirSourceResult *output,XrXirSourceDiagnostic *diagnostic) {
    char bytes[1024],name[]="derived-constant-table";size_t length=strlen(text);
    CHECK(length<sizeof(bytes));memcpy(bytes,text,length+1);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,name,NULL};
    XrCompilerSession *session=NULL;
    XrCompilerSessionStatus opened=xr_compile_session_new(context->resources,&session);
    if(opened!=XR_COMPILER_SESSION_OK) {
        CHECK(!session);
        return opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:
            opened==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
    }
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={NULL,bytes,length};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,output,diagnostic,NULL);
    xr_compile_session_free(session);memset(bytes,0xa5,sizeof(bytes));memset(name,0xa5,sizeof(name));
    return status;
}
static uint32_t constant_function(const XrXirModule *module,const char *name) {
    size_t length=strlen(name);uint32_t found=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *body=&module->functions[f];
        if(body->name_length==length && !memcmp(body->name,name,length)) {
            CHECK(found==UINT32_MAX);found=f;
        }
    }
    CHECK(found!=UINT32_MAX);return found;
}
static void constant_recipe(const XrXirCompileContext *context,const XrXirModule *module) {
    const XrXirFunction *body=&module->functions[constant_function(module,"table")];
    XrXirTypeDescription description={0};
    CHECK(xr_xir_compile_type_description(context,module->types,body->result,&description)==XR_XIR_OK);
    CHECK(description.kind==XR_XIR_TYPE_TUPLE && description.field_count==4);
    for(uint32_t f=0;f<description.field_count;++f) {
        XrXirFieldDescription field={0};
        CHECK(xr_xir_compile_description_field(context,&description,f,&field)==XR_XIR_OK);
        CHECK(field.resolved && field.type==XR_XIR_I16 && field.template_type==XR_XIR_I16 && !field.flags);
    }
    unsigned tuple=0,constants=0;const int64_t expected[]={2,3,5,7};
    for(uint32_t i=0;i<body->instruction_count;++i) {
        const XrXirInstruction *op=&body->instructions[i];
        CHECK(op->op!=XR_XIR_CALL && op->op!=XR_XIR_CALL_INDIRECT && op->op!=XR_XIR_FUNCTION_REF);
        tuple+=op->op==XR_XIR_TUPLE_NEW;
        if(op->op==XR_XIR_CONST_INT && op->type==XR_XIR_I16) {
            CHECK(constants<4 && op->immediate==expected[constants]);++constants;
        }
    }
    CHECK(tuple==1 && constants==4);
}
static void constant_value(const XrXirValue *value) {
    const int64_t expected[]={2,3,5,7};
    const XrXirTypeArena *arena=xr_xir_value_arena(value);CHECK(arena);
    const XrXirTypeNode *node=xr_xir_tuple_signature(xr_xir_compile_type_arena_types(arena),(XrXirType)value->type);
    CHECK(node && node->parameter_count==4);
    for(uint32_t f=0;f<4;++f) {
        XrXirValue field={0};CHECK(node->parameters[f].type==XR_XIR_I16);
        CHECK(xr_xir_tuple_get(value,f,&field)==XR_XIR_VALUE_OK);
        CHECK(field.type==XR_XIR_I16 && !field.reserved && field.payload==expected[f]);xr_xir_value_drop(&field);
    }
}
static void constant_table(void) {
    XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&constant_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=constant_source(&context,constant_table_source,&source,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"constant table Source status=%u at %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked && source.snapshot);
    constant_recipe(&context,xr_xir_compile_artifact_module(source.checked));
    XrXirCheckedPacket packet={0};XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_artifact_verify(received,NULL)==XR_XIR_OK);
    constant_recipe(&context,xr_xir_compile_artifact_module(received));
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    uint32_t checksum_entry=constant_function(module,"checksum"),table_entry=constant_function(module,"table");
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,checksum_entry,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,1024).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue checksum={0};CHECK(xr_xir_instance_take_result(instance,&checksum)==XR_XIR_CALL_RETURNED);
    CHECK(checksum.type==XR_XIR_I64 && !checksum.reserved && checksum.payload==7532);
    xr_xir_value_drop(&checksum);
    CHECK(xr_xir_instance_start(instance,table_entry,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,1024).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue table={0};CHECK(xr_xir_instance_take_result(instance,&table)==XR_XIR_CALL_RETURNED);
    constant_value(&table);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
    xr_compile_resources_release(context.resources);context.resources=NULL;
    constant_value(&table);xr_xir_value_drop(&table);
    CHECK(!runtime_live && !runtime_bytes);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
static void constant_wide_output(void) {
    const char text[]="fn large()->(i64,i64){return (2,3)}\nexport fn table()->(i64,i64){return comptime large()}\n";
    XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&constant_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(constant_source(&context,text,&source,&diagnostic)==XR_XIR_OK && source.checked && source.snapshot);
    XrXirArtifact *closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_specialize(source.checked,&closed,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    uint32_t entry=constant_function(xr_xir_compile_artifact_module(lowered),"table");
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,1024).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};
    CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);xr_compile_resources_release(context.resources);
    const int64_t expected[]={2,3};
    for(uint32_t field=0;field<2;++field) {
        XrXirValue part={0};CHECK(xr_xir_tuple_get(&value,field,&part)==XR_XIR_VALUE_OK);
        CHECK(part.type==XR_XIR_I64 && !part.reserved && part.payload==expected[field]);
        xr_xir_value_drop(&part);
    }
    xr_xir_value_drop(&value);
    CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
}
static void constant_refusals(void) {
    const char text[]="fn effectful()->(i16,i16){print(1);return (2,3)}\nexport fn table()->(i16,i16){return comptime effectful()}\n";
    XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&constant_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(constant_source(&context,text,&source,&diagnostic)==XR_XIR_BAD_TYPE);
    CHECK(diagnostic.status==XR_XIR_BAD_TYPE && diagnostic.line>0 && !source.checked && !source.snapshot);
    xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
static XrCompileResourceStats constant_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);return stats;
}
static void constant_resources(void) {
    XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&constant_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();
    XrCompileResourceStats baseline=constant_stats(&context);
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    source_fixture_compile_attempts=0;
    CHECK(constant_source(&context,constant_table_source,&source,&diagnostic)==XR_XIR_OK);
    size_t sites=source_fixture_compile_attempts;XrCompileResourceStats required=constant_stats(&context);
    xr_xir_compile_source_result_free(&source);
    CHECK(constant_stats(&context).live_bytes==baseline.live_bytes);xr_compile_resources_release(context.resources);
    CHECK(sites && !source_fixture_compile_live && !source_fixture_compile_bytes);
    for(size_t fault=0;fault<sites;++fault) {
        context=(XrXirCompileContext){0};
        CHECK(xr_compile_resources_new(&constant_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
        context.limits=xr_xir_compile_default_limits();baseline=constant_stats(&context);
        source_fixture_compile_attempts=0;source_fixture_compile_fail_at=fault;
        source=(XrXirSourceResult){0};diagnostic=(XrXirSourceDiagnostic){0};
        XrXirStatus status=constant_source(&context,constant_table_source,&source,&diagnostic);
        source_fixture_compile_fail_at=SIZE_MAX;
        CHECK(status==XR_XIR_OUT_OF_MEMORY && !source.checked && !source.snapshot);
        CHECK(source_fixture_compile_attempts>fault && constant_stats(&context).live_bytes==baseline.live_bytes);
        XrCompileResourceStats failed=constant_stats(&context);
        CHECK(constant_source(&context,constant_table_source,&source,&diagnostic)==XR_XIR_OK && source.checked && source.snapshot);
        xr_xir_compile_source_result_free(&source);XrCompileResourceStats retry=constant_stats(&context);
        CHECK(retry.live_bytes==baseline.live_bytes && retry.allocated_bytes>=failed.allocated_bytes && retry.work>=failed.work);
        xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    }
    for(unsigned axis=0;axis<3;++axis)for(unsigned shortfall=0;shortfall<2;++shortfall) {
        XrCompileResourceLimits limits=constant_limits;
        if(axis==0)limits.allocated_bytes=required.allocated_bytes-shortfall;
        if(axis==1)limits.live_bytes=required.peak_bytes-shortfall;
        if(axis==2)limits.work=required.work-shortfall;
        context=(XrXirCompileContext){0};
        CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
        context.limits=xr_xir_compile_default_limits();baseline=constant_stats(&context);
        source=(XrXirSourceResult){0};diagnostic=(XrXirSourceDiagnostic){0};
        XrXirStatus status=constant_source(&context,constant_table_source,&source,&diagnostic);
        CHECK(status==(shortfall?XR_XIR_BUDGET:XR_XIR_OK));
        CHECK(shortfall?(!source.checked && !source.snapshot):(source.checked && source.snapshot));
        xr_xir_compile_source_result_free(&source);CHECK(constant_stats(&context).live_bytes==baseline.live_bytes);
        xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    }
    printf("Constant table whole Source: %zu allocation faults with same-ledger retry, three exact/minus-one axes, physical0; allocated=%llu peak=%llu work=%llu\n",
        sites,(unsigned long long)required.allocated_bytes,(unsigned long long)required.peak_bytes,(unsigned long long)required.work);
}
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"--resources")){constant_resources();return 0;}
    CHECK(argc==1);
    constant_table();constant_wide_output();constant_refusals();
    source_fixture_source_owners_free();
    puts("Ordinary eight-byte constant table: ordered description, independent recipe, packet, VM7532, producer death, complete sixteen-byte pair and effect refusal");
    return 0;
}
