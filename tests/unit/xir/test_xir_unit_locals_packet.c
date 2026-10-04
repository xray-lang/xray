/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_unit_locals_packet.c - Zero-size local execution qualification
 *
 * KEY CONCEPT:
 *   Frozen Checked bytes feed independent source-free runtime expectations.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_generic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#include "xir_unit_locals_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_unit_locals_execution.h"
int main(int argc,char **argv){CHECK(argc==1||argc==2);
UnitCompileOwner owner={0};unit_compile_owner_new(&owner);
FILE *file=fopen(XR_CHECKED_FIXTURE,"rb");
CHECK(file&&!fseek(file,0,SEEK_END));
long size=ftell(file);
CHECK(size>=64&&size<=262144&&!fseek(file,0,SEEK_SET));
uint8_t *bytes=NULL;
CHECK(xr_compile_resources_alloc(owner.context.resources,(size_t)size,(void **)&bytes)==XR_COMPILE_RESOURCE_OK);
CHECK(xr_compile_resources_work(owner.context.resources,(uint64_t)size)==XR_COMPILE_RESOURCE_OK);
CHECK(bytes&&fread(bytes,1,(size_t)size,file)==(size_t)size&&!fclose(file));

 XrXirArtifact *checked=NULL,*special=NULL,*lowered=NULL;
CHECK(xr_xir_compile_checked_read(&owner.context,bytes,(size_t)size,&checked,NULL)==XR_XIR_OK);
xr_compile_resources_free(bytes);
CHECK(xr_xir_compile_specialize(checked,&special,NULL)==XR_XIR_OK);
xr_xir_compile_artifact_free(checked);
CHECK(xr_xir_compile_artifact_verify(special,NULL)==XR_XIR_OK);

 XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
CHECK(xr_xir_compile_lower(special,&target,&lowered,NULL)==XR_XIR_OK);
xr_xir_compile_artifact_free(special);
const XrXirModule *module=xr_xir_compile_artifact_module(lowered);

 UnitEntries entries=unit_entries(module);
 XrXirCSource c={0};
CHECK(xr_xir_compile_emit_c(lowered,"unit_locals",1048576,&c)==XR_XIR_OK);
if(argc==2){file=fopen(argv[1],"wb");
CHECK(file&&fwrite(c.text,1,c.length,file)==c.length&&!fclose(file));
}xr_xir_compile_c_source_free(&c);

 XrXirProgram *program=NULL;
CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK);
XrXirValue held[2]={{0}};
unit_pair(program,entries,held);
unit_cancel(program,entries);
unit_faults(program,entries);
xr_xir_compile_program_drop(program);
unit_retained(held);
CHECK(!runtime_live&&!runtime_bytes);
unit_compile_owner_free(&owner);unit_compile_report();
puts("source-free VM Unit trace1234 result41 yield/cancel retained string PASS");
return 0;
}
