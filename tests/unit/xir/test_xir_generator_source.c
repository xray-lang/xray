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
#include "xir_construction_fixture.h"
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
    XrXirCallResult complete=xr_xir_call_poll_bounded(call,1000);
    CHECK(complete.status==XR_XIR_CALL_RETURNED && !complete.wake &&
        complete.value.type==XR_XIR_UNIT && !complete.value.reserved && !complete.value.payload);
    CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY);
    CHECK(!accounting.live_bytes && !accounting.depth && accounting.allocations==accounting.frees);
}
/* Direct module cases validate semantic fields, never a packet format. */
typedef struct GeneratorModuleFixture {
    XrXirTypeNode nodes[2]; XrXirTypes types; XrXirType parameter;
    XrXirInstruction init,producer[3],entry[2]; XrXirBlock blocks[3];
    XrXirFunction functions[3]; XrXirFunctionIdentity identities[3];
    XrXirSourceModule source; XrXirDeclarations declarations; XrXirModule module;
} GeneratorModuleFixture;
static void generator_module_fixture(GeneratorModuleFixture *f,unsigned variant) {
    memset(f,0,sizeof(*f));
    bool unit=variant==1 || variant==8;
    XrXirType iterator=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ITERATOR,
        .element=unit?XR_XIR_UNIT:XR_XIR_I64,.result=XR_XIR_NEVER};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64};
    f->types=(XrXirTypes){f->nodes,variant==7?2u:1u,NULL,NULL};
    if(variant==12)f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_MAP,.element=XR_XIR_I64,.result=XR_XIR_I64};
    if(variant==13)f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_SET,.element=XR_XIR_I64};
    f->init=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->producer[0]=unit?(XrXirInstruction){.op=XR_XIR_GENERATOR_YIELD}:
        (XrXirInstruction){.op=variant==4?XR_XIR_CONST_BOOL:XR_XIR_CONST_INT,
            .type=variant==4?XR_XIR_BOOL:XR_XIR_I64,.immediate=variant==4?1:11};
    f->producer[1]=(XrXirInstruction){.op=XR_XIR_GENERATOR_YIELD,.args={unit?0u:variant==7?1u:0u}};
    f->producer[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={variant==5?1u:0u}};
    if(variant==8)f->producer[0].args[0]=1;
    f->entry[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64};
    f->entry[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0}};
    const char *names[]={"init",unit?"units":"numbers","entry"};
    XrXirInstruction *ops[]={&f->init,f->producer,f->entry};
    const uint32_t counts[]={1,3,2};
    for(unsigned n=0;n<3;++n) {
        f->blocks[n]=(XrXirBlock){.count=counts[n]};
        f->functions[n]=(XrXirFunction){.name=names[n],.name_length=(uint32_t)strlen(names[n]),
            .result=n==0?XR_XIR_UNIT:n==1?(variant==3?XR_XIR_I64:iterator):XR_XIR_I64,
            .blocks=&f->blocks[n],.block_count=1,.instructions=ops[n],.instruction_count=counts[n],
            .error_type=XR_XIR_NEVER};
    }
    if(variant==7) {
        f->parameter=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1);
        f->functions[1].parameters=&f->parameter;f->functions[1].parameter_count=1;
    }
    f->identities[1].exported=1;
    f->identities[1].promises=variant==2?0:variant==9?UINT32_MAX:
        XR_XIR_FUNCTION_GENERATOR | (variant==6?XR_XIR_FUNCTION_NO_SUSPEND:0);
    if(variant==10)f->identities[0].promises=XR_XIR_FUNCTION_GENERATOR;
    if(variant==11)f->identities[2].promises=XR_XIR_FUNCTION_GENERATOR;
    f->source=(XrXirSourceModule){"generator",9,NULL,0,0};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,
        .functions=f->identities,.entry_function=2};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=3,
        .types=&f->types,.declarations=&f->declarations,.linkage_kind=XR_XIR_PROGRAM};
}
static void generator_module_models(const XrXirCompileContext *context) {
    static const struct {const char *name;XrXirStatus expected;} cases[]={
        {"i64",XR_XIR_OK},{"unit",XR_XIR_OK},{"no_role",XR_XIR_BAD_TYPE},
        {"scalar_result",XR_XIR_BAD_TYPE},{"wrong_value",XR_XIR_BAD_TYPE},
        {"return_arg",XR_XIR_BAD_STRUCTURE},{"no_suspend",XR_XIR_BAD_STRUCTURE},
        {"ref_parameter",XR_XIR_BAD_TYPE},{"unit_arg",XR_XIR_BAD_STRUCTURE},
        {"unknown_role",XR_XIR_BAD_STRUCTURE},{"initializer_role",XR_XIR_BAD_STRUCTURE},
        {"entry_role",XR_XIR_BAD_STRUCTURE},{"map_result",XR_XIR_BAD_TYPE},{"set_result",XR_XIR_BAD_TYPE}};
    for(unsigned n=0;n<sizeof(cases)/sizeof(*cases);++n) {
        GeneratorModuleFixture fixture;generator_module_fixture(&fixture,n);
        XrXirArtifact *checked=NULL,*lowered=NULL;XrXirDiagnostic diagnostic={0};
        XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,&diagnostic);
        if(status!=cases[n].expected)fprintf(stderr,"Generator Module %s status%u expected%u f%u i%u\n",
            cases[n].name,status,cases[n].expected,diagnostic.function,diagnostic.instruction);
        CHECK(status==cases[n].expected);
        if(status==XR_XIR_OK) {
            CHECK(checked);memset(&fixture,0xa5,sizeof(fixture));
            CHECK(xr_xir_compile_lower(checked,&(XrXirTarget){XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},
                NULL,&lowered,&diagnostic)==XR_XIR_OK);
            CHECK(xr_xir_compile_artifact_verify(lowered,&diagnostic)==XR_XIR_OK);
        } else CHECK(!checked);
        xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(checked);
    }
}

static void reject_source(const XrXirCompileContext *context,const char *text) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
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
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILE_RESOURCE_OK);
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
    CHECK(xr_xir_compile_specialize(decoded,&closed,NULL, NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(decoded);
    CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION}, NULL,&lowered,NULL)==XR_XIR_OK);
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
    XrXirStatus received=xr_xir_compile_vm_program_take(&lowered,&program);
    if(received!=XR_XIR_OK) fprintf(stderr,"Generator Program receiving status%u\n",received);
    CHECK(received==XR_XIR_OK && program && !lowered);
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
    reject_source(&owner.context,"fn bad(x:ref i64)->Iterator<i64> {yield x}\nfn main(){}\n");
    generator_module_models(&owner.context);
    source_fixture_owner_free(&owner);
    puts("Real Source/Checked/Lowered generator role, VM yields/Unit/wake and verified C emission PASS");return 0;
}
