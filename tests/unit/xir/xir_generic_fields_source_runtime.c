/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generic_fields_source_runtime.c - Source-owned artifact runtime boundary
 *
 * KEY CONCEPT:
 *   Actual counted runtime implementations consume the detached Checked artifact
 *   only after the producer session and query snapshot have been destroyed.
 */
#include "xir/xxir_vm.h"
#include "xir/xxir_generic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#include "xir_runtime_allocations.h"
#include "xir_generic_fields_execution.h"
XR_FUNC size_t xr_test_generic_fields_runtime_live(void){return runtime_live;}
XR_FUNC size_t xr_test_generic_fields_runtime_bytes(void){return runtime_bytes;}
XR_FUNC void xr_test_generic_fields_source_run(XrXirArtifact **checked){
    CHECK(checked&&*checked);CHECK(xr_xir_compile_artifact_verify(*checked,NULL)==XR_XIR_OK);XrXirArtifact *special=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_specialize(*checked,&special,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(*checked);*checked=NULL;
    CHECK(xr_xir_compile_artifact_verify(special,NULL)==XR_XIR_OK);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(special,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(special);
    GenericFieldEntries entries=generic_fields_entries(xr_xir_compile_artifact_module(lowered));
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK);
    XrXirValue held[2]={{0}};generic_fields_pair(program,entries,held);generic_fields_faults(program,entries);
    xr_xir_compile_program_drop(program);generic_fields_retained(held);CHECK(!runtime_live&&!runtime_bytes);
    puts("direct Source Checked VM: producer destroyed, generic fields result41, retained string and physical zero PASS");
}
