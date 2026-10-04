/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_module_state_failure_native.c - Execute complete initialization failure Programs
 *
 * KEY CONCEPT:
 *   VM and generated native code share independent sticky failure oracles.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_source_program_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_module_state_failure_cases.h"
#define STATE_JOIN_(a,b) a##b
#define STATE_JOIN(a,b) STATE_JOIN_(a,b)
XR_DATA const XrXirProgramSpec STATE_JOIN(XR_STATE_FAILURE_PREFIX,_program);
int main(void) {
    const XrXirCompileContext *context=source_program_owner(UINT64_C(32)*1024*1024,64000000);
    const XrXirProgramSpec *spec=&STATE_JOIN(XR_STATE_FAILURE_PREFIX,_program);
    for(uint32_t instance=0;instance<2;++instance)for(uint32_t version=4;version<=6;++version) {
        XrXirProgramSpec stale=*spec;stale.abi_version=version;XrXirProgram *rejected=NULL;
        CHECK(xr_xir_compile_program_seal(context,&stale,&rejected)==XR_XIR_BAD_LAYOUT && !rejected);
    }
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(context,spec,&program)==XR_XIR_OK && program);
    state_failure_pair(program,XR_STATE_FAILURE_SCENARIO);
    source_program_owners_free();xr_free(runtime_owned);return 0;
}
