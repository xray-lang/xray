/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_receiver_source.c - Real Source keeps complete bound receiver environments
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
#include "xir_generator_receiver_cases.h"
#include "xir_generator_receiver_bounds.h"
static const char receiver_source[]=
    "import time\n"
    "struct Value {value:i64\n"
    " iterator()->Iterator<i64> {defer {print(401)}\n"
    " time.sleep(5)\n"
    " yield this.value}}\n"
    "final class Object {const value:i64\n"
    " init(value:i64){this.value=value}\n"
    " iterator()->Iterator<i64> {defer {print(401)}\n"
    " time.sleep(5)\n"
    " yield this.value}}\n"
    "fn gather(it:Iterator<i64>)->i64 {var sum=0\n"
    " for(value in it){sum=sum+value}\n"
    " return sum}\n"
    "fn value_escape()->Value {return Value{value:13}}\n"
    "fn object_escape()->Object {return Object(14)}\n"
    "fn object_bind(receiver:Object)->i64 {const make=receiver.iterator\n"
    " try{return gather(make())} catch(error){return -1}}\n"
    "fn value_worker()->i64 {const receiver=Value{value:11}\n"
    " const make=receiver.iterator\n"
    " try{return gather(make())} catch(error){return -1}}\n"
    "fn object_worker()->i64 {const receiver=Object(12)\n"
    " const make=receiver.iterator\n"
    " try{return gather(make())} catch(error){return -1}}\n"
    "fn value_return_worker()->i64 {const receiver=value_escape()\n"
    " const make=receiver.iterator\n"
    " try{return gather(make())} catch(error){return -1}}\n"
    "fn object_return_worker()->i64 {const receiver=object_escape()\n"
    " const make=receiver.iterator\n"
    " try{return gather(make())} catch(error){return -1}}\n"
    "fn object_phi_worker()->i64 {const left=Object(0)\n"
    " const right=Object(15)\n"
    " const make=true ? right.iterator : left.iterator\n"
    " try{return gather(make())} catch(error){return -1}}\n"
    "fn object_parameter_worker()->i64 {return object_bind(Object(16))}\n"
    "fn cell_worker()->i64 {const receiver=Object(19)\n"
    " var make=receiver.iterator\n"
    " make=receiver.iterator\n"
    " try{const unused=make()\n"
    " return 0} catch(error){return -1}}\n"
    "export fn cell_cold()->i64{return await go cell_worker()}\n"
    "export fn value_direct()->i64{return await go value_worker()}\n"
    "export fn object_direct()->i64{return await go object_worker()}\n"
    "export fn value_return()->i64{return await go value_return_worker()}\n"
    "export fn object_return()->i64{return await go object_return_worker()}\n"
    "export fn object_phi()->i64{return await go object_phi_worker()}\n"
    "export fn object_parameter()->i64{return await go object_parameter_worker()}\n"
    "fn main(){}\n";
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2);
    receiver_opaque_return_cases();
    if(argc==2 && !strcmp(argv[1],"--opaque-bounds")) return 0;
    if(argc==2 && !strcmp(argv[1],"--origin-controls")) {receiver_origin_cases();return 0;}
    bool cell=argc==2 && !strcmp(argv[1],"--cell");SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"generator-receiver-captures",NULL};
    XrXirSourceRequest request={session,NULL,&authority,&owner.context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,
        &(XrXirSourceText){NULL,receiver_source,sizeof(receiver_source)-1},&source,&diagnostic,NULL);
    if(status!=XR_XIR_OK) {
        XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(owner.context.resources,&stats)==XR_COMPILE_RESOURCE_OK);
        fprintf(stderr,"Boundary Source status%u line%d work%llu allocated%llu peak%llu live%llu: %s\n",status,diagnostic.line,
            (unsigned long long)stats.work,(unsigned long long)stats.allocated_bytes,
            (unsigned long long)stats.peak_bytes,(unsigned long long)stats.live_bytes,diagnostic.message);
    }
    CHECK(status==XR_XIR_OK && source.checked);xr_compile_session_free(session);
    XrXirCheckedPacket packet={0};XrXirArtifact *decoded=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_source_result_free(&source);
    CHECK(xr_xir_compile_specialize(decoded,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(decoded);
    CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    const char *names[]={"value_direct","object_direct","value_return","object_return","object_phi","object_parameter","cell_cold"};uint32_t ids[7]={0};
    for(unsigned n=0;n<7;++n) for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==strlen(names[n]) && !memcmp(module->functions[f].name,names[n],strlen(names[n]))) ids[n]=f;
    for(unsigned n=0;n<7;++n) CHECK(ids[n]);
    XrXirCSource c={0};CHECK(xr_xir_compile_emit_c(lowered,"generator_receiver",1048576,&c)==XR_XIR_OK);
    CHECK(c.text && c.length && !strstr(c.text,"({"));
    if(argc==2 && !cell) {FILE *file=fopen(argv[1],"wb");CHECK(file);CHECK(fwrite(c.text,1,c.length,file)==c.length);
        CHECK(fprintf(file,"\nconst uint32_t generator_receiver_ids[7]={%u,%u,%u,%u,%u,%u,%u};\n",ids[0],ids[1],ids[2],ids[3],ids[4],ids[5],ids[6])>0);CHECK(!fclose(file));}
    xr_xir_compile_c_source_free(&c);XrXirProgram *program=NULL;
    XrCompileResourceStats before_take={0};CHECK(xr_compile_resources_stats(owner.context.resources,&before_take)==XR_COMPILE_RESOURCE_OK);
    status=xr_xir_compile_vm_program_take(&lowered,&program);
    if(status!=XR_XIR_OK) {
        XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(owner.context.resources,&stats)==XR_COMPILE_RESOURCE_OK);
        fprintf(stderr,"Receiver Program take status%u before_work%llu work%llu allocated%llu live%llu\n",status,
            (unsigned long long)before_take.work,
            (unsigned long long)stats.work,(unsigned long long)stats.allocated_bytes,(unsigned long long)stats.live_bytes);
    }
    CHECK(status==XR_XIR_OK && program && !lowered);
    if(cell) generator_receiver_cell_case(program,ids[6]);else generator_receiver_cases(program,ids);xr_xir_compile_program_drop(program);
    source_fixture_owner_free(&owner);return 0;
}
