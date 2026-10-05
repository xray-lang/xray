/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_dependency_packet.c - Dependency-ready source execution qualification
 *
 * KEY CONCEPT:
 *   Frozen Checked bytes feed independent source-free runtime expectations.
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
#include "xir_runtime_allocations.h"
#include "xir_dependency_execution.h"
int main(int argc,char **argv){CHECK(argc==1||argc==2);
 const XrXirCompileContext compile=*effects_source_owner(UINT64_C(64)*1024*1024,UINT64_C(128000000));
 FILE *file=fopen(XR_CHECKED_FIXTURE,"rb");CHECK(file&&!fseek(file,0,SEEK_END));long size=ftell(file);
 CHECK(size>=64&&size<=262144&&!fseek(file,0,SEEK_SET));uint8_t *bytes=NULL;
 CHECK(xr_compile_resources_alloc(compile.resources,(size_t)size,(void **)&bytes)==XR_COMPILE_RESOURCE_OK);
 CHECK(fread(bytes,1,(size_t)size,file)==(size_t)size&&!fclose(file));XrXirArtifact *checked=NULL,*special=NULL,*lowered=NULL;
 CHECK(xr_xir_compile_checked_read(&compile,bytes,(size_t)size,&checked,NULL)==XR_XIR_OK);memset(bytes,0xcc,(size_t)size);xr_compile_resources_free(bytes);
 CHECK(xr_xir_compile_specialize(checked,&special,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
 CHECK(xr_xir_compile_artifact_verify(special,NULL)==XR_XIR_OK);
 const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
 CHECK(xr_xir_compile_lower(special,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(special);
 const XrXirModule *module=xr_xir_compile_artifact_module(lowered);ReadyEntries entries=ready_select(module);
 XrXirCSource c={0};CHECK(xr_xir_compile_emit_c(lowered,"dependency_ready",1048576,&c)==XR_XIR_OK);
 printf("ready generated C %zu bytes / %u functions\n",c.length,module->function_count);
 if(argc==2){file=fopen(argv[1],"wb");CHECK(file&&fwrite(c.text,1,c.length,file)==c.length);
 CHECK(fprintf(file,"\nconst uint32_t dependency_ready_functions[3]={%uu,%uu,%uu};\n",entries.answer,entries.retained,entries.values)>0);CHECK(!fclose(file));}
 xr_xir_compile_c_source_free(&c);XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK);
 XrXirValue held[2][2]={{{0}}};ready_pair(program,entries,held);ready_runtime_faults(program,entries);
 xr_xir_compile_program_drop(program);ready_retained(held);CHECK(!runtime_live&&!runtime_bytes);effects_source_owners_free();
 puts("source-free VM readiness41 trace12 retained strings/Array PASS");return 0;
}
