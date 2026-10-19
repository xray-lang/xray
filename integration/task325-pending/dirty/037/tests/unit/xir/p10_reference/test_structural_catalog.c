/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_structural_catalog.c - Owned structural declarations through real library reception
 */
#include "xir/xxir_source.h"
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)
#include "../xir_source_fixture_owner.h"
static const char library_text[]=
    "export fn read<T:{items:Array<i64>} & {apply:fn(i64)->i64}>(value:T)->i64{return value.apply(value.items[1])}\n";
static const char consumer_text[]=
    "import {read as fromPacket} from \"./structural-catalog\"\n"
    "struct Padding{flag:bool}\n"
    "fn plus(value:i64)->i64{return value+1}\n"
    "export fn referenceAnswer()->i64{const value:{a:bool,apply:fn(i64)->i64,items:Array<i64>}="
    "{a:true,items:[7,41],apply:plus};return fromPacket(value)}\n";
static void catalog_run(XrXirArtifact *lowered) {
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t entry=UINT32_MAX;
    for (uint32_t f=0;f<module->function_count;++f)
        if (module->functions[f].name_length==15 && !memcmp(module->functions[f].name,"referenceAnswer",15)) {
            CHECK(entry==UINT32_MAX);entry=f;
        }
    CHECK(entry!=UINT32_MAX);XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue answer={0};CHECK(xr_xir_instance_take_result(instance,&answer)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    CHECK(answer.type==XR_XIR_I64 && answer.payload==42);xr_xir_value_drop(&answer);
}
int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==3 && !strcmp(argv[1],"--emit")));
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILER_SESSION_OK);
    const XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_LANGUAGE_FIXTURES};
    char path[4096];int length=snprintf(path,sizeof(path),"%s/structural-catalog.xr",XR_LANGUAGE_FIXTURES);
    CHECK(length>0 && (size_t)length<sizeof(path));
    XrXirSourceRequest request={session,path,&authority,&owner.context,NULL,NULL,XR_XIR_LIBRARY,NULL};
    XrXirSourceResult producer={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,
        &(XrXirSourceText){"structural-catalog.xr",library_text,sizeof(library_text)-1},&producer,&diagnostic,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"structural Catalog producer=%u %s\n",status,diagnostic.message);
    CHECK(status==XR_XIR_OK && producer.checked);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(producer.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&producer);xr_compile_session_free(session);session=NULL;
    XrXirLibraryModuleInput binding={authority,"structural-catalog.xr"};
    XrXirLibraryInput input={packet.bytes,packet.length,{0},&binding,1};xr_sha256(packet.bytes,packet.length,input.sha256);
    XrXirLibraryCatalog *catalog=NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(&owner.context,&input,1,&catalog)==XR_XIR_OK && catalog);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    size_t count=0;const XrModuleResourceBinding *resources=xr_xir_compile_library_catalog_resources_v2(catalog,&count);
    CHECK(count==1 && resources && resources[0].checked);
    const XrXirModule *module=xr_xir_compile_artifact_module(resources[0].checked);uint32_t contracts=0;
    for (uint32_t f=0;f<module->function_count;++f)
        for (uint32_t p=0;module->generics && p<module->generics[f].parameter_count;++p) {
            const XrXirConstraint *constraint=&module->generics[f].constraints[p];
            for (uint32_t r=0;r<constraint->structural_count;++r) {
                CHECK(constraint->structural_requirements[r].policy==XR_XIR_STRUCTURAL_READONLY_RECEIVER);++contracts;
            }
        }
    CHECK(contracts==2 && xr_xir_compile_artifact_verify(resources[0].checked,NULL)==XR_XIR_OK);
    CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILER_SESSION_OK);
    length=snprintf(path,sizeof(path),"%s/structural-catalog-root.xr",XR_LANGUAGE_FIXTURES);
    CHECK(length>0 && (size_t)length<sizeof(path));
    request=(XrXirSourceRequest){session,path,&authority,&owner.context,NULL,NULL,XR_XIR_PROGRAM,catalog};
    XrXirSourceResult consumer={0};status=xr_xir_compile_source_check_text(&request,
        &(XrXirSourceText){"structural-catalog-root.xr",consumer_text,sizeof(consumer_text)-1},&consumer,&diagnostic,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"structural Catalog consumer=%u %s\n",status,diagnostic.message);
    CHECK(status==XR_XIR_OK && consumer.checked);
    XrXirCheckedPacket received={0};CHECK(xr_xir_compile_checked_write(consumer.checked,&received,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&consumer);xr_compile_session_free(session);
    xr_xir_compile_library_catalog_free(catalog);
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(&owner.context,received.bytes,received.length,&checked,NULL)==XR_XIR_OK);
    memset(received.bytes,0xa5,received.length);xr_xir_compile_checked_packet_free(&received);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    if (argc==3) {
        XrXirCSource source={0};CHECK(xr_xir_compile_emit_c(lowered,"language_reference",1048576,&source)==XR_XIR_OK);
        FILE *file=fopen(argv[2],"wb");CHECK(file && fwrite(source.text,1,source.length,file)==source.length && !fclose(file));
        xr_xir_compile_c_source_free(&source);
    }
    catalog_run(lowered);source_fixture_owner_free(&owner);
    puts("Owned structural Catalog preserved declarations, producer destruction and real function-field execution42");return 0;
}
