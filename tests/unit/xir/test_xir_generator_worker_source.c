/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_worker_source.c - Real Source distinguishes creation from worker execution
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
#include "xir_generator_worker_cases.h"
static const char worker_source[]=
    "import time\n"
    "var touched:i64=0\n"
    "fn module_items()->Iterator<i64> {touched=touched+1\n"
    " defer {print(401)}\n"
    " time.sleep(5)\n"
    " yield 7\n"
    "}\n"
    "export fn root_ok()->i64 {for(value in module_items()){return value}\n"
    " return 0}\n"
    "export fn count()->i64 {return touched}\n"
    "fn worker_direct()->i64 {for(value in module_items()){break}\n"
    " return 0}\n"
    "export fn bad_direct()->i64 {return await go worker_direct()}\n"
    "fn worker_local()->i64 {fn items()->Iterator<i64> {touched=touched+1\n"
    " defer {print(401)}\n"
    " time.sleep(5)\n"
    " yield 9}\n"
    " for(value in items()){break}\n"
    " return 0}\n"
    "export fn bad_local()->i64 {return await go worker_local()}\n"
    "fn transit()->Iterator<i64> {return module_items()}\n"
    "fn worker_transit()->i64 {for(value in transit()){break}\n"
    " return 0}\n"
    "export fn bad_transit()->i64 {return await go worker_transit()}\n"
    "fn worker_callback()->i64 {const factory=fn()->Iterator<i64> {return module_items()}\n"
    " for(value in factory()){break}\n"
    " return 0}\n"
    "export fn bad_callback()->i64 {return await go worker_callback()}\n"
    "fn owned_worker()->i64 {var done=0\n"
    " fn items()->Iterator<i64> {defer{done=done+1\n"
    " print(401)}\n"
    " time.sleep(5)\n"
    " yield 1\n"
    " done=1000}\n"
    " for(value in items()){break}\n"
    " return done}\n"
    "export fn good()->i64 {return await go owned_worker()}\n"
    "fn main(){}\n"
    "\n";
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2);SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"generator-worker-boundary",NULL};
    XrXirSourceRequest request={session,NULL,&authority,&owner.context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,
        &(XrXirSourceText){NULL,worker_source,sizeof(worker_source)-1},&source,&diagnostic,NULL);
    if(status!=XR_XIR_OK) fprintf(stderr,"Boundary Source status%u line%d: %s\n",status,diagnostic.line,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked);xr_compile_session_free(session);
    XrXirCheckedPacket packet={0};XrXirArtifact *decoded=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_source_result_free(&source);
    CHECK(xr_xir_compile_specialize(decoded,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(decoded);
    CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    const char *names[]={"root_ok","count","bad_direct","bad_local","bad_transit","bad_callback","good"};uint32_t ids[7]={0};
    for(unsigned n=0;n<7;++n) for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==strlen(names[n]) && !memcmp(module->functions[f].name,names[n],strlen(names[n]))) ids[n]=f;
    for(unsigned n=0;n<7;++n) CHECK(ids[n]);
    XrXirCSource c={0};CHECK(xr_xir_compile_emit_c(lowered,"generator_worker",1048576,&c)==XR_XIR_OK);
    CHECK(c.text && c.length && !strstr(c.text,"({"));
    if(argc==2) {FILE *file=fopen(argv[1],"wb");CHECK(file);CHECK(fwrite(c.text,1,c.length,file)==c.length);
        CHECK(fprintf(file,"\nconst uint32_t generator_worker_ids[7]={%u,%u,%u,%u,%u,%u,%u};\n",ids[0],ids[1],ids[2],ids[3],ids[4],ids[5],ids[6])>0);CHECK(!fclose(file));}
    xr_xir_compile_c_source_free(&c);XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && program && !lowered);
    worker_boundary_cases(program,ids);xr_xir_compile_program_drop(program);source_fixture_owner_free(&owner);return 0;
}
