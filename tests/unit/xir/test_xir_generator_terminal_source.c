/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_terminal_source.c - Retired Generator identity and physical captures
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_source_fixture_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_cold_callable_binding_cases.h"
static const char terminal_text[]=
    "fn scalar(seed:i64)->Iterator<i64>{yield seed}\n"
    "fn by_function(f:fn()->i64)->Iterator<i64>{try{yield f()} catch(error){yield -2}}\n"
    "fn by_iterator(inner:Iterator<i64>)->Iterator<i64>{try{yield inner.next()} catch(error){yield -2}}\n"
    "fn worker_scalar()->i64{try{const it=scalar(17)\n"
    "for(value in it){break}\n"
    "const holder=fn()->Iterator<i64>{return it}\n"
    "const done=holder()\n"
    "if(done.hasNext()){return -1}\n"
    "return 17} catch(error){return -99}}\n"
    "fn worker_function()->i64{try{const f:fn()->i64=fn()->i64{return 17}\n"
    "const it=by_function(f)\n"
    "for(value in it){break}\n"
    "const holder=fn()->Iterator<i64>{return it}\n"
    "const done=holder()\n"
    "if(done.hasNext()){return -1}\n"
    "return 17} catch(error){return -99}}\n"
    "fn worker_iterator()->i64{try{const inner=scalar(17)\n"
    "const it=by_iterator(inner)\n"
    "for(value in it){break}\n"
    "const holder=fn()->Iterator<i64>{return it}\n"
    "const done=holder()\n"
    "if(done.hasNext()){return -1}\n"
    "return 17} catch(error){return -99}}\n"
    "export fn run_scalar()->i64{return await go worker_scalar()}\n"
    "export fn run_function()->i64{return worker_function()}\n"
    "export fn run_iterator()->i64{return worker_iterator()}\n"
    "export fn make_external()->Iterator<i64>{const f:fn()->i64=fn()->i64{return 17}\n"
    "return by_function(f)}\n"
    "export fn make_holder()->fn()->Iterator<i64>{const f:fn()->i64=fn()->i64{return 17}\n"
    "const it=by_function(f)\n"
    "for(value in it){break}\n"
    "return fn()->Iterator<i64>{return it}}\n"
    "fn main(){}\n"
    "";
static size_t terminal_source_text(char *text,bool inferred,uint32_t workers) {
    memcpy(text,terminal_text,sizeof(terminal_text));uint32_t bindings=0;
    if(inferred) {
        const char *bound=":fn()->i64";char *at=NULL;
        while((at=strstr(text,"const f:fn()->i64="))!=NULL) {
            at+=strlen("const f");size_t size=strlen(bound);
            memmove(at,at+size,strlen(at+size)+1);++bindings;
        }
        CHECK(bindings==3);
    }
    if(workers) {
        const char *calls[]={"return worker_function()","return worker_iterator()"};
        for(uint32_t i=0;i<workers;++i) {
            char *at=strstr(text,calls[i]);CHECK(at && !strstr(at+1,calls[i]));at+=strlen("return ");
            CHECK(strlen(text)+9<sizeof(terminal_text)+64);
            memmove(at+9,at,strlen(at)+1);memcpy(at,"await go ",9);
        }
    }
    return strlen(text);
}
static uint32_t terminal_entry(const XrXirModule *module,const char *name) {
    for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==strlen(name) && !memcmp(module->functions[f].name,name,strlen(name)))return f;
    return UINT32_MAX;
}
static void terminal_source_execute(XrXirProgram *program,uint32_t entry) {
    XrXirInstanceConfig config;XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirCallStatus status=xr_xir_instance_poll_bounded(instance,1000000).outcome.status;
    if(status!=XR_XIR_CALL_RETURNED)fprintf(stderr,"terminal execution entry%u status%u\n",entry,status);
    CHECK(status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(value.type==XR_XIR_I64 && value.payload==17);xr_xir_value_drop(&value);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    printf("TERMINAL_EXECUTION entry%u returned17/released PASS\n",entry);
}
int main(int argc,char **argv) {
    bool workers=argc>1 && !strcmp(argv[1],"--inferred-workers");
    bool fn_worker=argc>1 && !strcmp(argv[1],"--inferred-function-worker");
    bool inferred=workers || fn_worker || (argc>1 && !strcmp(argv[1],"--inferred"));
    CHECK(inferred?(argc==2 || argc==3):(argc==1 || argc==2));
    const char *path=inferred?(argc==3?argv[2]:NULL):(argc==2?argv[1]:NULL);
    const char *prefix=workers?"generator_terminal_workers":fn_worker?"generator_terminal_fnworker":
        inferred?"generator_terminal_inferred":"generator_terminal";
    char text[sizeof(terminal_text)+64];size_t length=terminal_source_text(text,inferred,workers?2u:fn_worker?1u:0u);
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"generator-terminal-lifetime",NULL};
    XrXirSourceRequest request={session,NULL,&authority,&owner.context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,
        &(XrXirSourceText){NULL,text,length},&source,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"terminal Source status%u line%d: %s\n",status,diagnostic.line,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked);xr_compile_session_free(session);
    if(inferred)cold_binding_inspect(source.checked);
    XrXirCheckedPacket packet={0};XrXirArtifact *decoded=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&source);
    CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(decoded,&closed,NULL, NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(decoded);
    CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}, NULL,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    uint32_t ids[5]={terminal_entry(module,"run_scalar"),terminal_entry(module,"run_function"),
        terminal_entry(module,"run_iterator"),terminal_entry(module,"make_external"),terminal_entry(module,"make_holder")};
    for(uint32_t i=0;i<5;++i)CHECK(ids[i]!=UINT32_MAX);
    XrXirCSource c={0};CHECK(xr_xir_compile_emit_c(lowered,prefix,1048576,&c)==XR_XIR_OK);
    CHECK(c.text && c.length && !strstr(c.text,"({"));
    if(path) {FILE *file=fopen(path,"wb");CHECK(file);CHECK(fwrite(c.text,1,c.length,file)==c.length);
        CHECK(fprintf(file,"\nconst uint32_t %s_ids[5]={%u,%u,%u,%u,%u};\n",prefix,ids[0],ids[1],ids[2],ids[3],ids[4])>0);CHECK(!fclose(file));}
    xr_xir_compile_c_source_free(&c);XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    for(uint32_t i=0;i<3;++i)terminal_source_execute(program,ids[i]);xr_xir_compile_program_drop(program);
    source_fixture_owner_free(&owner);CHECK(!runtime_live && !runtime_bytes);
    puts("TERMINAL_CAPTURE genuine scalar/Fn/Iterator retired environments, worker Fn physical read, returned17/physical0 PASS");
    if(inferred)puts("COLD_FN_BIND genuine inferred Fn actuals through Generator creation and retired captures, returned17/physical0 PASS");
    if(workers)puts("WORKER_TERMINAL genuine scalar/Fn/Iterator parameter paths all in actual workers, returned17/physical0 PASS");
    if(fn_worker)puts("WORKER_FN_TERMINAL genuine Fn parameter and retired captures in actual worker, returned17/physical0 PASS");return 0;
}
