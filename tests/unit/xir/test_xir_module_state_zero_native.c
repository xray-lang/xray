/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_module_state_zero_native.c - A generated numeric state program with fixed expectations
 *
 * KEY CONCEPT:
 *   Native code preserves the full numeric workload after the source owner has gone.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while(0)
#include "xir_source_program_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_module_state_zero_cases.h"
XR_DATA const XrXirProgramSpec module_state_zero_program;
XR_DATA const uint32_t module_state_zero_advance;
int main(void) {
    CHECK(module_state_zero_advance<module_state_zero_program.entry_count);
    StateZeroShape shape=state_zero_shape(module_state_zero_program.declarations,module_state_zero_advance);
    CHECK(module_state_zero_program.entries[shape.advance].result==XR_XIR_I64 &&
        !module_state_zero_program.entries[shape.advance].parameter_count);
    const XrXirCompileContext *context=source_program_owner(UINT64_C(32)*1024*1024,UINT64_C(64000000));
    /* All six old descriptor-version refusals remain real native descriptor
     * refusals at the current sealed Program ABI boundary. */
    for(uint32_t instance=0;instance<2;++instance) for(uint32_t version=4;version<=6;++version) {
        XrXirProgramSpec stale=module_state_zero_program; stale.abi_version=version;
        XrXirProgram *rejected=NULL;
        CHECK(xr_xir_compile_program_seal(context,&stale,&rejected)==XR_XIR_BAD_LAYOUT && !rejected);
    }
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(context,&module_state_zero_program,&program)==XR_XIR_OK && program);
    state_zero_pair(program,shape);
    source_program_owners_free(); return 0;
}
