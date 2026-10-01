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
#include "base/xmalloc.h"
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
static size_t source_attempts,source_fail_at=SIZE_MAX,source_live;
static void *ready_alloc(size_t count,size_t size){if(source_attempts++==source_fail_at)return NULL;void *p=xr_calloc(count,size);if(p)++source_live;return p;}
static void ready_free(void *p){if(p){CHECK(source_live);--source_live;}xr_free(p);}
#undef xr_calloc
#undef xr_free
#define xr_calloc(c,s) ready_alloc(c,s)
#define xr_free(p) ready_free(p)
#include "xir/xxir_source_query.c"
#include "xir/xxir_type_inference.c"
#include "xir/xxir_source.c"
#undef xr_calloc
#undef xr_free
XR_FUNC void xr_test_initialization_source_run(XrXirArtifact **checked);
XR_FUNC size_t xr_test_initialization_runtime_live(void);
XR_FUNC size_t xr_test_initialization_runtime_bytes(void);
int main(int argc,char **argv){
 CHECK(argc==1||argc==2);XrCompilerSession *session=xr_compiler_session_new(NULL);CHECK(session);
 XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
 XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/root.xr",&authority,NULL,NULL,NULL, XR_XIR_PROGRAM, NULL};
 size_t sites=0,retained_live=0,retained_bytes=0;XrXirArtifact *owned=NULL;
 for(size_t pass=0;pass<=sites;++pass){
  source_attempts=0;source_fail_at=pass?pass-1:SIZE_MAX;XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
  XrXirStatus status=xr_xir_source_check(&request,&result,&diagnostic);
  if(!pass){if(status!=XR_XIR_OK)fprintf(stderr,"%u %d %s\n",status,diagnostic.line,diagnostic.message);CHECK(status==XR_XIR_OK&&result.checked&&result.snapshot);sites=source_attempts;
   XrXirCheckedPacket packet={0};CHECK(xr_xir_checked_write(result.checked,NULL,&packet,NULL)==XR_XIR_OK&&packet.length<=262144);
   if(argc==2){FILE *file=fopen(argv[1],"wb");CHECK(file&&fwrite(packet.bytes,1,packet.length,file)==packet.length&&!fclose(file));}
   xr_xir_checked_packet_free(&packet);owned=result.checked;result.checked=NULL;
  }else{CHECK(status==XR_XIR_OUT_OF_MEMORY&&!result.checked);if(result.snapshot)CHECK(!xr_xir_source_snapshot_view(result.snapshot)->complete);}
  xr_xir_source_result_free(&result);CHECK(!source_live);
  if(!pass){retained_live=xr_test_initialization_runtime_live();retained_bytes=xr_test_initialization_runtime_bytes();CHECK(retained_live&&retained_bytes);}
  CHECK(xr_test_initialization_runtime_live()==retained_live&&xr_test_initialization_runtime_bytes()==retained_bytes);
 }
 source_fail_at=SIZE_MAX;
 xr_compiler_session_delete(session);CHECK(!source_live);xr_test_initialization_source_run(&owned);CHECK(!owned);printf("sticky initialization source %zu OOM sites; producer destroyed\n",sites);return 0;
}
