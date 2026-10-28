/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_iterable_source.c - Real Source distinguishes creation from worker execution
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_source_fixture_owner.h"
#include "xir/xxir_source.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include "xir_generator_iterable_cases.h"
#include "xir_generator_iterable_rejections.h"
static const char iterable_source[]=
    "import time\n"
    "var created:i64=0\n"
    "var invoked:i64=0\n"
    "var closed:i64=0\n"
    "fn ticks(first:i64)->Iterator<i64> {defer {closed=closed+1\n"
    " print(401)}\n"
    " time.sleep(5)\n"
    " yield first\n"
    " time.sleep(7)\n"
    " yield first+1}\n"
    "final class Sequence {\n"
    " const first:i64\n"
    " init(first:i64){this.first=first}\n"
    " iterator()->Iterator<i64> {invoked=invoked+1\n"
    " return ticks(this.first)}\n"
    "}\n"
    "fn make()->Sequence {created=created+1\n"
    " return Sequence(6)}\n"
    "export fn run()->i64 {var sum=0\n"
    " for(value in make()){sum=sum+value}\n"
    " return sum}\n"
    "export fn stop()->i64 {for(value in make()){return value}\n"
    " return -1}\n"
    "export fn stats()->i64 {return created*100+invoked*10+closed}\n"
    "final class Lazy {\n"
    " const first:i64\n"
    " init(first:i64){this.first=first}\n"
    " iterator()->Iterator<i64> {defer {closed=closed+1\n"
    " print(401)}\n"
    " time.sleep(5)\n"
    " yield this.first\n"
    " time.sleep(7)\n"
    " yield this.first+1}\n"
    "}\n"
    "export fn lazy()->i64 {var sum=0\n"
    " for(value in Lazy(8)){sum=sum+value}\n"
    " return sum}\n"
    "interface StreamProvider<T>{iterator()->Iterator<T>}\n"
    "fn once<T>(value:T)->Iterator<T> {defer {closed=closed+1\n"
    " print(401)}\n"
    " time.sleep(5)\n"
    " yield value}\n"
    "struct ValueBox<T> implements StreamProvider<T> {\n"
    " value:T\n"
    " init(value:T){this.value=value}\n"
    " iterator()->Iterator<T> {invoked=invoked+1\n"
    " return once(this.value)}\n"
    "}\n"
    "fn collect<S:StreamProvider<i64>>(receiver:S)->i64 {var sum=0\n"
    " for(value in receiver){sum=sum+value}\n"
    " return sum}\n"
    "export fn generic()->i64 {return collect(ValueBox<i64>(19))}\n"
    "struct Delayed {\n"
    " iterator()->Iterator<i64> {invoked=invoked+1\n"
    " time.sleep(3)\n"
    " return ticks(10)}\n"
    "}\n"
    "export fn delayed()->i64 {var sum=0\n"
    " for(value in Delayed()){sum=sum+value}\n"
    " return sum}\n"
    "enum FactoryError{Bad}\n"
    "struct Broken {iterator()->Iterator<i64> {throw FactoryError.Bad}}\n"
    "export fn failed()->i64 {try {for(value in Broken()){return value}} catch(error){return 17}\n"
    " return -1}\n"
    "export fn explicit()->i64 {const receiver=Lazy(9)\n"
    " const it=receiver.iterator()\n"
    " for(value in it){return value}\n"
    " return -1}\n"
    "struct RefSequence {\n"
    " first:i64\n"
    " init(first:i64){this.first=first}\n"
    " ref iterator()->Iterator<i64> {this.first=this.first+1\n"
    " invoked=invoked+1\n"
    " return ticks(this.first)}\n"
    "}\n"
    "export fn reference()->i64 {var receiver=RefSequence(6)\n"
    " for(value in receiver){break}\n"
    " return receiver.first}\n"
    "var selections:i64=0\n"
    "fn choose()->i64 {selections=selections+1\n"
    " return 0}\n"
    "export fn select_path()->i64 {var receivers=[RefSequence(6)]\n"
    " for(value in receivers[choose()]){break}\n"
    " return receivers[0].first*10+selections}\n"
    "fn main(){}\n"
    "\n";
#include "xir_generator_iterable_query.h"
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2);iterable_rejections();SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"generator-custom-iterable",NULL};
    XrXirSourceRequest request={session,NULL,&authority,&owner.context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,
        &(XrXirSourceText){NULL,iterable_source,sizeof(iterable_source)-1},&source,&diagnostic,NULL);
    if(status!=XR_XIR_OK) fprintf(stderr,"Boundary Source status%u line%d: %s\n",status,diagnostic.line,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked);xr_compile_session_free(session);iterable_query(source.snapshot);
    XrXirCheckedPacket packet={0};XrXirArtifact *decoded=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_source_result_free(&source);
    CHECK(xr_xir_compile_specialize(decoded,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(decoded);
    CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}, NULL,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    const char *names[]={"run","stop","stats","lazy","generic","delayed","failed","explicit","reference","select_path"};uint32_t ids[10]={0};
    for(unsigned n=0;n<10;++n) for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==strlen(names[n]) && !memcmp(module->functions[f].name,names[n],strlen(names[n]))) ids[n]=f;
    for(unsigned n=0;n<10;++n) CHECK(ids[n]);
    XrXirCSource c={0};CHECK(xr_xir_compile_emit_c(lowered,"generator_iterable",1048576,&c)==XR_XIR_OK);
    CHECK(c.text && c.length && !strstr(c.text,"({"));
    if(argc==2) {FILE *file=fopen(argv[1],"wb");CHECK(file);CHECK(fwrite(c.text,1,c.length,file)==c.length);
        CHECK(fprintf(file,"\nconst uint32_t generator_iterable_ids[10]={%u,%u,%u,%u,%u,%u,%u,%u,%u,%u};\n",ids[0],ids[1],ids[2],ids[3],ids[4],ids[5],ids[6],ids[7],ids[8],ids[9])>0);CHECK(!fclose(file));}
    xr_xir_compile_c_source_free(&c);XrXirProgram *program=NULL;
    XrCompileResourceStats before_take={0};CHECK(xr_compile_resources_stats(owner.context.resources,&before_take)==XR_COMPILE_RESOURCE_OK);
    status=xr_xir_compile_vm_program_take(&lowered,&program);
    if(status!=XR_XIR_OK) {
        XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(owner.context.resources,&stats)==XR_COMPILE_RESOURCE_OK);
        fprintf(stderr,"Iterable Program take status%u before_work%llu work%llu allocated%llu live%llu\n",status,
            (unsigned long long)before_take.work,(unsigned long long)stats.work,
            (unsigned long long)stats.allocated_bytes,(unsigned long long)stats.live_bytes);
    }
    CHECK(status==XR_XIR_OK && program && !lowered);
    generator_iterable_cases(program,ids);xr_xir_compile_program_drop(program);source_fixture_owner_free(&owner);return 0;
}
