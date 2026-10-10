/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_unit_slots_source.c - Owned source Checked execution after producer destruction
 *
 * KEY CONCEPT:
 *   The first owned Checked result survives the source snapshot/session and executes directly.
 */
#include "base/xmalloc.h"
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#include "xir_unit_compile_owner.h"
XR_FUNC void xr_test_unit_slots_source_run(XrXirArtifact **checked);
XR_FUNC size_t xr_test_unit_slots_runtime_live(void);
XR_FUNC size_t xr_test_unit_slots_runtime_bytes(void);
static void unit_query_facts(const XrXirSourceResult *result){
 const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(result->snapshot);CHECK(view&&view->complete);
 const char *names[]={"empty","fixed","mutable","inferred","chosen","matched"};
 const bool mutable[]={true,false,true,true,false,false};
 for(unsigned n=0;n<sizeof(names)/sizeof(names[0]);++n){const XrXirSourceDeclaration *found=NULL;
  for(uint32_t d=0;d<view->declaration_count;++d)if(!strcmp(view->declarations[d].name,names[n])){CHECK(!found);found=&view->declarations[d];}
  CHECK(found&&found->id&&found->parent&&found->kind==XR_XIR_SOURCE_BINDING);
  CHECK(found->type.known&&found->type.type==XR_XIR_UNIT&&found->type.generic_owner==0&&found->mutable==mutable[n]);
  unsigned reads=0,writes=0;
  for(uint32_t q=0;q<view->reference_count;++q)if(view->references[q].declaration==found->id){
   if(view->references[q].access==XR_XIR_SOURCE_READ)++reads;
   if(view->references[q].access==XR_XIR_SOURCE_WRITE)++writes;
  }
  CHECK(reads);if(mutable[n])CHECK(writes);
 }
 CHECK(xr_xir_compile_artifact_verify(result->checked,NULL)==XR_XIR_OK);
 /* Canonical Checked verifier rejects Unit operands and private sentinel IDs.
  * All hidden physical parameters therefore remain ordinary admitted values. */
 const XrXirModule *m=xr_xir_compile_artifact_module(result->checked);
 for(uint32_t f=0;f<m->function_count;++f)
  for(uint32_t p=0;p<m->functions[f].parameter_count;++p)CHECK(m->functions[f].parameters[p]!=XR_XIR_UNIT);
}
#include "xir_unit_slot_contract_cases.h"
#include "xir_generic_storage_cases.h"
int main(int argc,char **argv){
 const bool write_only=argc==3&&!strcmp(argv[1],"--write-checked");
 CHECK(argc==1||argc==2||write_only);
 const char *output_path=write_only?argv[2]:argc==2?argv[1]:NULL;
 if(!write_only)generic_storage_cases();
 XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
 size_t sites=0,retained_live=0,retained_bytes=0;XrXirArtifact *owned=NULL;UnitCompileOwner retained_owner={0};
 for(size_t pass=0;pass<=(write_only?0:sites);++pass){
  UnitCompileOwner owner;unit_compile_owner_new(&owner);
  effects_compile_attempts=0;effects_compile_injected=false;effects_compile_fail_at=pass?pass-1:SIZE_MAX;
  XrCompilerSession *session=NULL;XrCompilerSessionStatus session_status=xr_compile_session_new(owner.context.resources,&session);
  XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/root.xr",&authority,&owner.context,NULL,NULL,XR_XIR_PROGRAM,NULL};
  XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
  XrXirStatus status=session_status==XR_COMPILER_SESSION_OK?xr_xir_compile_source_check(&request,&result,&diagnostic,NULL):session_status==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET;
  size_t actual=effects_compile_attempts;effects_compile_fail_at=SIZE_MAX;
  if(!pass){
   if(status!=XR_XIR_OK)fprintf(stderr,"%u %d %s\n",status,diagnostic.line,diagnostic.message);
   CHECK(status==XR_XIR_OK&&result.checked&&result.snapshot);sites=actual;if(!write_only){unit_query_facts(&result);unit_slot_contracts(result.checked);}
   XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(result.checked,&packet,NULL)==XR_XIR_OK&&packet.length<=262144);
   if(output_path){FILE *file=fopen(output_path,"wb");CHECK(file&&fwrite(packet.bytes,1,packet.length,file)==packet.length&&!fclose(file));}
   xr_xir_compile_checked_packet_free(&packet);owned=result.checked;result.checked=NULL;
  }else CHECK(effects_compile_injected&&status==XR_XIR_OUT_OF_MEMORY&&!result.checked&&!result.snapshot);
  xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);
  if(!pass){
   retained_owner=owner;retained_live=effects_compile_live;retained_bytes=effects_compile_bytes;
   CHECK(retained_live>1&&retained_bytes>owner.baseline.live_bytes);
  }else unit_compile_owner_free(&owner);
  CHECK(effects_compile_live==retained_live&&effects_compile_bytes==retained_bytes);
  CHECK(!xr_test_unit_slots_runtime_live()&&!xr_test_unit_slots_runtime_bytes());
 }
 if(write_only){
  xr_xir_compile_artifact_free(owned);owned=NULL;
  unit_compile_owner_report(&retained_owner,"unit_slots normal Checked generation");unit_compile_owner_free(&retained_owner);
  CHECK(!effects_compile_live&&!effects_compile_bytes);
  CHECK(!xr_test_unit_slots_runtime_live()&&!xr_test_unit_slots_runtime_bytes());
  puts("unit_slots normal Checked generation; compiler physical zero");return 0;
 }
 xr_test_unit_slots_source_run(&owned);CHECK(!owned);unit_compile_owner_report(&retained_owner,"unit_slots Source");unit_compile_owner_free(&retained_owner);
 CHECK(!effects_compile_live&&!effects_compile_bytes);
 printf("unit_slots Source whole-resource %zu OOM sites; producer destroyed, compiler physical zero\n",sites);return 0;
}
