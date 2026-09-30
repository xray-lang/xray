/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_initialization_failure_source_runtime.c - Source-owned artifact runtime boundary
 *
 * KEY CONCEPT:
 *   Actual counted runtime implementations consume the detached Checked artifact
 *   only after the producer session and query snapshot have been destroyed.
 */
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#include "xir_runtime_allocations.h"
#include "xir_initialization_failure_execution.h"
XR_FUNC size_t xr_test_initialization_runtime_live(void){return runtime_live;}
XR_FUNC size_t xr_test_initialization_runtime_bytes(void){return runtime_bytes;}
XR_FUNC void xr_test_initialization_source_run(XrXirArtifact **checked){
    CHECK(checked&&*checked);XrXirArtifact *special=NULL,*lowered=NULL;
    CHECK(xr_xir_specialize(*checked,NULL,&special,NULL)==XR_XIR_OK);
    xr_xir_artifact_free(*checked);*checked=NULL;
    CHECK(xr_xir_artifact_verify(special,NULL,NULL)==XR_XIR_OK);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(special,&target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_artifact_free(special);
    uint32_t entry=xr_xir_artifact_module(lowered)->declarations->entry_function;
    XrXirProgram *program=NULL;
    CHECK(xr_xir_vm_program_take(&lowered,(XrXirProgramBudget){33554432,64000000},&program)==XR_XIR_OK);
    XrXirValue held[3]={{0}};init_pair(program,entry,held);init_protocol_failures(program,entry);init_runtime_faults(program,entry);
    xr_xir_program_drop(program);init_retained(held);CHECK(!runtime_live&&!runtime_bytes);
    puts("direct Source Checked VM: producer destroyed, initialization41, retained string and physical zero PASS");
}
