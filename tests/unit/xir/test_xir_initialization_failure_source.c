/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_initialization_failure_source.c - Owned source Checked execution after producer destruction
 *
 * KEY CONCEPT:
 *   The first owned Checked result survives the source snapshot/session and executes directly.
 */
/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_dependency_source.c - Dependency-ready source execution qualification
 *
 * KEY CONCEPT:
 *   Owned source Checked and packet consumers execute independently after producer destruction.
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
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#include "xir_effect_execution_owner.h"
/* Compile the actual Source implementation to retain its private conversion tests. */
#include "xir/xxir_source_query.c"
#include "xir/xxir_type_inference.c"
#include "xir/xxir_source.c"
static void init_compiler_faults(void){
 size_t sites=0;uint64_t max_allocated=0,max_work=0,max_peak=0;
 for(size_t pass=0;pass<=sites;++pass){
  const size_t global_blocks=effects_compile_live,global_bytes=effects_compile_bytes;
  XrXirCompileContext compile={0};compile.limits=xr_xir_compile_default_limits();
  const XrCompileResourceLimits caps={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
  CHECK(xr_compile_resources_new(&caps,&compile.resources)==XR_COMPILE_RESOURCE_OK);
  XrCompileResourceStats initial={0};CHECK(xr_compile_resources_stats(compile.resources,&initial)==XR_COMPILE_RESOURCE_OK);
  XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(compile.resources,&session)==XR_COMPILER_SESSION_OK);
  XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
  XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/root.xr",&authority,&compile,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
  effects_compile_attempts=0;effects_compile_injected=false;effects_compile_fail_at=pass?pass-1:SIZE_MAX;
  XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
  XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
  if(!pass){if(status!=XR_XIR_OK)fprintf(stderr,"baseline %u %s\n",status,diagnostic.message);CHECK(status==XR_XIR_OK&&result.checked&&result.snapshot);sites=effects_compile_attempts;CHECK(sites);}
  else {if(status!=XR_XIR_OUT_OF_MEMORY)fprintf(stderr,"compiler ordinal %zu/%zu: %u %s\n",pass,sites,status,diagnostic.message);CHECK(effects_compile_injected&&status==XR_XIR_OUT_OF_MEMORY&&!result.checked&&!result.snapshot);}
  effects_compile_fail_at=SIZE_MAX;xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);
  XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(compile.resources,&stats)==XR_COMPILE_RESOURCE_OK);
  CHECK(stats.live_bytes==initial.live_bytes);
  if(stats.allocated_bytes>max_allocated)max_allocated=stats.allocated_bytes;
  if(stats.work>max_work)max_work=stats.work;if(stats.peak_bytes>max_peak)max_peak=stats.peak_bytes;
  xr_compile_resources_release(compile.resources);CHECK(effects_compile_live==global_blocks&&effects_compile_bytes==global_bytes);
 }
 printf("sticky initialization source %zu real Source-check OOM ordinals; max allocated=%llu peak=%llu work=%llu; every physical baseline restored\n",sites,(unsigned long long)max_allocated,(unsigned long long)max_peak,(unsigned long long)max_work);
}
XR_FUNC void xr_test_initialization_source_run(XrXirArtifact **checked);
XR_FUNC size_t xr_test_initialization_runtime_live(void);
XR_FUNC size_t xr_test_initialization_runtime_bytes(void);
int main(int argc,char **argv){
 CHECK(argc==1||argc==2);const XrXirCompileContext compile=*effects_source_owner(UINT64_C(64)*1024*1024,UINT64_C(128000000));
 XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(compile.resources,&session)==XR_COMPILER_SESSION_OK);
 XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
 XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/root.xr",&authority,&compile,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
 XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
 XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
 if(status!=XR_XIR_OK)fprintf(stderr,"%u %d %s\n",status,diagnostic.line,diagnostic.message);
 CHECK(status==XR_XIR_OK&&result.checked&&result.snapshot);XrXirArtifact *owned=result.checked;result.checked=NULL;
 XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(owned,&packet,NULL)==XR_XIR_OK&&packet.length<=262144);
 if(argc==2){FILE *file=fopen(argv[1],"wb");CHECK(file&&fwrite(packet.bytes,1,packet.length,file)==packet.length&&!fclose(file));}
 xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_source_result_free(&result);
 const size_t retained_live=effects_compile_live,retained_bytes=effects_compile_bytes;
 const size_t runtime_blocks=xr_test_initialization_runtime_live(),runtime_bytes=xr_test_initialization_runtime_bytes();
 CHECK(retained_live&&retained_bytes);init_compiler_faults();
 CHECK(effects_compile_live==retained_live&&effects_compile_bytes==retained_bytes);
 CHECK(xr_test_initialization_runtime_live()==runtime_blocks&&xr_test_initialization_runtime_bytes()==runtime_bytes);
 xr_compile_session_free(session);CHECK(owned);xr_test_initialization_source_run(&owned);CHECK(!owned);
 effects_source_owners_free();puts("sticky initialization Source producer destroyed; actual compiler/runtime physical zero");return 0;
}
