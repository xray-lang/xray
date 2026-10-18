/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_consumer_source.c - Authentic Source factories create independent frames
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
#include "xir_generator_consumer_cases.h"
_Static_assert(XR_XIR_ITERATOR_HAS_NEXT==186 && XR_XIR_ITERATOR_NEXT==187,"canonical consumer ordinals");
static const char factory_source[]=
    "fn numbers()->Iterator<i64> {defer {print(301)}\n"
    " yield 11\n"
    " Coro.yield()\n"
    " yield 22\n"
    " }\n"
    "export fn run()->i64 {const it=numbers()\n"
    " if (!it.hasNext()) {return -1}\n"
    " if (!it.hasNext()) {return -2}\n"
    " const a=it.next()\n"
    " if (!it.hasNext()) {return -3}\n"
    " const b=it.next()\n"
    " if (it.hasNext()) {return -4}\n"
    " if (it.hasNext()) {return -5}\n"
    " return a+b}\n"
    "fn units()->Iterator<()> {defer {print(302)}\n"
    " yield ()\n"
    " yield ()\n"
    " }\n"
    "export fn unit_run()->i64 {const it=units()\n"
    " if (!it.hasNext()) {return -1}\n"
    " it.next()\n"
    " if (!it.hasNext()) {return -2}\n"
    " it.next()\n"
    " if (it.hasNext()) {return -3}\n"
    " return 42}\n"
    "fn owned()->Iterator<string> {defer {print(303)}\n"
    " yield \"owned\"\n"
    " }\n"
    "export fn owned_run()->string {const it=owned()\n"
    " if (!it.hasNext()) {return \"bad\"}\n"
    " return it.next()}\n"
    "enum E { Bad }\n"
    "fn failer()->Iterator<i64> {defer {print(304)}\n"
    " yield 7\n"
    " throw E.Bad\n"
    " }\n"
    "export fn fail_run()->i64 {const it=failer()\n"
    " var total=it.next()\n"
    " try {it.next()} catch(error) {total=total+10}\n"
    " try {it.hasNext()} catch(error) {return total+20}\n"
    " return -1}\n"
    "export fn exhaust_run()->i64 {const it=numbers()\n"
    " it.next()\n"
    " it.next()\n"
    " if (it.hasNext()) {return -1}\n"
    " return it.next()}\n"
    "export fn make()->Iterator<i64> {return numbers()}\n"
    "export fn probe(it:Iterator<i64>)->bool {return it.hasNext()}\n"
    "export fn take(it:Iterator<i64>)->i64 {return it.next()}\n"
    "fn main(){}\n";
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2);SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"generator-consumer",NULL};
    XrXirSourceRequest request={session,NULL,&authority,&owner.context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,
        &(XrXirSourceText){NULL,factory_source,sizeof(factory_source)-1},&source,&diagnostic,NULL);
    if(status!=XR_XIR_OK) fprintf(stderr,"Factory Source status%u line%d: %s\n",status,diagnostic.line,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked);xr_compile_session_free(session);
    XrXirCheckedPacket packet={0};XrXirArtifact *decoded=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_source_result_free(&source);
    XrXirDiagnostic detail={0};status=xr_xir_compile_specialize(decoded,&closed,&detail);
    if(status!=XR_XIR_OK) fprintf(stderr,"Factory specialize status%u function%u block%u instruction%u\n",status,detail.function,detail.block,detail.instruction);
    CHECK(status==XR_XIR_OK);xr_xir_compile_artifact_free(decoded);
    CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    const char *names[]={"run","unit_run","owned_run","fail_run","exhaust_run","make","probe","take"};
    uint32_t ids[8]={0};
    for(unsigned n=0;n<8;++n) for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==strlen(names[n]) && !memcmp(module->functions[f].name,names[n],strlen(names[n]))) ids[n]=f;
    for(unsigned n=0;n<8;++n) CHECK(ids[n]);
    XrXirCSource c={0};CHECK(xr_xir_compile_emit_c(lowered,"generator_consumer",1048576,&c)==XR_XIR_OK);
    CHECK(c.text && c.length && !strstr(c.text,"({"));
    if(argc==2){FILE *file=fopen(argv[1],"wb");CHECK(file);CHECK(fwrite(c.text,1,c.length,file)==c.length);CHECK(fprintf(file,"\nconst uint32_t generator_consumer_ids[8]={%u,%u,%u,%u,%u,%u,%u,%u};\n",
        ids[0],ids[1],ids[2],ids[3],ids[4],ids[5],ids[6],ids[7])>0);CHECK(!fclose(file));}
    xr_xir_compile_c_source_free(&c);XrXirProgram *program=NULL;
    XrXirStatus receiving=xr_xir_compile_vm_program_take(&lowered,&program);
    if(receiving!=XR_XIR_OK) fprintf(stderr,"Consumer Program receiving status%u\n",receiving);
    CHECK(receiving==XR_XIR_OK && program && !lowered);
    generator_consumer_cases(program,ids);source_fixture_owner_free(&owner);
    puts("Source Iterator cached hasNext and exact next with Coro scheduling PASS");return 0;
}
