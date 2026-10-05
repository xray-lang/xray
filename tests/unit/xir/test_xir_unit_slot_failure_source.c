/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_unit_slot_failure_source.c - Owned source Checked execution after producer destruction
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
XR_FUNC void xr_test_unit_slot_failure_source_run(XrXirArtifact **checked);
XR_FUNC size_t xr_test_unit_slot_failure_runtime_live(void);
XR_FUNC size_t xr_test_unit_slot_failure_runtime_bytes(void);
int main(int argc,char **argv){
 CHECK(argc==1||argc==2);
 XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
 size_t sites=0,retained_live=0,retained_bytes=0;XrXirArtifact *owned=NULL;UnitCompileOwner retained_owner={0};
 for(size_t pass=0;pass<=sites;++pass){
  UnitCompileOwner owner;unit_compile_owner_new(&owner);
  effects_compile_attempts=0;effects_compile_injected=false;effects_compile_fail_at=pass?pass-1:SIZE_MAX;
  XrCompilerSession *session=NULL;XrCompilerSessionStatus session_status=xr_compile_session_new(owner.context.resources,&session);
  XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/root.xr",&authority,&owner.context,NULL,NULL,XR_XIR_PROGRAM,NULL};
  XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
  XrXirStatus status=session_status==XR_COMPILER_SESSION_OK?xr_xir_compile_source_check(&request,&result,&diagnostic,NULL):session_status==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET;
  size_t actual=effects_compile_attempts;effects_compile_fail_at=SIZE_MAX;
  if(!pass){
   if(status!=XR_XIR_OK)fprintf(stderr,"%u %d %s\n",status,diagnostic.line,diagnostic.message);
   CHECK(status==XR_XIR_OK&&result.checked&&result.snapshot);sites=actual;
   XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(result.checked,&packet,NULL)==XR_XIR_OK&&packet.length<=262144);
   if(argc==2){FILE *file=fopen(argv[1],"wb");CHECK(file&&fwrite(packet.bytes,1,packet.length,file)==packet.length&&!fclose(file));}
   xr_xir_compile_checked_packet_free(&packet);owned=result.checked;result.checked=NULL;
  }else CHECK(effects_compile_injected&&status==XR_XIR_OUT_OF_MEMORY&&!result.checked&&!result.snapshot);
  xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);
  if(!pass){
   retained_owner=owner;retained_live=effects_compile_live;retained_bytes=effects_compile_bytes;
   CHECK(retained_live>1&&retained_bytes>owner.baseline.live_bytes);
  }else unit_compile_owner_free(&owner);
  CHECK(effects_compile_live==retained_live&&effects_compile_bytes==retained_bytes);
  CHECK(!xr_test_unit_slot_failure_runtime_live()&&!xr_test_unit_slot_failure_runtime_bytes());
 }
 xr_test_unit_slot_failure_source_run(&owned);CHECK(!owned);unit_compile_owner_report(&retained_owner,"unit_slot_failure Source");unit_compile_owner_free(&retained_owner);
 CHECK(!effects_compile_live&&!effects_compile_bytes);
 printf("unit_slot_failure Source whole-resource %zu OOM sites; producer destroyed, compiler physical zero\n",sites);return 0;
}
