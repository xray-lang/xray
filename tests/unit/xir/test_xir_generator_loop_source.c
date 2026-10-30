/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_loop_source.c - Real Source producers suspend through the governed time module
 *
 * KEY CONCEPT:
 *   Only the owning executor holds code authority for a live producer.
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
#include "xir_generator_wait_cases.h"
#include "xir_generator_loop_cases.h"
_Static_assert(XR_XIR_ITERATOR_HAS_NEXT==178 && XR_XIR_ITERATOR_NEXT==179,"canonical consumer ordinals");
static const char factory_source[]=
    "import time\n"
    "var cleanup_count:i64=0\n"
    "var saved:Iterator<string>?=null\n"
    "fn delayed()->Iterator<string> {defer {print(401)}\n"
    " time.sleep(5)\n"
    " yield \"one\"\n"
    " time.sleep(7)\n"
    " yield \"two\"\n"
    " }\n"
    "export fn make()->Iterator<string> {return delayed()}\n"
    "export fn probe(it:Iterator<string>)->bool {return it.hasNext()}\n"
    "export fn take(it:Iterator<string>)->string {return it.next()}\n"
    "export fn empty()->i64 {return 9}\n"
    "fn relay(it:Iterator<string>)->Iterator<string> {defer {print(402)}\n"
    " const a=it.next()\n"
    " yield a\n"
    " const b=it.next()\n"
    " yield b\n"
    " }\n"
    "export fn make_relay(it:Iterator<string>)->Iterator<string> {return relay(it)}\n"
    "fn task_worker()->string {time.sleep(11)\n"
    " return \"task\"}\n"
    "export fn task_root()->string {const work=go task_worker()\n"
    " return await work}\n"
    "fn self_body()->Iterator<string> {defer {print(402)}\n"
    " const current=saved!\n"
    " current.next()\n"
    " yield \"unreachable\"\n"
    " }\n"
    "export fn make_self()->Iterator<string> {const it=self_body()\n"
    " saved=it\n"
    " return it}\n"
    "fn cycle_inner(target:Iterator<string>)->Iterator<string> {defer {print(403)}\n"
    " target.next()\n"
    " yield \"unreachable\"\n"
    " }\n"
    "fn cycle_outer()->Iterator<string> {defer {print(402)}\n"
    " const current=saved!\n"
    " const other=cycle_inner(current)\n"
    " other.next()\n"
    " yield \"unreachable\"\n"
    " }\n"
    "export fn make_cycle()->Iterator<string> {const it=cycle_outer()\n"
    " saved=it\n"
    " return it}\n"
    "export fn loop_break(it:Iterator<string>)->i64 {for(value in it) {break}\n"
    " return closed_count()}\n"
    "export fn loop_continue(it:Iterator<string>)->i64 {var count=0\n"
    " for(value in it) {if(value==\"one\") {continue}\n"
    " count=count+1}\n"
    " return count*10000+closed_count()}\n"
    "export fn loop_return(it:Iterator<string>)->string {for(value in it) {return value}\n"
    " return \"empty\"}\n"
    "enum LoopError { Bad }\n"
    "export fn loop_throw(it:Iterator<string>)->i64 {try {for(value in it) {throw LoopError.Bad}} catch(error) {return closed_count()}\n"
    " return -1}\n"
    "export fn closed_count()->i64 {return cleanup_count}\n"
    "fn units()->Iterator<()> {defer {cleanup_count=cleanup_count+1\n"
    " print(401)}\n"
    " time.sleep(5)\n"
    " yield ()\n"
    " time.sleep(7)\n"
    " yield ()\n"
    " cleanup_count=cleanup_count+1000}\n"
    "export fn make_units()->Iterator<()> {return units()}\n"
    "export fn loop_units(it:Iterator<()>)->i64 {var count=0\n"
    " for(value in it) {const kept=value\n"
    " count=count+1}\n"
    " return count*10000+closed_count()}\n"
    "export fn make_owned()->Iterator<string> {var first=\"one\"\n"
    " var second=\"two\"\n"
    " fn producer()->Iterator<string> {defer {first=\"closed\"\n"
    " cleanup_count=cleanup_count+1\n"
    " print(401)}\n"
    " time.sleep(5)\n"
    " yield first\n"
    " time.sleep(7)\n"
    " yield second\n"
    " cleanup_count=cleanup_count+1000}\n"
    " return producer()}\n"
    "export fn loop_panic(it:Iterator<string>)->i64 {for(value in it) {const a=[1]\n"
    " return a[2]}\n"
    " return -1}\n"
    "export fn loop_nested(outer:Iterator<string>,inner:Iterator<string>)->i64 {for(a in outer) {for(b in inner) {break}\n"
    " return closed_count()}\n"
    " return -1}\n"
    "export fn loop_captures(it:Iterator<string>)->string {var actions:Array<fn()->string>=[]\n"
    " for(value in it) {actions.push(fn()->string {return value})}\n"
    " return actions[0]()+actions[1]()}\n"
    "fn identity<T>(it:Iterator<T>)->Iterator<T> {return it}\n"
    "fn count_values<T>(it:Iterator<T>)->i64 {var count=0\n"
    " const ready=identity(it)\n"
    " for(value in ready) {count=count+1}\n"
    " return count*10000+closed_count()}\n"
    "export fn loop_generic(it:Iterator<string>)->i64 {return count_values(it)}\n"
    "export fn loop_builtin()->i64 {var values=[1,2,3]\n"
    " var sum=0\n"
    " for(value in values){sum=sum+value\n"
    " values[0]=9}\n"
    " for(index in 0..3){sum=sum+index}\n"
    " for(index in 1..=3){if(index==2){continue}\n"
    " sum=sum+index}\n"
    " return sum}\n"
    "fn task_iterator_worker(fail:bool)->i64 {var done=0\n"
    " fn items()->Iterator<i64> {defer {done=done+1\n"
    " print(401)}\n"
    " time.sleep(5)\n"
    " yield 1\n"
    " if(fail){throw LoopError.Bad}\n"
    " done=1000}\n"
    " for(value in items()){if(!fail){break}}\n"
    " return done}\n"
    "export fn loop_task()->i64 {const work=go task_iterator_worker(false)\n"
    " return await work}\n"
    "export fn loop_task_error()->i64 {const work=go task_iterator_worker(true)\n"
    " try{return await work} catch(error){return 17}}\n"
    "fn cold_worker()->i64 {const unused=make_owned()\n return 11}\n"
    "export fn loop_cold()->i64 {return await go cold_worker()}\n"
    "fn main(){}\n";
static void loop_rejections(SourceFixtureOwner *owner) {
    const char *cases[]={
        "enum E { Bad }\nfn main(){defer {throw E.Bad}}\n",
        "fn main(){defer {Coro.yield()}}\n",
        "fn bad(it:Iterator<i64>){it.close()}\nfn main(){}\n",
        "fn bad(it:Iterator<i64>){for(value in it){value=1}}\nfn main(){}\n",
        "fn count<T>(it:Iterator<T>)->i64 {var n=0\n for(value in it){n=n+1}\n return n}\nfn bad(it:Iterator<()>)->i64{return count(it)}\nfn main(){}\n"
    };
    for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(owner->context.resources,&session)==XR_COMPILER_SESSION_OK);
        XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"iterator-loop-negative",NULL};
        XrXirSourceRequest request={session,NULL,&authority,&owner->context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
        XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_compile_source_check_text(&request,
            &(XrXirSourceText){NULL,cases[i],strlen(cases[i])},&source,&diagnostic,NULL);
        if(status!=XR_XIR_BAD_TYPE) fprintf(stderr,"Loop negative%u status%u: %s\n",i,status,diagnostic.message);
        CHECK(status==XR_XIR_BAD_TYPE && !source.checked);
        if(i<2) CHECK(strstr(diagnostic.message,i ? "E0392" : "E0387"));
        xr_xir_compile_source_result_free(&source);xr_compile_session_free(session);
    }
}
static void loop_worker_rejections(void) {
    const char *cases[]={
        "class Local {}\nenum Unsafe {Bad{value:Local}}\nfn worker()->i64 {fn items()->Iterator<i64> {yield 1\n throw Unsafe.Bad{value:Local()}}\n for(value in items()){}\n return 0}\nfn start(){const work=%sworker()}\nfn main(){}\n",
        "var root=0\nfn worker()->i64 {root=root+1\n fn items()->Iterator<i64> {yield 1}\n for(value in items()){}\n return 0}\nfn start(){const work=%sworker()}\nfn main(){}\n",
        "fn worker(make:fn()->Iterator<i64>)->i64 {for(value in make()){}\n return 0}\nfn start(make:fn()->Iterator<i64>){const work=%sworker(make)}\nfn main(){}\n"
    };
    for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
        for(unsigned go=0;go<2;++go) {
            char text[1024];int length=snprintf(text,sizeof(text),cases[i],go ? "go " : "");
            CHECK(length>0 && (size_t)length<sizeof(text));
            XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILER_SESSION_OK);
            XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"iterator-worker-negative",NULL};
            XrXirSourceRequest request={session,NULL,&authority,&owner.context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
            XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
            XrXirStatus status=xr_xir_compile_source_check_text(&request,
                &(XrXirSourceText){NULL,text,(size_t)length},&source,&diagnostic,NULL);
            XrXirStatus expected=go ? XR_XIR_BAD_TYPE : XR_XIR_OK;
            if(status!=expected) fprintf(stderr,"Loop worker case%u go%u status%u expected%u: %s\n",i,go,status,expected,diagnostic.message);
            CHECK(status==expected && (go ? !source.checked : source.checked!=NULL));
            xr_xir_compile_source_result_free(&source);xr_compile_session_free(session);
        }
        source_fixture_owner_free(&owner);
    }
}
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2);SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);loop_rejections(&owner);loop_worker_rejections();
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"generator-wait",NULL};
    XrXirSourceRequest request={session,NULL,&authority,&owner.context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,
        &(XrXirSourceText){NULL,factory_source,sizeof(factory_source)-1},&source,&diagnostic,NULL);
    if(status!=XR_XIR_OK) fprintf(stderr,"Waiting Source status%u line%d: %s\n",status,diagnostic.line,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked);xr_compile_session_free(session);
    XrXirCheckedPacket packet={0};XrXirArtifact *decoded=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_source_result_free(&source);
    XrXirDiagnostic detail={0};status=xr_xir_compile_specialize(decoded,&closed,&detail, NULL);
    if(status!=XR_XIR_OK) fprintf(stderr,"Waiting specialize status%u function%u block%u instruction%u\n",status,detail.function,detail.block,detail.instruction);
    CHECK(status==XR_XIR_OK);xr_xir_compile_artifact_free(decoded);
    CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}, NULL,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    const char *names[]={"make","probe","take","empty","make_relay","task_root","make_self","make_cycle","loop_break","loop_continue","loop_return","loop_throw","closed_count","make_units","loop_units","make_owned","loop_panic","loop_nested","loop_captures","loop_generic","loop_builtin","loop_task","loop_task_error","loop_cold"};
    uint32_t ids[24]={0};
    for(unsigned n=0;n<24;++n) for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==strlen(names[n]) && !memcmp(module->functions[f].name,names[n],strlen(names[n]))) ids[n]=f;
    for(unsigned n=0;n<24;++n) CHECK(ids[n]);
    for(uint32_t i=0;i<module->functions[ids[20]].instruction_count;++i)
        CHECK(module->functions[ids[20]].instructions[i].op!=XR_XIR_CLEANUP_REGISTER);
    XrXirCSource c={0};CHECK(xr_xir_compile_emit_c(lowered,"generator_loop",1048576,&c)==XR_XIR_OK);
    CHECK(c.text && c.length && !strstr(c.text,"({"));
    if(argc==2){FILE *file=fopen(argv[1],"wb");CHECK(file);CHECK(fwrite(c.text,1,c.length,file)==c.length);
        CHECK(fprintf(file,"\nconst uint32_t generator_loop_ids[24]={%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u};\n",ids[0],ids[1],ids[2],ids[3],ids[4],ids[5],ids[6],ids[7],ids[8],ids[9],ids[10],ids[11],ids[12],ids[13],ids[14],ids[15],ids[16],ids[17],ids[18],ids[19],ids[20],ids[21],ids[22],ids[23])>0);CHECK(!fclose(file));}
    xr_xir_compile_c_source_free(&c);XrXirProgram *program=NULL;
    XrCompileResourceStats before_take={0};CHECK(xr_compile_resources_stats(owner.context.resources,&before_take)==XR_COMPILE_RESOURCE_OK);
    status=xr_xir_compile_vm_program_take(&lowered,&program);
    if(status!=XR_XIR_OK) {
        XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(owner.context.resources,&stats)==XR_COMPILE_RESOURCE_OK);
        fprintf(stderr,"Loop Program take status%u before_work%llu work%llu allocated%llu live%llu\n",status,
            (unsigned long long)before_take.work,(unsigned long long)stats.work,
            (unsigned long long)stats.allocated_bytes,(unsigned long long)stats.live_bytes);
    }
    CHECK(status==XR_XIR_OK && program && !lowered);
    CHECK(xr_xir_compile_program_retain(program));generator_wait_cases(program,ids);
    generator_loop_cases(program,ids);xr_xir_compile_program_drop(program);source_fixture_owner_free(&owner);
    puts("Real Source time.sleep in Generator: exact waits and cancellation PASS");return 0;
}
