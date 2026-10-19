/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_library_source.c - Entryless Checked Library source import
 *
 * KEY CONCEPT:
 *   Owned source Checked and packet consumers execute independently after producer destruction.
 */
#include "base/xmalloc.h"
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#include "xir_library_compile_owner.h"
static const XrXirCompileContext *library_context;
#define source_attempts source_program_compile_attempts
#define source_fail_at source_program_compile_fail_at
#define source_live source_program_compile_live
#define source_bytes source_program_compile_bytes
#include "xir/xxir_source_query.c"
#include "xir/xxir_type_inference.c"
#include "xir/xxir_source.c"
static XrCompilerSession *library_session_new(const XrXirCompileContext *context) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);return session;
}
static XrModuleResolver *library_resolver_new(const XrXirCompileContext *context,const XrModuleResolverConfig *config) {
    XrModuleResolver *resolver=NULL;CHECK(xr_compile_module_resolver_new(context->resources,config,&resolver)==XR_MODULE_OK);return resolver;
}
#include "xir_library_source_runtime.h"
#include "xir/xxir_library_catalog.h"
#include "base/xsha256.h"
#include "xir_library_source_owner.h"

static void catalog_thresholds(const XrXirLibraryInput *input) {
    LibrarySourceFixture fixture={input,1,NULL,{0},XR_XIR_LIBRARY};
    library_compile_operation_cases("Library catalog exact/minus1",library_catalog_operation,&fixture);
}
#include "xir_library_reader_cases.h"
#include "xir_defaults_wire_cases.h"
#include "xir_library_map_cases.h"
#include "xir_library_goldens.h"
#include "xir_library_source_budget_cases.h"
#include "xir_library_string_catalog_cases.h"
static void conflict_text(const char *name,const char *text) {
    char path[1024];CHECK(snprintf(path,sizeof(path),"%s/%s",XR_SOURCE_FIXTURES,name)>0);
    FILE *file=fopen(path,"wb");CHECK(file);size_t n=strlen(text);CHECK(fwrite(text,1,n,file)==n&&!fclose(file));
}
#include "xir_library_dispatch_cases.h"
#include "xir_library_views_cases.h"
#include "xir_library_typed_closed_cases.h"
#include "xir_library_construction_cases.h"
#include "xir_library_generic_parent_cases.h"
#include "xir_library_member_witness_cases.h"
#include "xir_library_carrier_member_cases.h"
#include "xir_library_enum_owned_cases.h"
#include "xir_library_class_owned_cases.h"
static void source_representation_conflicts(void) {
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    for(unsigned mode=0;mode<2;++mode){
        conflict_text("collision.xr","export fn answer()->i64 { return 41; }");
        XrCompilerSession *producer=library_session_new(library_context);CHECK(producer);
        XrXirSourceRequest request={producer,XR_SOURCE_FIXTURES "/collision.xr",&authority,library_context,NULL,NULL,XR_XIR_LIBRARY,NULL};
        XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
        CHECK(xr_xir_compile_source_check(&request,&result,&diagnostic,NULL)==XR_XIR_OK);
        XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(result.checked,&packet,NULL)==XR_XIR_OK);
        xr_xir_compile_source_result_free(&result);xr_compile_session_free(producer);
        XrXirLibraryInput input={packet.bytes,packet.length,{0}, (XrXirLibraryModuleInput[]){{authority,"collision.xr"}},1};
        xr_sha256(packet.bytes,packet.length,input.sha256);
        XrXirLibraryCatalog *catalog=NULL;CHECK(xr_xir_compile_library_catalog_new_v2(library_context,&input,1,&catalog)==XR_XIR_OK);
        xr_xir_compile_checked_packet_free(&packet);
        conflict_text("collision.xr",mode?"import {answer} from \"./bridge\"; export fn result()->i64 {return answer();}":
            "import {answer} from \"./collision\"; export fn result()->i64 {return answer();}");
        if(mode)conflict_text("bridge.xr","import {answer} from \"./collision\"; export fn bridge()->i64{return answer();}");
        XrCompilerSession *consumer=library_session_new(library_context);CHECK(consumer);
        request.session=consumer;request.linkage_kind=XR_XIR_PROGRAM;request.libraries=catalog;
        XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
        fprintf(stderr,"representation mode=%u status=%u %s\n",mode,status,diagnostic.message);
        CHECK(status==XR_XIR_BAD_STRUCTURE&&!result.checked&&!result.snapshot);
        CHECK(!strcmp(diagnostic.message,"module graph build failed"));
        xr_xir_compile_source_result_free(&result);xr_compile_session_free(consumer);xr_xir_compile_library_catalog_free(catalog);
        CHECK(!runtime_live&&!runtime_bytes);
    }
}
static void library_source_case(bool middle_case,const char *packet_path) {
 conflict_text("library.xr","fn secret(value:i64=41)->i64 { return value; }\nexport fn answer(value:i64=secret())->i64 { return value; }\n");
 XrCompilerSession *producer=library_session_new(library_context);CHECK(producer);
 XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
 XrXirSourceRequest library_request={producer,XR_SOURCE_FIXTURES "/library.xr",&authority,library_context,NULL,NULL,XR_XIR_LIBRARY,NULL};
 XrXirSourceResult library={0};XrXirSourceDiagnostic diagnostic={0};
 XrXirStatus status=xr_xir_compile_source_check(&library_request,&library,&diagnostic,NULL);
 if(status!=XR_XIR_OK)fprintf(stderr,"library %u %s\n",status,diagnostic.message);
 CHECK(status==XR_XIR_OK);
 fprintf(stderr,"producer checked\n");
 const XrXirModule *producer_module=xr_xir_compile_artifact_module(library.checked);
 const XrXirSourceModule *producer_identity=&producer_module->declarations->modules[0];
 char canonical[1024];CHECK(producer_identity->name_length<sizeof(canonical));
 memcpy(canonical,producer_identity->name,producer_identity->name_length);canonical[producer_identity->name_length]=0;
 XrXirArtifact *middle=library_initializer_map_cases(canonical);
 if(middle_case){xr_xir_compile_artifact_free(library.checked);library.checked=middle;}
 else xr_xir_compile_artifact_free(middle);

 XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(library.checked,&packet,NULL)==XR_XIR_OK);
 xr_xir_compile_source_result_free(&library);xr_compile_session_free(producer);
 XrXirLibraryInput input={packet.bytes,packet.length,{0}, (XrXirLibraryModuleInput[]){{authority,"library.xr"}},1};
 XrSHA256Context sha;xr_sha256_init(&sha);xr_sha256_update(&sha,packet.bytes,packet.length);xr_sha256_final(&sha,input.sha256);
 void *replay_bytes=malloc(input.length);CHECK(replay_bytes);memcpy(replay_bytes,input.packet,input.length);
 XrXirLibraryInput replay_input=input;replay_input.packet=replay_bytes;
 LibrarySourceFixture replay={&replay_input,1,XR_SOURCE_FIXTURES "/root.xr",authority,XR_XIR_PROGRAM};
 fprintf(stderr,"producer destroyed\n");
 runtime_attempts=0;
 XrXirLibraryCatalog *catalog=NULL;status=xr_xir_compile_library_catalog_new_v2(library_context,&input,1,&catalog);
 if(status!=XR_XIR_OK)fprintf(stderr,"catalog %u\n",status);CHECK(status==XR_XIR_OK);
 library_source_metadata_cases(catalog);
 catalog_thresholds(&input);reader_remaining_cases(packet.bytes,packet.length);
 XrXirLibraryCatalog *bad=NULL;XrXirLibraryModuleInput wrong_binding=*input.modules;XrXirLibraryInput wrong=input;wrong.sha256[0]^=1;
 CHECK(xr_xir_compile_library_catalog_new_v2(library_context,&wrong,1,&bad)==XR_XIR_BAD_STRUCTURE&&!bad);
 wrong=input;wrong_binding=*input.modules;wrong.modules=&wrong_binding;wrong_binding.logical_path="other.xr";CHECK(xr_xir_compile_library_catalog_new_v2(library_context,&wrong,1,&bad)==XR_XIR_BAD_STRUCTURE&&!bad);
 wrong=input;wrong_binding=*input.modules;wrong.modules=&wrong_binding;wrong_binding.authority.kind=XR_MODULE_IDENTITY_PROJECT;wrong_binding.authority.namespace_id="other";
 CHECK(xr_xir_compile_library_catalog_new_v2(library_context,&wrong,1,&bad)==XR_XIR_BAD_STAGE&&!bad);
 size_t identity_offset=0;
 for(size_t i=64;i+10<=packet.length;++i)if(!memcmp(packet.bytes+i,"library.xr",10)){identity_offset=i;break;}
 CHECK(identity_offset);packet.bytes[identity_offset]='x';
 xr_sha256_init(&sha);xr_sha256_update(&sha,packet.bytes,32);xr_sha256_update(&sha,packet.bytes+64,packet.length-64);xr_sha256_final(&sha,packet.bytes+32);
 wrong=input;xr_sha256(packet.bytes,packet.length,wrong.sha256);
 CHECK(xr_xir_compile_library_catalog_new_v2(library_context,&wrong,1,&bad)==XR_XIR_BAD_STRUCTURE&&!bad);
 packet.bytes[identity_offset]='l';xr_sha256_init(&sha);xr_sha256_update(&sha,packet.bytes,32);xr_sha256_update(&sha,packet.bytes+64,packet.length-64);xr_sha256_final(&sha,packet.bytes+32);
 uint8_t saved[8];memcpy(saved,packet.bytes+8,8);
 for(unsigned mode=0;mode<7;++mode){
  memcpy(packet.bytes+8,saved,8);
  if(mode<4){
   if(mode!=3){memset(packet.bytes+8,0,4);packet.bytes[8]=19;}
   if(mode!=2){memset(packet.bytes+12,0,4);packet.bytes[12]=mode==0?48:50;}
  }else{
   if(mode!=6){memset(packet.bytes+8,0,4);packet.bytes[8]=20;}
   if(mode!=5){memset(packet.bytes+12,0,4);packet.bytes[12]=51;}
  }
  xr_sha256_init(&sha);xr_sha256_update(&sha,packet.bytes,32);xr_sha256_update(&sha,packet.bytes+64,packet.length-64);xr_sha256_final(&sha,packet.bytes+32);
  wrong=input;xr_sha256(packet.bytes,packet.length,wrong.sha256);
  CHECK(xr_xir_compile_library_catalog_new_v2(library_context,&wrong,1,&bad)==XR_XIR_BAD_STRUCTURE&&!bad);
  XrXirArtifact *bad_artifact=NULL;
  CHECK(xr_xir_compile_checked_read(library_context,packet.bytes,packet.length,&bad_artifact,NULL)==XR_XIR_BAD_STRUCTURE&&!bad_artifact);
 }
 memcpy(packet.bytes+8,saved,8);
 xr_xir_compile_checked_packet_free(&packet);
 /* A matching resource with a different authority must fail even when source exists. */
 size_t resource_count=0;const XrModuleResourceBinding *resources=xr_xir_compile_library_catalog_resources_v2(catalog,&resource_count);
 CHECK(resources&&resource_count==1);XrXirLibraryModuleInput foreign_binding=*replay_input.modules;foreign_binding.authority.physical_root="E:/different-root";XrXirLibraryInput foreign_input=replay_input;foreign_input.modules=&foreign_binding;
 XrXirLibraryCatalog *foreign_catalog=NULL;CHECK(xr_xir_compile_library_catalog_new_v2(library_context,&foreign_input,1,&foreign_catalog)==XR_XIR_OK);
 XrModuleResolverConfig resolver_config={.catalog=foreign_catalog};
 XrModuleResolver *resolver=library_resolver_new(library_context,&resolver_config);CHECK(resolver);
 XrModuleId identity={0};char *error=NULL;
 CHECK(xr_compile_module_resolver_resolve(resolver,"./library",XR_SOURCE_FIXTURES "/root.xr",&authority,&identity,&error)==XR_MODULE_INVALID);
 CHECK(error&&!strcmp(error,"module resolution failed")&&!identity.canonical&&!identity.logical_path&&!identity.source_path&&!identity.resource);xr_compile_resources_free(error);xr_compile_module_id_cleanup(&identity);error=NULL;
 CHECK(xr_compile_module_resolver_resolve(resolver,"./library.xr",XR_SOURCE_FIXTURES "/root.xr",&authority,&identity,&error)==XR_MODULE_INVALID);
 CHECK(error&&!strcmp(error,"module resolution failed")&&!identity.canonical&&!identity.logical_path&&!identity.source_path&&!identity.resource);xr_compile_resources_free(error);xr_compile_module_id_cleanup(&identity);xr_compile_module_resolver_free(resolver);xr_xir_compile_library_catalog_free(foreign_catalog);
 library_unmatched_source_imports(&authority,catalog);
 CHECK(remove(XR_SOURCE_FIXTURES "/library.xr")==0);
 resolver_config.catalog=catalog;resolver=library_resolver_new(library_context,&resolver_config);CHECK(resolver);
 error=NULL;XrCompilerSession *graph_session=library_session_new(library_context);XrModuleGraph *graph=NULL;
 CHECK(xr_compile_module_graph_new(library_context->resources,graph_session,resolver,&graph)==XR_MODULE_OK);
 CHECK(xr_compile_module_graph_build(graph,XR_SOURCE_FIXTURES "/root.xr",&authority,&error)==XR_MODULE_OK&&!error);
 bool checked_resource=false;for(int i=0;i<graph->spec_count;++i)if(graph->specs[i].representation==XR_MODULE_CHECKED_LIBRARY){
    CHECK(graph->specs[i].resource==resources&&graph->specs[i].resource->checked==resources[0].checked);
    CHECK(graph->specs[i].authority.kind==authority.kind&&!strcmp(graph->specs[i].authority.physical_root,authority.physical_root));checked_resource=true;}
 CHECK(checked_resource);xr_compile_module_graph_free(graph);xr_compile_module_resolver_free(resolver);xr_compile_session_free(graph_session);
 size_t graph_live=source_live,graph_bytes=source_bytes;
 graph_session=library_session_new(library_context);XrXirSourceRequest absent={graph_session,XR_SOURCE_FIXTURES "/root.xr",&authority,library_context,NULL,NULL,XR_XIR_PROGRAM,NULL};
 XrXirSourceResult missing={0};XrXirStatus missing_status=xr_xir_compile_source_check(&absent,&missing,&diagnostic,NULL);fprintf(stderr,"Deleted-source no catalog status%u reason%s\n",missing_status,diagnostic.message);CHECK(missing_status==XR_XIR_UNRESOLVED&&!missing.checked&&!missing.snapshot&&!strcmp(diagnostic.message,"module not found"));
 xr_xir_compile_source_result_free(&missing);xr_compile_session_free(graph_session);CHECK(source_live==graph_live&&source_bytes==graph_bytes);
 graph_session=library_session_new(library_context);XrModuleResolverConfig absent_config={0};resolver=library_resolver_new(library_context,&absent_config);graph=NULL;error=NULL;
 CHECK(xr_compile_module_graph_new(library_context->resources,graph_session,resolver,&graph)==XR_MODULE_OK);
 CHECK(xr_compile_module_graph_build(graph,XR_SOURCE_FIXTURES "/root.xr",&authority,&error)==XR_MODULE_NOT_FOUND&&graph->resolution_status==XR_MODULE_NOT_FOUND);
 CHECK(error&&!strcmp(error,"module not found"));xr_compile_resources_free(error);xr_compile_module_graph_free(graph);xr_compile_module_resolver_free(resolver);xr_compile_session_free(graph_session);
 CHECK(source_live==graph_live&&source_bytes==graph_bytes);
 puts("Retired source-only rejection replaced: Checked catalog graph admitted; same deleted-source graph without catalog rejected/unpublished");
 library_compile_operation_cases(middle_case?"Library middle initializer whole Source":"Library default whole Source",library_source_operation,&replay);
 memset(replay_bytes,0,replay_input.length);free(replay_bytes);

 XrCompilerSession *session=library_session_new(library_context);CHECK(session);
 XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/root.xr",&authority,library_context,NULL,NULL,XR_XIR_PROGRAM,catalog};
 fprintf(stderr,"catalog ready consumer begin\n");
 source_attempts=0;
 XrXirSourceResult private_result={0};
 XrXirSourceRequest private_request=request;private_request.entry_path=XR_SOURCE_FIXTURES "/private.xr";
 CHECK(xr_xir_compile_source_check(&private_request,&private_result,&diagnostic,NULL)==XR_XIR_BAD_STRUCTURE);
 CHECK(!private_result.checked&&strstr(diagnostic.message,"exported declaration"));xr_xir_compile_source_result_free(&private_result);
 source_attempts=0;runtime_attempts=0;
 XrXirSourceResult result={0};status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
 if(status!=XR_XIR_OK)fprintf(stderr,"consumer %u %s\n",status,diagnostic.message);CHECK(status==XR_XIR_OK);
 fprintf(stderr,"consumer checked\n");
 XrXirArtifact *owned=result.checked;result.checked=NULL;xr_xir_compile_source_result_free(&result);
 xr_compile_session_free(session);xr_xir_compile_library_catalog_free(catalog);

 const XrXirModule *module=xr_xir_compile_artifact_module(owned);uint32_t entry=UINT32_MAX;
 for(uint32_t f=0;f<module->function_count;++f)if(module->functions[f].name_length==6&&!memcmp(module->functions[f].name,"result",6))entry=f;
 CHECK(entry!=UINT32_MAX&&module->declarations->module_count==2);
 XrXirCheckedPacket program_packet={0};CHECK(xr_xir_compile_checked_write(owned,&program_packet,NULL)==XR_XIR_OK);
 if(packet_path){FILE *packet_file=fopen(packet_path,"wb");CHECK(packet_file);
 CHECK(program_packet.length<=262144&&fwrite(program_packet.bytes,1,program_packet.length,packet_file)==program_packet.length);CHECK(!fclose(packet_file));}
 xr_xir_compile_checked_packet_free(&program_packet);
 xr_test_library_source_run(owned);
 CHECK(!runtime_live&&!runtime_bytes);
}
static void library_unit_boundary_cases(void) {
 const char *cases[]={
  "export fn answer(value:())->i64 { return 41; }",
  "fn empty() {}\nexport fn answer(value:()=empty())->i64 { return 41; }",
  "fn empty() {}\nfn answer(value:())->i64 { return 41; }\nexport fn result()->i64 { return answer(empty()); }"};
 for(unsigned i=0;i<3;++i){
  conflict_text("unit.xr",cases[i]);
  XrCompilerSession *session=library_session_new(library_context);CHECK(session);
  XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
  XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/unit.xr",&authority,library_context,NULL,NULL,i<2?XR_XIR_LIBRARY:XR_XIR_PROGRAM,NULL};
  XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
  XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
  printf("Unit probe=%u status=%u diagnostic=%s\n",i,status,diagnostic.message);
  CHECK(status==XR_XIR_BAD_TYPE && !result.checked);
  CHECK(!strcmp(diagnostic.message,"parameter contract is not implemented in XIR"));
  xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);
  CHECK(!runtime_live&&!runtime_bytes);
 }
}
int main(int argc,char **argv) {
 CHECK(argc==1||argc==2);
 if(argc==2&&!strcmp(argv[1],"--views")){
  LibraryCompileOwner views_owner={0};
  for(unsigned mode=0;mode<5;++mode){
   CHECK(library_compile_owner_new(&views_owner,&library_compile_limits)==XR_XIR_OK);
   library_context=&views_owner.context;
   if(mode<4)library_views_case(mode);else library_views_growth_case();
   library_compile_owner_drop(&views_owner);
  }
  library_context=NULL;library_compile_observer_free();
  puts("Checked Library five real Source graphs and complete failure boundaries PASS");
  return 0;
 }
 conflict_text("root.xr","import {answer} from \"./library\";\nexport fn result()->i64 { return answer(); }\n");
 conflict_text("private.xr","import {secret} from \"./library\";\nexport fn result()->i64 { return secret(); }\n");
 LibraryCompileOwner owner={0};
 CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);library_context=&owner.context;
 library_string_catalog_cases();library_compile_owner_drop(&owner);
 CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);library_context=&owner.context;
 library_independent_goldens();defaults_wire_cases();library_compile_owner_drop(&owner);
 CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);library_context=&owner.context;
 library_unit_boundary_cases();source_representation_conflicts();library_compile_owner_drop(&owner);
 CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);library_context=&owner.context;
 library_source_case(false,argc==2?argv[1]:NULL);library_compile_owner_drop(&owner);
 CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);library_context=&owner.context;
 library_source_case(true,NULL);library_compile_owner_drop(&owner);
 for(unsigned mode=0;mode<4;++mode){CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);library_context=&owner.context;library_views_case(mode);library_compile_owner_drop(&owner);}
 CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);library_context=&owner.context;
 library_views_growth_case();library_compile_owner_drop(&owner);
 for(unsigned mode=0;mode<2;++mode){CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);library_context=&owner.context;library_typed_closed_case(mode);library_compile_owner_drop(&owner);}
 CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);library_context=&owner.context;
 library_construction_case();library_compile_owner_drop(&owner);
 for(unsigned mode=0;mode<2;++mode){CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);library_context=&owner.context;library_generic_parent_case(mode);library_compile_owner_drop(&owner); }
 CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);library_context=&owner.context;
 library_member_witness_case();library_compile_owner_drop(&owner);
 CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);library_context=&owner.context;
 library_carrier_member_case();library_compile_owner_drop(&owner);
 CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);library_context=&owner.context;
 library_enum_owned_case();library_compile_owner_drop(&owner);
 CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);library_context=&owner.context;
 library_class_owned_case();library_compile_owner_drop(&owner);
 library_context=NULL;library_compile_observer_free();
 puts("Checked Library direct Source lifetime, exact identities and fault boundaries PASS");return 0;
}
