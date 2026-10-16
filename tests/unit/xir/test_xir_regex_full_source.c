/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_regex_full_source.c - The complete existing ordinary regex source
 */
#include "base/xmalloc.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#ifdef XR_BUILD_ASAN
#define xr_compile_resources_work full_regex_resource_work
#endif
#include "xir_source_compile_owner.h"
#ifdef XR_BUILD_ASAN
#undef xr_compile_resources_work
#include <sanitizer/common_interface_defs.h>
XR_FUNC XrCompileResourceStatus xr_compile_resources_work(XrCompileResources *resources,uint64_t units) {
 XrCompileResourceStatus status=full_regex_resource_work(resources,units);
 if(status==XR_COMPILE_RESOURCE_BUDGET)__sanitizer_print_stack_trace();return status;
}
#endif
#include "xir_runtime_allocations.h"
#include "xir/xxir_source.h"
int main(void) {
 FILE *file=fopen(XR_REGEX_FULL_SOURCE,"rb");CHECK(file && !fseek(file,0,SEEK_END));long length=ftell(file);CHECK(length>0 && length<1048576 && !fseek(file,0,SEEK_SET));
 char *bytes=xr_malloc((size_t)length);CHECK(bytes && fread(bytes,1,(size_t)length,file)==(size_t)length && !fclose(file));
 uint8_t digest[32];xr_sha256((const uint8_t *)bytes,(size_t)length,digest);fprintf(stderr,"Actual full Regex source bytes=%ld sha256=",length);for(unsigned i=0;i<32;++i)fprintf(stderr,"%02x",(unsigned)digest[i]);fputc('\n',stderr);
 const XrCompileResourceLimits caps={67108864,8388608,128000000};XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
 XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILER_SESSION_OK);
 XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"actual-full-ordinary-regex-source",NULL};
 XrXirSourceRequest request={.session=session,.authority=&authority,.context=&context,.linkage_kind=XR_XIR_LIBRARY};
 XrXirSourceText text={NULL,bytes,(size_t)length};XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
 XrXirStatus status=xr_xir_compile_source_check_text(&request,&text,&source,&diagnostic,NULL);
 fprintf(stderr,"Actual full Regex Source status%u at%d:%d: %s\n",(unsigned)status,diagnostic.line,diagnostic.column,diagnostic.message);
 if(status!=XR_XIR_OK)CHECK(!source.checked && !source.snapshot);
 xr_compile_session_free(session);memset(bytes,0xa5,(size_t)length);xr_free(bytes);
 if(status==XR_XIR_OK){XrXirCheckedPacket packet={0};XrXirArtifact *received=NULL;
  CHECK(source.checked && source.snapshot && xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&source);
  CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);CHECK(xr_xir_compile_artifact_verify(received,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
 }
 xr_xir_compile_source_result_free(&source);XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context.resources,&stats)==XR_COMPILE_RESOURCE_OK);
 fprintf(stderr,"Actual full Regex compiler allocated=%llu peak=%llu work=%llu\n",(unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work);
 xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes && !runtime_live && !runtime_bytes);CHECK(status==XR_XIR_OK);
 puts("Complete existing ordinary Regex Source and independent Checked receiving after caller bytes/session death PASS");return 0;
}
