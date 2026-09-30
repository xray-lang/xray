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
#include "base/xmalloc.h"
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
static size_t source_attempts,source_fail_at=SIZE_MAX,source_live;
static void *ready_alloc(size_t count,size_t size){if(source_attempts++==source_fail_at)return NULL;void *p=xr_calloc(count,size);if(p)++source_live;return p;}
static void ready_free(void *p){if(p){CHECK(source_live);--source_live;}xr_free(p);}
#pragma push_macro("xr_calloc")
#pragma push_macro("xr_free")
#undef xr_calloc
#undef xr_free
#define xr_calloc(c,s) ready_alloc(c,s)
#define xr_free(p) ready_free(p)
#include "xir/xxir_source_query.c"
#include "xir/xxir_type_inference.c"
#include "xir/xxir_source.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_calloc")
#include "xir_runtime_allocations.h"
#include "xir_dependency_execution.h"
int main(int argc,char **argv){
 CHECK(argc==1||argc==2);XrCompilerSession *session=xr_compiler_session_new(NULL);CHECK(session);
 XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
 XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/root.xr",&authority,NULL,NULL,NULL};
 SourceContext context={0};context.budget=xr_xir_default_budget();SourceConversionRecipe conversion={0};
 CHECK(source_conversion_plan(&context,NULL,XR_XIR_UNIT,(SourceExpectedType){true,XR_XIR_UNIT},&conversion)&&!conversion.needed);
 CHECK(source_conversion_plan(&context,NULL,XR_XIR_I8,(SourceExpectedType){false,XR_XIR_UNIT},&conversion)&&conversion.target==XR_XIR_I8);
 CHECK(!source_conversion_plan(&context,NULL,XR_XIR_I8,(SourceExpectedType){true,XR_XIR_UNIT},&conversion));
 XrXirArtifact *source_checked=NULL;
 size_t sites=0;
 for(size_t pass=0;pass<=sites;++pass){
  source_attempts=0;source_fail_at=pass?pass-1:SIZE_MAX;XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
  XrXirStatus status=xr_xir_source_check(&request,&result,&diagnostic);
  if(!pass){if(status!=XR_XIR_OK)fprintf(stderr,"%u %d %s\n",status,diagnostic.line,diagnostic.message);CHECK(status==XR_XIR_OK&&result.checked&&result.snapshot);sites=source_attempts;
   XrXirCheckedPacket packet={0};CHECK(xr_xir_checked_write(result.checked,NULL,&packet,NULL)==XR_XIR_OK&&packet.length<=262144);
   if(argc==2){FILE *file=fopen(argv[1],"wb");CHECK(file&&fwrite(packet.bytes,1,packet.length,file)==packet.length&&!fclose(file));}
   xr_xir_checked_packet_free(&packet);
   source_checked=result.checked;result.checked=NULL;
  }else{CHECK(status==XR_XIR_OUT_OF_MEMORY&&!result.checked);if(result.snapshot)CHECK(!xr_xir_source_snapshot_view(result.snapshot)->complete);}
  xr_xir_source_result_free(&result);CHECK(!source_live);
 }
 source_fail_at=SIZE_MAX;
 const char *paths[]={XR_SOURCE_FIXTURES "/negative_narrow.xr",XR_SOURCE_FIXTURES "/negative_array.xr",XR_SOURCE_FIXTURES "/negative_proof.xr"};
 const char *messages[]={"expression cannot satisfy its declared type","expression cannot satisfy its declared type","type argument does not prove the declared constraint"};
 for(unsigned n=0;n<3;++n){request.entry_path=paths[n];XrXirSourceResult rejected={0};XrXirSourceDiagnostic why={0};XrXirStatus status=xr_xir_source_check(&request,&rejected,&why);
 if(status!=XR_XIR_BAD_TYPE||strcmp(why.message,messages[n]))fprintf(stderr,"negative %u %u %s\n",n,status,why.message);
 CHECK(status==XR_XIR_BAD_TYPE&&!rejected.checked&&!strcmp(why.message,messages[n]));if(rejected.snapshot)CHECK(!xr_xir_source_snapshot_view(rejected.snapshot)->complete);xr_xir_source_result_free(&rejected);CHECK(!source_live);}
 xr_compiler_session_delete(session);printf("ready source %zu OOM sites; producer destroyed\n",sites);
 CHECK(source_checked && !source_live);
 XrXirArtifact *special=NULL,*lowered=NULL;
 CHECK(xr_xir_specialize(source_checked,NULL,&special,NULL)==XR_XIR_OK);
 xr_xir_artifact_free(source_checked);source_checked=NULL;
 CHECK(xr_xir_artifact_verify(special,NULL,NULL)==XR_XIR_OK);
 XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
 CHECK(xr_xir_lower(special,&target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_artifact_free(special);
 const XrXirModule *module=xr_xir_artifact_module(lowered);
 CHECK(module->functions[3].name_length==6&&!memcmp(module->functions[3].name,"answer",6));
 CHECK(module->functions[4].name_length==8&&!memcmp(module->functions[4].name,"retained",8));
 CHECK(module->functions[5].name_length==6&&!memcmp(module->functions[5].name,"values",6));
 XrXirProgram *program=NULL;
 CHECK(xr_xir_vm_program_take(&lowered,(XrXirProgramBudget){33554432,64000000},&program)==XR_XIR_OK);
 XrXirValue held[2][2]={{{0}}};ready_pair(program,held);ready_runtime_faults(program);
 xr_xir_program_drop(program);ready_retained(held);
 CHECK(!source_live&&!runtime_live&&!runtime_bytes);
 puts("direct owned source Checked VM readiness41 trace12 retained strings/Array PASS");return 0;
}
