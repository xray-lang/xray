/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_cell_borrow_source.c - Actual owned Cell captures and consumer ref loans
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
#include "xir_generator_cell_borrow_text.h"
#include "xir_generator_cell_weakened_text.h"
static uint32_t cell_borrow_entry(const XrXirModule *module,const char *name) {
    for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==strlen(name) && !memcmp(module->functions[f].name,name,strlen(name)))return f;
    return UINT32_MAX;
}
static void cell_borrow_execute(XrXirProgram *program,uint32_t entry,bool borrowed) {
    XrXirInstanceConfig config;XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,1000000);
    XrXirCallStatus expected=borrowed?XR_XIR_CALL_BAD_ARGUMENT:XR_XIR_CALL_RETURNED;
    if(result.outcome.status!=expected)fprintf(stderr,"owned Cell Source entry%u borrowed%u status%u expected%u\n",
        entry,borrowed?1u:0u,result.outcome.status,expected);
    CHECK(result.outcome.status==expected && !result.outcome.wake);
    if(!borrowed) {
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        CHECK(value.type==XR_XIR_I64 && value.payload==17);xr_xir_value_drop(&value);
    } else CHECK(!result.outcome.value.type);
    CHECK(xr_xir_instance_free(instance)==(borrowed?XR_XIR_CALL_BAD_ARGUMENT:XR_XIR_CALL_READY));
    printf("OWNED_CELL_SOURCE entry%u borrowed%u status%u PASS\n",entry,borrowed?1u:0u,result.outcome.status);
}
int main(int argc,char **argv) {
    bool weakened=argc>1 && !strcmp(argv[1],"--explicit-root");
    CHECK(weakened?(argc==2 || argc==3):(argc==1 || argc==2));
    const char *path=weakened?(argc==3?argv[2]:NULL):(argc==2?argv[1]:NULL);
    const char *text=weakened?cell_weakened_text:cell_borrow_text;
    size_t length=weakened?sizeof(cell_weakened_text)-1:sizeof(cell_borrow_text)-1;
    const char *prefix=weakened?"generator_cell_weakened":"generator_cell_borrow";
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"generator-cell-borrow",NULL};
    XrXirSourceRequest request={session,NULL,&authority,&owner.context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,
        &(XrXirSourceText){NULL,text,length},&source,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"owned Cell Source status%u line%d: %s\n",status,diagnostic.line,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked);xr_compile_session_free(session);
    XrXirCheckedPacket packet={0};XrXirArtifact *decoded=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&source);
    CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(decoded,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(decoded);
    CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    uint32_t ids[2]={cell_borrow_entry(module,"run_owned"),cell_borrow_entry(module,"run_borrowed")};
    CHECK(ids[0]!=UINT32_MAX && ids[1]!=UINT32_MAX);
    if(weakened) {
        uint32_t conversions=0;for(uint32_t f=0;f<module->function_count;++f)
            for(uint32_t i=0;i<module->functions[f].instruction_count;++i)
                conversions+=module->functions[f].instructions[i].op==XR_XIR_FUNCTION_WEAKEN;
        CHECK(conversions);printf("OWNED_CELL_WEAKEN Source actual_FUNCTION_WEAKEN=%u PASS\n",conversions);
    }
    XrXirCSource c={0};CHECK(xr_xir_compile_emit_c(lowered,prefix,1048576,&c)==XR_XIR_OK);
    CHECK(c.text && c.length && !strstr(c.text,"({"));
    if(path) {FILE *file=fopen(path,"wb");CHECK(file);CHECK(fwrite(c.text,1,c.length,file)==c.length);
        CHECK(fprintf(file,"\nconst uint32_t %s_ids[2]={%u,%u};\n",prefix,ids[0],ids[1])>0);CHECK(!fclose(file));}
    xr_xir_compile_c_source_free(&c);XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    cell_borrow_execute(program,ids[0],false);cell_borrow_execute(program,ids[1],true);xr_xir_compile_program_drop(program);
    source_fixture_owner_free(&owner);CHECK(!runtime_live && !runtime_bytes);
    puts(weakened?"OWNED_CELL_WEAKEN root UNKNOWN carrier returns17 and actual worker ref refusal, physical0 PASS":
        "OWNED_CELL_SOURCE genuine worker Cell/Fn/Generator success and actual ref consumer refusal, physical0 PASS");return 0;
}
