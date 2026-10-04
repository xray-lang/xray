/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_module_state_one_native.c - A generated String state program with fixed expectations
 *
 * KEY CONCEPT:
 *   Native code preserves the full String workload after the source owner has gone.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while(0)
#include "xir_source_program_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_module_state_one_cases.h"
XR_DATA const XrXirProgramSpec module_state_one_program;
XR_DATA const uint32_t module_state_one_advance;
XR_DATA const uint32_t module_state_one_snapshot,module_state_one_left,module_state_one_right;
int main(void) {
    CHECK(module_state_one_advance<module_state_one_program.entry_count);
    CHECK(module_state_one_snapshot<module_state_one_program.entry_count &&
        module_state_one_left<module_state_one_program.entry_count && module_state_one_right<module_state_one_program.entry_count);
    StateOneShape shape=state_one_shape(module_state_one_program.declarations,module_state_one_advance,
        module_state_one_snapshot,module_state_one_left,module_state_one_right);
    CHECK(module_state_one_program.entries[shape.advance].result==XR_XIR_UNIT &&
        !module_state_one_program.entries[shape.advance].parameter_count);
    const XrXirCompileContext *context=source_program_owner(UINT64_C(32)*1024*1024,UINT64_C(64000000));
    /* All six old descriptor-version refusals remain real native descriptor
     * refusals at the current sealed Program ABI boundary. */
    for(uint32_t instance=0;instance<2;++instance) for(uint32_t version=4;version<=6;++version) {
        XrXirProgramSpec stale=module_state_one_program; stale.abi_version=version;
        XrXirProgram *rejected=NULL;
        CHECK(xr_xir_compile_program_seal(context,&stale,&rejected)==XR_XIR_BAD_LAYOUT && !rejected);
    }
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(context,&module_state_one_program,&program)==XR_XIR_OK && program);
    state_one_pair(program,shape);
    source_program_owners_free(); return 0;
}
