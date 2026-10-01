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
static size_t source_attempts,source_fail_at=SIZE_MAX,source_live,source_bytes,source_peak;
typedef struct LibrarySourceAllocation {void *pointer;size_t bytes;} LibrarySourceAllocation;
static LibrarySourceAllocation source_owned[4096];
XR_FUNC void *xr_test_library_source_calloc(size_t count,size_t size){
 CHECK(!size||count<=SIZE_MAX/size);
 if(source_attempts++==source_fail_at)return NULL;void *p=xr_calloc(count,size);
 if(p){CHECK(source_live<4096);CHECK(count*size<=SIZE_MAX-source_bytes);source_owned[source_live++]=(LibrarySourceAllocation){p,count*size};source_bytes+=count*size;if(source_bytes>source_peak)source_peak=source_bytes;}return p;
}
XR_FUNC void xr_test_library_source_free(void *p){
 if(p)for(size_t i=0;i<source_live;++i)if(source_owned[i].pointer==p){source_bytes-=source_owned[i].bytes;source_owned[i]=source_owned[--source_live];break;}
 xr_free(p);
}
XR_FUNC void *xr_test_library_source_malloc(size_t size){return xr_test_library_source_calloc(1,size);}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_calloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_calloc
#undef xr_free
#define xr_malloc(s) xr_test_library_source_malloc(s)
#define xr_calloc(c,s) xr_test_library_source_calloc(c,s)
#define xr_free(p) xr_test_library_source_free(p)
#include "xir/xxir_source_query.c"
#include "xir/xxir_type_inference.c"
#include "xir/xxir_source.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_calloc")
#pragma pop_macro("xr_malloc")
#include "xir_library_source_runtime.h"
#include "xir/xxir_library_catalog.h"
#include "base/xsha256.h"

static void catalog_thresholds(const XrXirLibraryInput *input) {
 for(unsigned dimension=0;dimension<2;++dimension){
  XrXirBudget defaults=xr_xir_default_budget();uint64_t low=0,high=dimension?defaults.metadata_bytes:defaults.work;
  unsigned rounds=0;size_t live=runtime_live,bytes=runtime_bytes;
  while(low<high){CHECK(++rounds<=64);uint64_t middle=low+(high-low)/2;XrXirBudget budget=defaults;
   if(dimension)budget.metadata_bytes=middle;else budget.work=middle;
   XrXirLibraryCatalog *catalog=NULL;XrXirStatus status=xr_xir_library_catalog_new(input,1,&budget,&catalog);
   CHECK(status==XR_XIR_OK||status==XR_XIR_BUDGET);if(status==XR_XIR_OK){xr_xir_library_catalog_free(catalog);high=middle;}else{CHECK(!catalog);low=middle+1;}
   CHECK(runtime_live==live&&runtime_bytes==bytes);
  }
  CHECK(low);for(unsigned below=0;below<2;++below){XrXirBudget budget=defaults;if(dimension)budget.metadata_bytes=low-below;else budget.work=low-below;
   XrXirLibraryCatalog *catalog=NULL;CHECK(xr_xir_library_catalog_new(input,1,&budget,&catalog)==(below?XR_XIR_BUDGET:XR_XIR_OK));xr_xir_library_catalog_free(catalog);CHECK(runtime_live==live&&runtime_bytes==bytes);
  }
  printf("Catalog exact %s threshold=%llu; minus1 BUDGET physicalbaseline\n",dimension?"metadata":"work",(unsigned long long)low);
 }
 XrXirBudget budget=xr_xir_default_budget(),before=budget;XrXirArtifact *artifact=NULL;
 CHECK(xr_xir_checked_read_remaining(input->packet,input->length,&budget,&artifact,NULL)==XR_XIR_OK);
 CHECK(budget.work<before.work&&budget.metadata_bytes<before.metadata_bytes&&budget.scratch_bytes==before.scratch_bytes);xr_xir_artifact_free(artifact);
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
static void source_representation_conflicts(void) {
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    for(unsigned mode=0;mode<2;++mode){
        conflict_text("collision.xr","export fn answer()->i64 { return 41; }");
        XrCompilerSession *producer=xr_compiler_session_new(NULL);CHECK(producer);
        XrXirSourceRequest request={producer,XR_SOURCE_FIXTURES "/collision.xr",&authority,NULL,NULL,NULL,XR_XIR_LIBRARY,NULL};
        XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
        CHECK(xr_xir_source_check(&request,&result,&diagnostic)==XR_XIR_OK);
        XrXirCheckedPacket packet={0};CHECK(xr_xir_checked_write(result.checked,NULL,&packet,NULL)==XR_XIR_OK);
        xr_xir_source_result_free(&result);xr_compiler_session_delete(producer);
        XrXirLibraryInput input={authority,"collision.xr",packet.bytes,packet.length,{0}};
        xr_sha256(packet.bytes,packet.length,input.sha256);
        XrXirLibraryCatalog *catalog=NULL;CHECK(xr_xir_library_catalog_new(&input,1,NULL,&catalog)==XR_XIR_OK);
        xr_xir_checked_packet_free(&packet);
        conflict_text("collision.xr",mode?"import {answer} from \"./bridge\"; export fn result()->i64 {return answer();}":
            "import {answer} from \"./collision\"; export fn result()->i64 {return answer();}");
        if(mode)conflict_text("bridge.xr","import {answer} from \"./collision\"; export fn bridge()->i64{return answer();}");
        XrCompilerSession *consumer=xr_compiler_session_new(NULL);CHECK(consumer);
        request.session=consumer;request.linkage_kind=XR_XIR_PROGRAM;request.libraries=catalog;
        XrXirStatus status=xr_xir_source_check(&request,&result,&diagnostic);
        fprintf(stderr,"representation mode=%u status=%u %s\n",mode,status,diagnostic.message);
        CHECK(status==XR_XIR_BAD_STRUCTURE&&!result.checked&&!result.snapshot);
        CHECK(strstr(diagnostic.message,"representation conflicts"));
        xr_xir_source_result_free(&result);xr_compiler_session_delete(consumer);xr_xir_library_catalog_free(catalog);
        CHECK(!source_live&&!source_bytes&&!runtime_live&&!runtime_bytes);
    }
}
static void library_source_case(bool middle_case,const char *packet_path) {
 conflict_text("library.xr","fn secret(value:i64=41)->i64 { return value; }\nexport fn answer(value:i64=secret())->i64 { return value; }\n");
 XrCompilerSession *producer=xr_compiler_session_new(NULL);CHECK(producer);
 XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
 XrXirSourceRequest library_request={producer,XR_SOURCE_FIXTURES "/library.xr",&authority,NULL,NULL,NULL,XR_XIR_LIBRARY,NULL};
 XrXirSourceResult library={0};XrXirSourceDiagnostic diagnostic={0};
 XrXirStatus status=xr_xir_source_check(&library_request,&library,&diagnostic);
 if(status!=XR_XIR_OK)fprintf(stderr,"library %u %s\n",status,diagnostic.message);
 CHECK(status==XR_XIR_OK);
 fprintf(stderr,"producer checked\n");
 const XrXirModule *producer_module=xr_xir_artifact_module(library.checked);
 const XrXirSourceModule *producer_identity=&producer_module->declarations->modules[0];
 char canonical[1024];CHECK(producer_identity->name_length<sizeof(canonical));
 memcpy(canonical,producer_identity->name,producer_identity->name_length);canonical[producer_identity->name_length]=0;
 XrXirArtifact *middle=library_initializer_map_cases(canonical);
 if(middle_case){xr_xir_artifact_free(library.checked);library.checked=middle;}
 else xr_xir_artifact_free(middle);

 XrXirCheckedPacket packet={0};CHECK(xr_xir_checked_write(library.checked,NULL,&packet,NULL)==XR_XIR_OK);
 xr_xir_source_result_free(&library);xr_compiler_session_delete(producer);
 XrXirLibraryInput input={authority,"library.xr",packet.bytes,packet.length,{0}};
 XrSHA256Context sha;xr_sha256_init(&sha);xr_sha256_update(&sha,packet.bytes,packet.length);xr_sha256_final(&sha,input.sha256);
 fprintf(stderr,"producer destroyed\n");
 runtime_attempts=0;
 XrXirLibraryCatalog *catalog=NULL;status=xr_xir_library_catalog_new(&input,1,NULL,&catalog);
 if(status!=XR_XIR_OK)fprintf(stderr,"catalog %u\n",status);CHECK(status==XR_XIR_OK);
 size_t catalog_sites=runtime_attempts,catalog_live=runtime_live,catalog_bytes=runtime_bytes;
 library_source_metadata_cases(catalog);
 for(size_t fault=0;fault<catalog_sites;++fault){
  XrXirLibraryCatalog *failed=NULL;runtime_attempts=0;runtime_fail_at=fault;
  XrXirStatus failed_status=xr_xir_library_catalog_new(&input,1,NULL,&failed);
  runtime_fail_at=SIZE_MAX;
  if(failed_status!=XR_XIR_OUT_OF_MEMORY)fprintf(stderr,"catalog fault %zu status %u\n",fault,failed_status);
  CHECK(failed_status==XR_XIR_OUT_OF_MEMORY&&!failed);CHECK(runtime_live==catalog_live&&runtime_bytes==catalog_bytes);
 }
 catalog_thresholds(&input);reader_remaining_cases(packet.bytes,packet.length);
 printf("Catalog/Checked hooked allocation OOM=%zu; external identity helper excluded\n",catalog_sites);
 XrXirLibraryCatalog *bad=NULL;XrXirLibraryInput wrong=input;wrong.sha256[0]^=1;
 CHECK(xr_xir_library_catalog_new(&wrong,1,NULL,&bad)==XR_XIR_BAD_STRUCTURE&&!bad);
 wrong=input;wrong.logical_path="other.xr";CHECK(xr_xir_library_catalog_new(&wrong,1,NULL,&bad)==XR_XIR_BAD_STRUCTURE&&!bad);
 wrong=input;wrong.authority.kind=XR_MODULE_IDENTITY_PROJECT;wrong.authority.namespace_id="other";
 CHECK(xr_xir_library_catalog_new(&wrong,1,NULL,&bad)==XR_XIR_BAD_STAGE&&!bad);
 size_t identity_offset=0;
 for(size_t i=64;i+10<=packet.length;++i)if(!memcmp(packet.bytes+i,"library.xr",10)){identity_offset=i;break;}
 CHECK(identity_offset);packet.bytes[identity_offset]='x';
 xr_sha256_init(&sha);xr_sha256_update(&sha,packet.bytes,32);xr_sha256_update(&sha,packet.bytes+64,packet.length-64);xr_sha256_final(&sha,packet.bytes+32);
 wrong=input;xr_sha256(packet.bytes,packet.length,wrong.sha256);
 CHECK(xr_xir_library_catalog_new(&wrong,1,NULL,&bad)==XR_XIR_BAD_STRUCTURE&&!bad);
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
  CHECK(xr_xir_library_catalog_new(&wrong,1,NULL,&bad)==XR_XIR_BAD_STRUCTURE&&!bad);
  XrXirArtifact *bad_artifact=NULL;
  CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&bad_artifact,NULL)==XR_XIR_BAD_STRUCTURE&&!bad_artifact);
 }
 memcpy(packet.bytes+8,saved,8);
 xr_xir_checked_packet_free(&packet);
 /* A matching resource with a different authority must fail even when source exists. */
 size_t resource_count=0;const XrModuleResourceBinding *resources=xr_xir_library_catalog_resources(catalog,&resource_count);
 CHECK(resources&&resource_count==1);XrModuleResourceBinding binding=resources[0];
 binding.authority.physical_root="E:/different-root";
 XrModuleResolverConfig resolver_config={NULL,NULL,&binding,1};
 XrModuleResolver *resolver=xr_module_resolver_new(&resolver_config);CHECK(resolver);
 XrModuleId identity={0};char *error=NULL;
 CHECK(xr_module_resolver_resolve(resolver,"./library",XR_SOURCE_FIXTURES "/root.xr",&authority,&identity,&error)==-1);
 CHECK(error&&strstr(error,"authority binding"));xr_free(error);xr_module_id_cleanup(&identity);error=NULL;
 CHECK(xr_module_resolver_resolve(resolver,"./library.xr",XR_SOURCE_FIXTURES "/root.xr",&authority,&identity,&error)==-1);
 CHECK(error&&strstr(error,"authority binding"));xr_free(error);xr_module_id_cleanup(&identity);xr_module_resolver_free(resolver);
 library_unmatched_source_imports(&authority,catalog);
 CHECK(remove(XR_SOURCE_FIXTURES "/library.xr")==0);
 resolver_config.resources=xr_xir_library_catalog_resources(catalog,&resolver_config.resource_count);resolver=xr_module_resolver_new(&resolver_config);CHECK(resolver);
 XrCompilerSession *old_session=xr_compiler_session_new(NULL);CHECK(old_session);
 XrModuleGraph *old_graph=xr_module_graph_new(old_session,resolver);CHECK(old_graph);error=NULL;
 CHECK(xr_module_graph_build(old_graph,XR_SOURCE_FIXTURES "/root.xr",&authority,&error)==-1);
 CHECK(error&&strstr(error,"not admitted"));xr_free(error);xr_module_graph_free(old_graph);xr_module_resolver_free(resolver);xr_compiler_session_delete(old_session);

 XrCompilerSession *session=xr_compiler_session_new(NULL);CHECK(session);
 XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/root.xr",&authority,NULL,NULL,NULL,XR_XIR_PROGRAM,catalog};
 fprintf(stderr,"catalog ready consumer begin\n");
 source_attempts=0;
 XrXirSourceResult private_result={0};
 XrXirSourceRequest private_request=request;private_request.entry_path=XR_SOURCE_FIXTURES "/private.xr";
 CHECK(xr_xir_source_check(&private_request,&private_result,&diagnostic)==XR_XIR_BAD_STRUCTURE);
 CHECK(!private_result.checked&&strstr(diagnostic.message,"exported declaration"));xr_xir_source_result_free(&private_result);
 source_attempts=0;runtime_attempts=0;
 XrXirSourceResult result={0};status=xr_xir_source_check(&request,&result,&diagnostic);
 if(status!=XR_XIR_OK)fprintf(stderr,"consumer %u %s\n",status,diagnostic.message);CHECK(status==XR_XIR_OK);
 fprintf(stderr,"consumer checked\n");
 size_t core_sites=runtime_attempts,source_baseline=source_live,source_physical=source_bytes;
 size_t sites=source_attempts, baseline=runtime_live, bytes=runtime_bytes;
 for(size_t fault=0;fault<sites;++fault){
  XrCompilerSession *failed_session=xr_compiler_session_new(NULL);CHECK(failed_session);
  XrXirSourceRequest failed_request=request;failed_request.session=failed_session;
  XrXirSourceResult failed={0};source_attempts=0;source_fail_at=fault;
  XrXirStatus failed_status=xr_xir_source_check(&failed_request,&failed,&diagnostic);
  source_fail_at=SIZE_MAX;
  if(failed_status!=XR_XIR_OUT_OF_MEMORY)fprintf(stderr,"source fault=%zu status=%u %s\n",fault,failed_status,diagnostic.message);
  CHECK(failed_status==XR_XIR_OUT_OF_MEMORY&&!failed.checked&&!failed.snapshot);
  xr_xir_source_result_free(&failed);xr_compiler_session_delete(failed_session);
  CHECK(runtime_live==baseline&&runtime_bytes==bytes&&source_live==source_baseline&&source_bytes==source_physical);
 }
 for(size_t fault=0;fault<core_sites;++fault){
  XrCompilerSession *failed_session=xr_compiler_session_new(NULL);CHECK(failed_session);
  XrXirSourceRequest failed_request=request;failed_request.session=failed_session;
  XrXirSourceResult failed={0};runtime_attempts=0;runtime_fail_at=fault;
  XrXirStatus failed_status=xr_xir_source_check(&failed_request,&failed,&diagnostic);runtime_fail_at=SIZE_MAX;
  if(failed_status!=XR_XIR_OUT_OF_MEMORY)fprintf(stderr,"core source fault=%zu status=%u %s\n",fault,failed_status,diagnostic.message);
  CHECK(failed_status==XR_XIR_OUT_OF_MEMORY&&!failed.checked&&!failed.snapshot);
  xr_xir_source_result_free(&failed);xr_compiler_session_delete(failed_session);
  CHECK(runtime_live==baseline&&runtime_bytes==bytes&&source_live==source_baseline&&source_bytes==source_physical);
 }
 printf("Source combined Checked core hooked OOM=%zu\n",core_sites);
 printf("Source hooks OOM=%zu (graph/AST external allocations excluded)\n",sites);

 XrXirArtifact *owned=result.checked;result.checked=NULL;xr_xir_source_result_free(&result);
 xr_compiler_session_delete(session);xr_xir_library_catalog_free(catalog);
 CHECK(!source_live&&!source_bytes);
 const XrXirModule *module=xr_xir_artifact_module(owned);uint32_t entry=UINT32_MAX;
 for(uint32_t f=0;f<module->function_count;++f)if(module->functions[f].name_length==6&&!memcmp(module->functions[f].name,"result",6))entry=f;
 CHECK(entry!=UINT32_MAX&&module->declarations->module_count==2);
 XrXirCheckedPacket program_packet={0};CHECK(xr_xir_checked_write(owned,NULL,&program_packet,NULL)==XR_XIR_OK);
 if(packet_path){FILE *packet_file=fopen(packet_path,"wb");CHECK(packet_file);
 CHECK(program_packet.length<=262144&&fwrite(program_packet.bytes,1,program_packet.length,packet_file)==program_packet.length);CHECK(!fclose(packet_file));}
 xr_xir_checked_packet_free(&program_packet);
 xr_test_library_source_run(owned);
 CHECK(!source_live&&!source_bytes&&!runtime_live&&!runtime_bytes);
}
static void library_unit_boundary_cases(void) {
 const char *cases[]={
  "export fn answer(value:())->i64 { return 41; }",
  "fn empty() {}\nexport fn answer(value:()=empty())->i64 { return 41; }",
  "fn empty() {}\nfn answer(value:())->i64 { return 41; }\nexport fn result()->i64 { return answer(empty()); }"};
 for(unsigned i=0;i<3;++i){
  conflict_text("unit.xr",cases[i]);
  XrCompilerSession *session=xr_compiler_session_new(NULL);CHECK(session);
  XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
  XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/unit.xr",&authority,NULL,NULL,NULL,i<2?XR_XIR_LIBRARY:XR_XIR_PROGRAM,NULL};
  XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
  XrXirStatus status=xr_xir_source_check(&request,&result,&diagnostic);
  printf("Unit probe=%u status=%u diagnostic=%s\n",i,status,diagnostic.message);
  CHECK(status==XR_XIR_BAD_TYPE && !result.checked);
  CHECK(!strcmp(diagnostic.message,"parameter contract is not implemented in XIR"));
  xr_xir_source_result_free(&result);xr_compiler_session_delete(session);
  CHECK(!source_live&&!source_bytes&&!runtime_live&&!runtime_bytes);
 }
}
int main(int argc,char **argv) {
 CHECK(argc==1||argc==2);
 conflict_text("root.xr","import {answer} from \"./library\";\nexport fn result()->i64 { return answer(); }\n");
 conflict_text("private.xr","import {secret} from \"./library\";\nexport fn result()->i64 { return secret(); }\n");
 library_string_catalog_cases();library_independent_goldens();defaults_wire_cases();library_unit_boundary_cases();source_representation_conflicts();
 library_source_case(false,argc==2?argv[1]:NULL);library_source_case(true,NULL);
 puts("Checked Library direct Source lifetime, exact identities and fault boundaries PASS");return 0;
}
