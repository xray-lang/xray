/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_factory_source.c - Authentic Source factories create independent frames
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
#include "xir_generator_factory_cases.h"
#include "xir_generator_factory85_golden.h"
_Static_assert(XR_XIR_GENERATOR_NEW==177 && XR_XIR_TYPE_ITERATOR==11,"independent factory protocol ordinals");
static void factory_packet_models(const XrXirCompileContext *context) {
    const struct {const uint8_t *bytes;size_t length;XrXirStatus expected;} cases[]={
        {factory85_i64,sizeof(factory85_i64),XR_XIR_OK},
        {factory85_unit,sizeof(factory85_unit),XR_XIR_OK},
        {factory85_no_suspend,sizeof(factory85_no_suspend),XR_XIR_OK},
        {factory85_ordinary_target,sizeof(factory85_ordinary_target),XR_XIR_BAD_TYPE},
        {factory85_scalar_result,sizeof(factory85_scalar_result),XR_XIR_BAD_TYPE},
        {factory85_argument_count,sizeof(factory85_argument_count),XR_XIR_BAD_STRUCTURE},
        {factory85_targets,sizeof(factory85_targets),XR_XIR_BAD_STRUCTURE},
        {factory85_yield_in_factory,sizeof(factory85_yield_in_factory),XR_XIR_BAD_TYPE}};
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        XrXirArtifact *decoded=NULL;XrXirStatus status=xr_xir_compile_checked_read(context,cases[i].bytes,cases[i].length,&decoded,NULL);
        if(status!=cases[i].expected) fprintf(stderr,"Factory packet%u status%u expected%u\n",i,status,cases[i].expected);
        CHECK(status==cases[i].expected);
        if(status==XR_XIR_OK) {
            XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(decoded,&packet,NULL)==XR_XIR_OK);
            CHECK(packet.length==cases[i].length && !memcmp(packet.bytes,cases[i].bytes,packet.length));
            xr_xir_compile_checked_packet_free(&packet);XrXirArtifact *lowered=NULL;
            CHECK(xr_xir_compile_lower(decoded,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},&lowered,NULL)==XR_XIR_OK);
            CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(lowered);
        } else CHECK(!decoded);
        xr_xir_compile_artifact_free(decoded);
    }
}

static const char factory_source[]=
    "fn numbers(seed:i64)->Iterator<i64> { defer {print(seed)}\n yield seed\n }\n"
    "export fn make(seed:i64)->Iterator<i64> {return numbers(seed)}\n"
    "export fn discard(){numbers(9)}\n"
    "fn main(){}\n";
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2);SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"generator-factory",NULL};
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
    CHECK(module->function_count>3 && module->functions[2].name_length==4 && !memcmp(module->functions[2].name,"make",4) &&
        module->functions[3].name_length==7 && !memcmp(module->functions[3].name,"discard",7));
    uint32_t factories=0;for(uint32_t f=0;f<module->function_count;++f)
        for(uint32_t i=0;i<module->functions[f].instruction_count;++i)
            factories+=module->functions[f].instructions[i].op==XR_XIR_GENERATOR_NEW;
    CHECK(factories==2);
    XrXirCSource c={0};CHECK(xr_xir_compile_emit_c(lowered,"generator_factory",1048576,&c)==XR_XIR_OK);
    CHECK(c.text && c.length && !strstr(c.text,"({"));
    if(argc==2){FILE *file=fopen(argv[1],"wb");CHECK(file);CHECK(fwrite(c.text,1,c.length,file)==c.length);CHECK(!fclose(file));}
    xr_xir_compile_c_source_free(&c);XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && program && !lowered);
    generator_factory_cases(program);factory_packet_models(&owner.context);source_fixture_owner_free(&owner);
    puts("Source canonical factory creates owned parked Iterator; drop and Instance close retire frames before final code PASS");return 0;
}
