/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_method_factory_source.c - Real Source preserves ordinary method factory calls
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
#include "xir_generator_method_factory_cases.h"
#include "xir_generator_method_factory_promises.h"
static const char method_factory_source[]=
    "import time\n"
    "var completed:i64=0\n"
    "interface Provider<T>{iterator()->Iterator<T>}\n"
    "final class Producer<T> implements Provider<T> {\n"
    " const value:T\n"
    " constructor(value:T){this.value=value}\n"
    " iterator()->Iterator<T> {defer {completed=completed+1\n"
    " print(401)}\n"
    " time.sleep(5)\n"
    " yield this.value\n"
    " time.sleep(7)\n"
    " yield this.value}\n"
    " repeat<U>(value:U)->Iterator<U> {defer {completed=completed+1\n"
    " print(401)}\n"
    " time.sleep(5)\n"
    " yield value}\n"
    " static values(value:i64=10)->Iterator<i64> {defer {completed=completed+1\n"
    " print(401)}\n"
    " time.sleep(5)\n"
    " yield value}\n"
    "}\n"
    "fn escape()->fn()->Iterator<i64> {const receiver=Producer<i64>(6)\n"
    " return receiver.iterator}\n"
    "fn gather(it:Iterator<i64>)->i64 {var sum=0\n"
    " for(value in it){sum=sum+value}\n"
    " return sum}\n"
    "fn through<S:Provider<i64>>(receiver:S)->i64 {return gather(receiver.iterator())}\n"
    "fn bind<S:Provider<i64>>(receiver:S)->fn()->Iterator<i64> {return receiver.iterator}\n"
    "export fn bound()->i64 {const make=escape()\n"
    " return gather(make())}\n"
    "export fn witness()->i64 {return through(Producer<i64>(7))}\n"
    "export fn witness_value()->i64 {const make=bind(Producer<i64>(8))\n"
    " return gather(make())}\n"
    "export fn generic_value()->i64 {const receiver=Producer<i64>(0)\n"
    " const make=receiver.repeat<i64>\n"
    " return gather(make(9))}\n"
    "export fn static_value()->i64 {const make=Producer<i64>.values\n"
    " return gather(make(10))}\n"
    "export fn direct_default()->i64 {return gather(Producer<i64>.values())}\n"
    "struct Safe {static values(value:i64)->Iterator<i64> {defer {print(401)}\n"
    " time.sleep(5)\n"
    " yield value}}\n"
    "fn safe_worker()->i64 {const make=Safe.values\n"
    " try{return gather(make(11))} catch(error){return -1}}\n"
    "export fn worker()->i64 {return await go safe_worker()}\n"
    "fn cold_worker()->i64 {const make=Producer<i64>.values\n"
    " try{const unused=make(12)\n"
    " return 12} catch(error){return -1}}\n"
    "export fn cold()->i64 {return await go cold_worker()}\n"
    "fn forbidden_worker()->i64 {const make=Producer<i64>.values\n"
    " try{return gather(make(6))} catch(error){return -1}}\n"
    "export fn forbidden()->i64 {return await go forbidden_worker()}\n"
    "export fn stats()->i64 {return completed}\n"
    "fn main(){}\n";
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2);SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"generator-method-factory",NULL};
    XrXirSourceRequest request={session,NULL,&authority,&owner.context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,
        &(XrXirSourceText){NULL,method_factory_source,sizeof(method_factory_source)-1},&source,&diagnostic,NULL);
    if(status!=XR_XIR_OK) fprintf(stderr,"Boundary Source status%u line%d: %s\n",status,diagnostic.line,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked);xr_compile_session_free(session);
    XrXirCheckedPacket packet={0};XrXirArtifact *decoded=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_source_result_free(&source);
    CHECK(xr_xir_compile_specialize(decoded,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(decoded);
    CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    const char *names[]={"bound","witness","witness_value","generic_value","static_value","direct_default","worker","cold","forbidden","stats"};uint32_t ids[10]={0};
    for(unsigned n=0;n<10;++n) for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==strlen(names[n]) && !memcmp(module->functions[f].name,names[n],strlen(names[n]))) ids[n]=f;
    for(unsigned n=0;n<10;++n) CHECK(ids[n]);
    XrXirCSource c={0};CHECK(xr_xir_compile_emit_c(lowered,"generator_method_factory",1048576,&c)==XR_XIR_OK);
    CHECK(c.text && c.length && !strstr(c.text,"({"));
    if(argc==2) {FILE *file=fopen(argv[1],"wb");CHECK(file);CHECK(fwrite(c.text,1,c.length,file)==c.length);
        CHECK(fprintf(file,"\nconst uint32_t generator_method_factory_ids[10]={%u,%u,%u,%u,%u,%u,%u,%u,%u,%u};\n",ids[0],ids[1],ids[2],ids[3],ids[4],ids[5],ids[6],ids[7],ids[8],ids[9])>0);CHECK(!fclose(file));}
    xr_xir_compile_c_source_free(&c);XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && program && !lowered);
    generator_method_factory_cases(program,ids);xr_xir_compile_program_drop(program);
    generator_method_factory_promises();source_fixture_owner_free(&owner);return 0;
}
