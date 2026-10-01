/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_library_execution.c - Independent packet native and mixed execution
 *
 * KEY CONCEPT:
 *   Source-free consumers require independent 41 results and physical release.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#include "base/xsha256.c"
#include "xir_runtime_allocations.h"
#include "xir/xxir_specialize.c"
#include "xir/xxir_effects.c"
#include "xir/xxir_vm.c"
#if CONSUMER_KIND==0
#include "xir/xxir_emit_c.c"
#endif
#if CONSUMER_KIND
extern const XrXirProgramSpec source_library_program;
#endif
static bool pair(XrXirProgram *program){
 for(unsigned i=0;i<2;++i){XrXirInstanceConfig config=xr_xir_instance_defaults();XrXirInstance *instance=NULL;
  XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
  if(status==XR_XIR_CALL_READY)status=xr_xir_instance_start(instance,2,NULL,0);
  if(status==XR_XIR_CALL_READY)status=xr_xir_instance_poll(instance).outcome.status;
  if(status==XR_XIR_CALL_RETURNED){XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
   CHECK(result.type==XR_XIR_I64&&result.payload==41);xr_xir_value_drop(&result);
  }else CHECK(status==XR_XIR_CALL_OOM);
  if(instance)CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
  if(status!=XR_XIR_CALL_RETURNED)return false;
 }return true;
}
int main(int argc,char **argv){CHECK(argc==1||argc==2);(void)argv;XrXirArtifact *checked=NULL,*special=NULL,*lowered=NULL;XrXirProgram *program=NULL;
#if CONSUMER_KIND==0
 FILE *f=fopen(XR_CHECKED_FIXTURE,"rb");CHECK(f&&!fseek(f,0,SEEK_END));long n=ftell(f);CHECK(n>=64&&n<=262144&&!fseek(f,0,SEEK_SET));
 void *bytes=malloc((size_t)n);CHECK(bytes&&fread(bytes,1,(size_t)n,f)==(size_t)n&&!fclose(f));
 CHECK(xr_xir_checked_read(bytes,(size_t)n,NULL,&checked,NULL)==XR_XIR_OK);free(bytes);
 CHECK(xr_xir_specialize(checked,NULL,&special,NULL)==XR_XIR_OK);xr_xir_artifact_free(checked);checked=NULL;
 XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};CHECK(xr_xir_lower(special,&target,NULL,&lowered,NULL)==XR_XIR_OK);
 xr_xir_artifact_free(special);special=NULL;
 XrXirCSource generated={0};CHECK(xr_xir_emit_c(lowered,"source_library",1048576,&generated)==XR_XIR_OK);
 if(argc==2){FILE *cfile=fopen(argv[1],"wb");CHECK(cfile);CHECK(fwrite(generated.text,1,generated.length,cfile)==generated.length);CHECK(!fclose(cfile));}xr_xir_c_source_free(&generated);
 CHECK(xr_xir_vm_program_take(&lowered,(XrXirProgramBudget){1048576,1048576},&program)==XR_XIR_OK);
#else
 XrXirProgramSpec spec=source_library_program;
#if CONSUMER_KIND==2
 CHECK(xr_xir_checked_read(spec.proof.bytes,spec.proof.length,NULL,&checked,NULL)==XR_XIR_OK);
 CHECK(xr_xir_lower(checked,&spec.target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_artifact_free(checked);checked=NULL;
 XrXirCallEntry entries[6];XrXirVmBinding bindings[6];CHECK(spec.entry_count==6);
 for(uint32_t f=0;f<6;++f){CHECK(xr_xir_vm_bind(lowered,f,&bindings[f],&entries[f])==XR_XIR_OK);if(f%2)entries[f]=spec.entries[f];}
 spec.entries=entries;const XrXirModule *module=xr_xir_artifact_module(lowered);spec.types=module->types;spec.declarations=module->declarations;spec.proof=xr_xir_program_proof(lowered);
#endif
 CHECK(xr_xir_program_seal(&spec,(XrXirProgramBudget){1048576,1048576},&program)==XR_XIR_OK);
#endif
 runtime_attempts=0;CHECK(pair(program));size_t sites=runtime_attempts,live=runtime_live,bytes_live=runtime_bytes;
 for(size_t fail=0;fail<sites;++fail){runtime_attempts=0;runtime_fail_at=fail;CHECK(!pair(program));CHECK(runtime_live==live&&runtime_bytes==bytes_live);}
 runtime_fail_at=SIZE_MAX;xr_xir_program_drop(program);xr_xir_artifact_free(lowered);xr_xir_artifact_free(special);xr_xir_artifact_free(checked);
 CHECK(!runtime_live&&!runtime_bytes);printf("Checked Library consumer=%u twoInstance41 runtimeOOM=%zu physicalzero\n",CONSUMER_KIND,sites);return 0;
}
