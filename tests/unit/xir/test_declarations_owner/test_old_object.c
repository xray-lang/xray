/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_old_object.c - Reject a previously executed native descriptor and proof
 */
#include "xir/xxir_program.h"
#include "xir/xxir_checked.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
extern const XrXirProgramSpec compile_owner_program;
int main(void) {
    XrCompileResourceLimits limits={UINT64_MAX,UINT64_MAX,UINT64_MAX};
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    CHECK(compile_owner_program.abi_version==27 && XR_XIR_PROGRAM_ABI_VERSION==28);
    _Static_assert(XR_XIR_VALUE_ABI_VERSION==18 && XR_XIR_CALL_ABI_VERSION==22,"unchanged value and call ABI");
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&context,&compile_owner_program,&program)==XR_XIR_BAD_LAYOUT && !program);
    XrXirArtifact *artifact=NULL;
    CHECK(compile_owner_program.proof.bytes[8]==22 && compile_owner_program.proof.bytes[12]==58);
    CHECK(xr_xir_compile_checked_read(&context,compile_owner_program.proof.bytes,
        compile_owner_program.proof.length,&artifact,NULL)==XR_XIR_BAD_STRUCTURE && !artifact);
    xr_compile_resources_release(context.resources);
    puts("real old Program27 and Checked22/58 rejected before new identity traversal");
    return 0;
}
