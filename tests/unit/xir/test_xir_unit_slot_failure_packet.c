/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_unit_slot_failure_packet.c - Sticky initialization failure execution qualification
 *
 * KEY CONCEPT:
 *   Frozen Checked bytes feed independent source-free runtime expectations.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#include "xir_runtime_allocations.h"
#include "xir_unit_slot_failure_execution.h"
int main(int argc,char **argv){CHECK(argc==1||argc==2);
FILE *file=fopen(XR_CHECKED_FIXTURE,"rb");
CHECK(file&&!fseek(file,0,SEEK_END));
long size=ftell(file);
CHECK(size>=64&&size<=262144&&!fseek(file,0,SEEK_SET));
uint8_t *bytes=malloc((size_t)size);
CHECK(bytes&&fread(bytes,1,(size_t)size,file)==(size_t)size&&!fclose(file));

 XrXirArtifact *checked=NULL,*special=NULL,*lowered=NULL;
CHECK(xr_xir_checked_read(bytes,(size_t)size,NULL,&checked,NULL)==XR_XIR_OK);
free(bytes);
CHECK(xr_xir_specialize(checked,NULL,&special,NULL)==XR_XIR_OK);
xr_xir_artifact_free(checked);
CHECK(xr_xir_artifact_verify(special,NULL,NULL)==XR_XIR_OK);

 XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
CHECK(xr_xir_lower(special,&target,NULL,&lowered,NULL)==XR_XIR_OK);
xr_xir_artifact_free(special);
const XrXirModule *module=xr_xir_artifact_module(lowered);

 uint32_t entry=module->declarations->entry_function;
 XrXirCSource c={0};
CHECK(xr_xir_emit_c(lowered,"unit_slot_failure",1048576,&c)==XR_XIR_OK);
if(argc==2){file=fopen(argv[1],"wb");
CHECK(file&&fwrite(c.text,1,c.length,file)==c.length&&!fclose(file));
}xr_xir_c_source_free(&c);

 XrXirProgram *program=NULL;
CHECK(xr_xir_vm_program_take(&lowered,(XrXirProgramBudget){33554432,64000000},&program)==XR_XIR_OK);
XrXirValue held[3]={{0}};
init_pair(program,entry,held);
init_protocol_failures(program,entry);
init_runtime_faults(program,entry);
xr_xir_program_drop(program);
init_retained(held);
CHECK(!runtime_live&&!runtime_bytes);
puts("source-free VM initialization failure41 reverse-release retained string PASS");
return 0;
}
