/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_source.c - Real Source yields through owned typed execution
 *
 * KEY CONCEPT:
 *   A produced Unit is observable separately from completion and scheduling.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_call_generator.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_types.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)
#include "xir_source_fixture_owner.h"
#include "xir_generator84_golden.h"
#include "xir_generator74_golden.h"
_Static_assert(XR_XIR_TYPE_ITERATOR==11,"Iterator excludes Map and Set descriptor ordinals");
_Static_assert(XR_XIR_GENERATOR_YIELD==176,"independent Generator opcode ordinal");
_Static_assert(XR_XIR_PROGRAM==0 && XR_XIR_LIBRARY==1,"independent packet linkage ordinals");
static const char source_text[]=
    "export fn numbers() -> Iterator<i64> {\n yield 11\n Coro.yield()\n yield 22\n }\n"
    "fn units() -> Iterator<()> { yield ()\n yield ()\n }\n"
    "fn identity(it: Iterator<i64>) -> Iterator<i64> { return it }\n"
    "fn main() {}\n";
static uint32_t find_function(const XrXirModule *m,const char *name) {
    for(uint32_t f=0;f<m->function_count;++f)
        if(m->functions[f].name_length==strlen(name) && !memcmp(m->functions[f].name,name,strlen(name))) return f;
    CHECK(false);return 0;
}
static void vm_pull(const XrXirArtifact *lowered,uint32_t function,XrXirTypeArena *arena,XrXirDomain *domain,bool unit) {
    XrXirVmBinding binding={0};XrXirCallEntry entry={0};
    CHECK(xr_xir_compile_vm_bind(lowered,function,&binding,&entry)==XR_XIR_OK);
    CHECK(entry.flags & XR_XIR_ENTRY_GENERATOR);
    XrXirCallConfig config;CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirCallAccounting accounting={0};config.entries=&entry;config.entry_count=1;config.accounting=&accounting;
    config.admission=(XrXirValueAdmission){arena,domain,NULL,NULL,1000000,65536};
    XrXirCall *call=NULL;CHECK(xr_xir_call_generator_new(&config,0,NULL,0,&call)==XR_XIR_CALL_READY);
    for(unsigned i=0;i<2;++i) {
        XrXirCallResult result=xr_xir_call_poll_bounded(call,1000);
        if(!unit && i==1) {
            CHECK(result.status==XR_XIR_CALL_SUSPENDED && result.wake);
            CHECK(xr_xir_call_resume(call,result.wake)==XR_XIR_CALL_READY);
            result=xr_xir_call_poll_bounded(call,1000);
        }
        CHECK(result.status==XR_XIR_CALL_YIELDED && !result.wake);
        XrXirValue value={0};CHECK(xr_xir_call_generator_take(call,&value)==XR_XIR_CALL_YIELDED);
        CHECK(value.type==(uint32_t)(unit ? XR_XIR_UNIT : XR_XIR_I64) && value.payload==(unit ? 0 : i ? 22 : 11));
        xr_xir_value_drop(&value);
    }
    CHECK(xr_xir_call_poll_bounded(call,1000).status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY);
    CHECK(!accounting.live_bytes && !accounting.depth && accounting.allocations==accounting.frees);
}
static void packet_models(const XrXirCompileContext *context) {
    const struct {const uint8_t *bytes;size_t length;} previous[]={
        {generator74_i64,sizeof(generator74_i64)},
        {generator74_unit,sizeof(generator74_unit)},
        {generator74_no_role,sizeof(generator74_no_role)},
        {generator74_scalar_result,sizeof(generator74_scalar_result)},
        {generator74_wrong_value,sizeof(generator74_wrong_value)},
        {generator74_return_arg,sizeof(generator74_return_arg)},
        {generator74_no_suspend,sizeof(generator74_no_suspend)},
        {generator74_ref_parameter,sizeof(generator74_ref_parameter)},
        {generator74_unit_arg,sizeof(generator74_unit_arg)},
        {generator74_unknown_role,sizeof(generator74_unknown_role)},
        {generator74_initializer_role,sizeof(generator74_initializer_role)},
        {generator74_entry_role,sizeof(generator74_entry_role)}};
    for(unsigned i=0;i<sizeof(previous)/sizeof(previous[0]);++i) {
        XrCompileResourceStats before={0},after={0};
        CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
        XrXirArtifact *decoded=NULL;
        CHECK(xr_xir_compile_checked_read(context,previous[i].bytes,previous[i].length,&decoded,NULL)==XR_XIR_BAD_STRUCTURE && !decoded);
        CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK);
        CHECK(after.allocated_bytes==before.allocated_bytes && after.allocation_count==before.allocation_count && after.live_bytes==before.live_bytes);
    }
    const struct {const uint8_t *bytes;size_t length;XrXirStatus expected;} cases[]={
        {generator84_i64,sizeof(generator84_i64),XR_XIR_OK},{generator84_unit,sizeof(generator84_unit),XR_XIR_OK},
        {generator84_no_role,sizeof(generator84_no_role),XR_XIR_BAD_TYPE},
        {generator84_scalar_result,sizeof(generator84_scalar_result),XR_XIR_BAD_TYPE},
        {generator84_wrong_value,sizeof(generator84_wrong_value),XR_XIR_BAD_TYPE},
        {generator84_return_arg,sizeof(generator84_return_arg),XR_XIR_BAD_STRUCTURE},
        {generator84_no_suspend,sizeof(generator84_no_suspend),XR_XIR_BAD_STRUCTURE},
        {generator84_ref_parameter,sizeof(generator84_ref_parameter),XR_XIR_BAD_TYPE},
        {generator84_unit_arg,sizeof(generator84_unit_arg),XR_XIR_BAD_STRUCTURE},
        {generator84_unknown_role,sizeof(generator84_unknown_role),XR_XIR_BAD_STRUCTURE},
        {generator84_initializer_role,sizeof(generator84_initializer_role),XR_XIR_BAD_STRUCTURE},
        {generator84_entry_role,sizeof(generator84_entry_role),XR_XIR_BAD_STRUCTURE},
        {generator84_reserved_map,sizeof(generator84_reserved_map),XR_XIR_BAD_TYPE},
        {generator84_reserved_set,sizeof(generator84_reserved_set),XR_XIR_BAD_TYPE}};
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        XrXirArtifact *decoded=NULL;XrXirStatus status=xr_xir_compile_checked_read(context,cases[i].bytes,cases[i].length,&decoded,NULL);
        if(status!=cases[i].expected) fprintf(stderr,"Independent packet%u status%u expected%u\n",i,status,cases[i].expected);
        CHECK(status==cases[i].expected);
        if(status==XR_XIR_OK) {
            XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(decoded,&packet,NULL)==XR_XIR_OK);
            CHECK(packet.length==cases[i].length && !memcmp(packet.bytes,cases[i].bytes,packet.length));
            xr_xir_compile_checked_packet_free(&packet);
            XrXirArtifact *lowered=NULL;status=xr_xir_compile_lower(decoded,
                &(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},&lowered,NULL);
            if(status!=XR_XIR_OK) fprintf(stderr,"Independent packet%u lower status%u\n",i,status);
            CHECK(status==XR_XIR_OK);
            CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(lowered);
        } else CHECK(!decoded);
        xr_xir_compile_artifact_free(decoded);
    }
}
static void reject_source(const XrXirCompileContext *context,const char *text) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"generator-negative",NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,
        &(XrXirSourceText){NULL,text,strlen(text)},&source,&diagnostic,NULL);
    if(status!=XR_XIR_BAD_TYPE) fprintf(stderr,"Negative Source status%u line%d: %s\n",status,diagnostic.line,diagnostic.message);
    CHECK(status==XR_XIR_BAD_TYPE && !source.checked);
    xr_xir_compile_source_result_free(&source);xr_compile_session_free(session);
}
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2);SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"generator-source",NULL};
    XrXirSourceRequest request={session,NULL,&authority,&owner.context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,
        &(XrXirSourceText){NULL,source_text,strlen(source_text)},&source,&diagnostic,NULL);
    if(status!=XR_XIR_OK) fprintf(stderr,"Source status%u line%d: %s\n",status,diagnostic.line,diagnostic.message);
    CHECK(status==XR_XIR_OK);xr_compile_session_free(session);
    XrXirCheckedPacket packet={0};XrXirArtifact *decoded=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(decoded,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(decoded);
    CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);const XrXirModule *m=xr_xir_compile_artifact_module(lowered);
    uint32_t numbers=find_function(m,"numbers"),units=find_function(m,"units"),identity=find_function(m,"identity");
    CHECK(numbers==1 && units==2 && m->function_count==6);
    CHECK(m->declarations->functions[numbers].promises==XR_XIR_FUNCTION_GENERATOR);
    CHECK(m->declarations->functions[units].promises==XR_XIR_FUNCTION_GENERATOR);
    CHECK(!m->declarations->functions[identity].promises);
    uint32_t yields=0;for(uint32_t f=0;f<m->function_count;++f)for(uint32_t i=0;i<m->functions[f].instruction_count;++i)
        yields+=m->functions[f].instructions[i].op==XR_XIR_GENERATOR_YIELD;
    CHECK(yields==4);
    XrXirTypeArena *arena=NULL;XrXirDomain *domain=NULL;
    CHECK(xr_xir_compile_type_arena_new(&owner.context,m->types,&arena)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    vm_pull(lowered,numbers,arena,domain,false);vm_pull(lowered,units,arena,domain,true);
    XrXirCSource c={0};CHECK(xr_xir_compile_emit_c(lowered,"generator_source",1048576,&c)==XR_XIR_OK);
    CHECK(c.text && c.length && !strstr(c.text,"({"));
    if(argc==2) {FILE *file=fopen(argv[1],"wb");CHECK(file);CHECK(fwrite(c.text,1,c.length,file)==c.length);CHECK(!fclose(file));}
    xr_xir_compile_c_source_free(&c);xr_xir_compile_type_arena_drop(arena);xr_xir_domain_drop(domain);
    uint32_t program_entry=m->declarations->entry_function;XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && program && !lowered);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,numbers,NULL,0)==XR_XIR_CALL_BAD_ARGUMENT &&
        xr_xir_instance_state(instance)==XR_XIR_INSTANCE_NEW);
    CHECK(xr_xir_instance_start(instance,program_entry,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,1000000).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(result.type==XR_XIR_I64 && !result.payload);xr_xir_value_drop(&result);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
    reject_source(&owner.context,"fn bad()->i64 {yield 1}\nfn main(){}\n");
    reject_source(&owner.context,"fn bad()->Iterator<i64> {yield true}\nfn main(){}\n");
    reject_source(&owner.context,"fn bad()->Iterator<i64> {yield 1\nreturn 2}\nfn main(){}\n");
    reject_source(&owner.context,"fn bad()->Iterator<i64> {yield 1}\nfn main(){bad()}\n");
    packet_models(&owner.context);
    source_fixture_owner_free(&owner);
    puts("Real Source/Checked/Lowered generator role, VM yields/Unit/wake and verified C emission PASS");return 0;
}
