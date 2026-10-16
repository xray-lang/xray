/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_closure_source.c - Real closure factories retain latent owned producer captures
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
#include "xir_generator_cell_escape_cases.h"
_Static_assert(XR_XIR_ITERATOR_HAS_NEXT==178 && XR_XIR_ITERATOR_NEXT==179,"canonical consumer ordinals");
static const char factory_source[]=
    "import time\n"
    "var saved:Iterator<string>?=null\n"
    "fn delayed()->Iterator<string> {defer {print(401)}\n"
    " time.sleep(5)\n"
    " yield \"one\"\n"
    " time.sleep(7)\n"
    " yield \"two\"\n"
    " }\n"
    "export fn make()->Iterator<string> {const first=\"one\"\n"
    " const second=\"two\"\n"
    " fn local()->Iterator<string> {defer {print(401)}\n"
    " time.sleep(5)\n yield first\n time.sleep(7)\n yield second\n }\n"
    " return local()}\n"
    "export fn make_expr()->Iterator<string> {const first=\"one\"\n"
    " const second=\"two\"\n"
    " const local=fn()->Iterator<string> {defer {print(401)}\n"
    " time.sleep(5)\n yield first\n time.sleep(7)\n yield second\n }\n"
    " return local()}\n"
    "fn generic_factory<T>(first:T,second:T)->Iterator<T> {\n"
    " fn local()->Iterator<T> {defer {print(401)}\n"
    " time.sleep(5)\n yield first\n time.sleep(7)\n yield second\n }\n"
    " return local()}\n"
    "export fn make_generic()->Iterator<string> {return generic_factory(\"one\",\"two\")}\n"    "export fn make_array()->Iterator<string> {var values=[\"one\",\"two\"]\n"
    " const snapshot=values\n"
    " fn local()->Iterator<string> {defer {print(401)}\n"
    " time.sleep(5)\n yield snapshot[0]\n time.sleep(7)\n yield snapshot[1]\n }\n"
    " const it=local()\n values[0]=\"changed\"\n return it}\n"    "export fn make_recursive()->Iterator<string> {const first=\"one\"\n const second=\"two\"\n"
    " fn local(depth:i64)->Iterator<string> {defer {print(401)}\n"
    " if(depth>0) {const unstarted=local(0)}\n"
    " time.sleep(5)\n yield first\n time.sleep(7)\n yield second\n }\n"
    " return local(1)}\n"    "export fn make_owned_cell()->Iterator<string> {var first=\"one\"\n var second=\"two\"\n"
    " fn local()->Iterator<string> {defer {first=\"closed\"\n print(401)}\n"
    " time.sleep(5)\n yield first\n time.sleep(7)\n yield second\n }\n"
    " return local()}\n"    "export fn probe(it:Iterator<string>)->bool {return it.hasNext()}\n"
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
    "fn main(){}\n";
static void closure_rejections(SourceFixtureOwner *owner) {
    const char *cases[]={
        "fn make(x:ref i64)->Iterator<i64> {const f=fn()->Iterator<i64>{yield x}\n return f()}\n fn main(){}",
        "fn main(){const f=fn()->Iterator<i64>{yield \"bad\"}}",
        "fn main(){const f=fn()->i64{yield 1\n return 1}}"
    };
    for(unsigned i=0;i<3;++i) {
        XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(owner->context.resources,&session)==XR_COMPILER_SESSION_OK);
        XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"generator-closure-negative",NULL};
        XrXirSourceRequest request={session,NULL,&authority,&owner->context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
        XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_compile_source_check_text(&request,
            &(XrXirSourceText){NULL,cases[i],strlen(cases[i])},&source,&diagnostic,NULL);
        if(status!=XR_XIR_BAD_TYPE) fprintf(stderr,"Closure rejection%u status%u: %s\n",i,status,diagnostic.message);
        CHECK(status==XR_XIR_BAD_TYPE && !source.checked);
        if(!i) CHECK(strstr(diagnostic.message,"external ref"));
        xr_xir_compile_source_result_free(&source);xr_compile_session_free(session);
    }
}
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2);SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);closure_rejections(&owner);
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
    XrXirDiagnostic detail={0};status=xr_xir_compile_specialize(decoded,&closed,&detail);
    if(status!=XR_XIR_OK) fprintf(stderr,"Waiting specialize status%u function%u block%u instruction%u\n",status,detail.function,detail.block,detail.instruction);
    CHECK(status==XR_XIR_OK);xr_xir_compile_artifact_free(decoded);
    CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    const char *names[]={"make","probe","take","empty","make_relay","task_root","make_self","make_cycle","make_expr","make_generic","make_array","make_recursive","make_owned_cell"};
    uint32_t ids[13]={0};
    for(unsigned n=0;n<13;++n) for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==strlen(names[n]) && !memcmp(module->functions[f].name,names[n],strlen(names[n]))) ids[n]=f;
    for(unsigned n=0;n<13;++n) CHECK(ids[n]);
    XrXirCSource c={0};CHECK(xr_xir_compile_emit_c(lowered,"generator_closure",1048576,&c)==XR_XIR_OK);
    CHECK(c.text && c.length && !strstr(c.text,"({"));
    if(argc==2){FILE *file=fopen(argv[1],"wb");CHECK(file);CHECK(fwrite(c.text,1,c.length,file)==c.length);
        CHECK(fprintf(file,"\nconst uint32_t generator_closure_ids[13]={%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u};\n",ids[0],ids[1],ids[2],ids[3],ids[4],ids[5],ids[6],ids[7],ids[8],ids[9],ids[10],ids[11],ids[12])>0);CHECK(!fclose(file));}
    xr_xir_compile_c_source_free(&c);XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && program && !lowered);
    uint32_t factories[]={ids[0],ids[8],ids[9],ids[10],ids[11],ids[12]};
    for(unsigned i=0;i<3;++i) {CHECK(xr_xir_compile_program_retain(program));ids[0]=factories[i];generator_wait_cases(program,ids);}
    closure_cell_escape_case(program,ids);source_fixture_owner_free(&owner);
    puts("Real Source time.sleep in Generator: exact waits and cancellation PASS");return 0;
}
