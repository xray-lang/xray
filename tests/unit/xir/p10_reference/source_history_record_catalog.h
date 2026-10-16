/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_history_record_catalog.h - Real Source record state through owned Catalog
 */
#ifndef SOURCE_HISTORY_RECORD_CATALOG_H
#define SOURCE_HISTORY_RECORD_CATALOG_H
static int history_record_catalog(void) {
    SourceFixtureOwner compiler={0};source_fixture_owner_new(&compiler);
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(compiler.context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_GENERATION_STDLIB};
    char path[4096];CHECK(snprintf(path,sizeof(path),"%s/record-catalog.xr",XR_GENERATION_STDLIB)>0);
    XrXirSourceRequest request={session,path,&authority,&compiler.context,NULL,NULL,XR_XIR_LIBRARY,NULL};
    const char text[]="var value:{apply:fn()->i64,items:Array<i64>}=make()\n"
        "fn make()->{apply:fn()->i64,items:Array<i64>}{var count:i64=41;return {apply:fn()->i64{count+=1;return count},items:[1]}}\n"
        "export fn answer()->i64{var copy=value;copy.items[0]=9;return copy.apply()+value.items[0]*10+copy.items[0]-19}";
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,
        &(XrXirSourceText){"record-catalog.xr",text,sizeof(text)-1},&result,&diagnostic,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"record Catalog Source status=%u %s\n",status,diagnostic.message);
    CHECK(status==XR_XIR_OK && result.checked);
    CHECK(xr_xir_compile_artifact_module(result.checked)->declarations->module_count==1);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(result.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);session=NULL;
    XrXirLibraryModuleInput module={authority,"record-catalog.xr"};
    XrXirLibraryInput input={packet.bytes,packet.length,{0},&module,1};
    xr_sha256(packet.bytes,packet.length,input.sha256);
    XrXirLibraryCatalog *catalog=NULL;
    status=xr_xir_compile_library_catalog_new_v2(&compiler.context,&input,1,&catalog);
    if (status!=XR_XIR_OK) fprintf(stderr,"record Catalog admission status=%u\n",status);
    CHECK(status==XR_XIR_OK && catalog);
    memset(packet.bytes,0,packet.length);xr_xir_compile_checked_packet_free(&packet);
    size_t count=0;const XrModuleResourceBinding *resources=xr_xir_compile_library_catalog_resources_v2(catalog,&count);
    CHECK(resources && count==1 && resources[0].checked);
    const XrXirModule *owned=xr_xir_compile_artifact_module(resources[0].checked);
    bool record=false;
    for (uint32_t t=0;t<owned->types->count;++t) record|=owned->types->nodes[t].kind==XR_XIR_TYPE_RECORD;
    CHECK(record && xr_xir_compile_artifact_verify(resources[0].checked,NULL)==XR_XIR_OK);
    CHECK(xr_compile_session_new(compiler.context.resources,&session)==XR_COMPILER_SESSION_OK);
    CHECK(snprintf(path,sizeof(path),"%s/record-catalog-root.xr",XR_GENERATION_STDLIB)>0);
    request=(XrXirSourceRequest){session,path,&authority,&compiler.context,NULL,NULL,XR_XIR_PROGRAM,catalog};
    const char root[]="import {answer as previous} from \"./record-catalog\"\nexport fn main()->i64{return previous()}";
    XrXirSourceResult consumer={0};
    status=xr_xir_compile_source_check_text(&request,
        &(XrXirSourceText){"record-catalog-root.xr",root,sizeof(root)-1},&consumer,&diagnostic,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"record Catalog import status=%u %s\n",status,diagnostic.message);
    CHECK(status==XR_XIR_OK && consumer.checked);xr_compile_session_free(session);
    xr_xir_compile_library_catalog_free(catalog);
    XrXirArtifact *closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_specialize(consumer.checked,&closed,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&consumer);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);owned=xr_xir_compile_artifact_module(lowered);
    uint32_t entry=UINT32_MAX;
    for (uint32_t f=0;f<owned->function_count;++f)
        if (owned->functions[f].name_length==4 && !memcmp(owned->functions[f].name,"main",4)) {
            CHECK(entry==UINT32_MAX);entry=f;
        }
    CHECK(entry!=UINT32_MAX);
    XrXirProgram *sealed=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&sealed)==XR_XIR_OK && !lowered);
    XrXirInstanceConfig runtime;CHECK(xr_xir_instance_config_init(&runtime,sizeof(runtime))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(sealed,&runtime,&instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(sealed);
    for (unsigned call=0;call<2;++call) {
        CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY && generation_poll(instance)==XR_XIR_CALL_RETURNED);
        XrXirValue answer={0};CHECK(xr_xir_instance_take_result(instance,&answer)==XR_XIR_CALL_RETURNED);
        CHECK(answer.type==XR_XIR_I64 && answer.payload==42+call);xr_xir_value_drop(&answer);
    }
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);source_fixture_owner_free(&compiler);
    puts("Owned Record Catalog preserved Source/packet/catalog/Program producer death,COW and actual closure42/43");return 0;
}
#endif // SOURCE_HISTORY_RECORD_CATALOG_H
