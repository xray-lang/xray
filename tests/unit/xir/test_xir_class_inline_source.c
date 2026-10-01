/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_class_inline_source.c - Owned source Checked execution after producer destruction
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
static size_t source_attempts,source_fail_at=SIZE_MAX,source_live,source_bytes;
typedef struct SourceOwned {void *pointer;size_t bytes;} SourceOwned;
static SourceOwned *source_owned;
static size_t source_capacity;
static void *ready_alloc(size_t count,size_t size){
 CHECK(!size||count<=SIZE_MAX/size);
 if(source_attempts++==source_fail_at)return NULL;
 void *pointer=xr_calloc(count,size);if(!pointer)return NULL;
 if(source_live==source_capacity){
  CHECK(source_capacity<=SIZE_MAX/2/sizeof(*source_owned));
  size_t capacity=source_capacity?source_capacity*2:256;
  SourceOwned *grown=xr_realloc(source_owned,capacity*sizeof(*grown));CHECK(grown);
  source_owned=grown;source_capacity=capacity;
 }
 CHECK(count*size<=SIZE_MAX-source_bytes);
 source_owned[source_live++]=(SourceOwned){pointer,count*size};source_bytes+=count*size;return pointer;
}
static void ready_free(void *pointer){
 if(pointer){size_t index=0;while(index<source_live&&source_owned[index].pointer!=pointer)++index;
  CHECK(index<source_live);source_bytes-=source_owned[index].bytes;
  source_owned[index]=source_owned[--source_live];xr_free(pointer);
  if(!source_live){CHECK(!source_bytes);xr_free(source_owned);source_owned=NULL;source_capacity=0;}
 }
}
#undef xr_calloc
#undef xr_free
#define xr_calloc(c,s) ready_alloc(c,s)
#define xr_free(p) ready_free(p)
#include "xir/xxir_source_query.c"
#include "xir/xxir_type_inference.c"
#include "xir/xxir_source.c"
#undef xr_calloc
#undef xr_free
XR_FUNC void xr_test_class_inline_source_run(XrXirArtifact **checked);
XR_FUNC size_t xr_test_class_inline_runtime_live(void);
XR_FUNC size_t xr_test_class_inline_runtime_bytes(void);
static void class_inline_query_facts(const XrXirSourceResult *result){
 CHECK(result->snapshot&&xr_xir_source_snapshot_view(result->snapshot)->complete);
 CHECK(xr_xir_artifact_verify(result->checked,NULL,NULL)==XR_XIR_OK);
 CHECK(xr_xir_artifact_module(result->checked)->function_count==19);
}
int main(int argc,char **argv){
 CHECK(argc==1||argc==2);XrCompilerSession *session=xr_compiler_session_new(NULL);CHECK(session);
 XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
 XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/root.xr",&authority,NULL,NULL,NULL, XR_XIR_PROGRAM, NULL};
 size_t sites=0,retained_live=0,retained_bytes=0;XrXirArtifact *owned=NULL;
 for(size_t pass=0;pass<=sites;++pass){
  source_attempts=0;source_fail_at=pass?pass-1:SIZE_MAX;XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
  XrXirStatus status=xr_xir_source_check(&request,&result,&diagnostic);
  if(!pass){if(status!=XR_XIR_OK)fprintf(stderr,"%u %d %s\n",status,diagnostic.line,diagnostic.message);CHECK(status==XR_XIR_OK&&result.checked&&result.snapshot);sites=source_attempts;class_inline_query_facts(&result);
   XrXirCheckedPacket packet={0};CHECK(xr_xir_checked_write(result.checked,NULL,&packet,NULL)==XR_XIR_OK&&packet.length<=262144);
   if(argc==2){FILE *file=fopen(argv[1],"wb");CHECK(file&&fwrite(packet.bytes,1,packet.length,file)==packet.length&&!fclose(file));}
   XrXirArtifact *decoded=NULL;CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&decoded,NULL)==XR_XIR_OK);xr_xir_artifact_free(decoded);xr_xir_checked_packet_free(&packet);owned=result.checked;result.checked=NULL;
  }else{CHECK(status==XR_XIR_OUT_OF_MEMORY&&diagnostic.status==XR_XIR_OUT_OF_MEMORY&&!result.checked);if(result.snapshot)CHECK(!xr_xir_source_snapshot_view(result.snapshot)->complete);}
  xr_xir_source_result_free(&result);CHECK(!source_live&&!source_bytes);
  if(!pass){retained_live=xr_test_class_inline_runtime_live();retained_bytes=xr_test_class_inline_runtime_bytes();CHECK(retained_live&&retained_bytes);}
  CHECK(xr_test_class_inline_runtime_live()==retained_live&&xr_test_class_inline_runtime_bytes()==retained_bytes);
 }
 source_fail_at=SIZE_MAX;
 xr_compiler_session_delete(session);CHECK(!source_live&&!source_bytes);xr_test_class_inline_source_run(&owned);CHECK(!owned);printf("inline class source %zu OOM sites; producer destroyed\n",sites);return 0;
}
