/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_class_inheritance_native.c - Identical native output and retired receivers
 *
 * KEY CONCEPT:
 *   The actual generated Program owns its packet and derived type facts; the
 *   native host adds no dispatch permission or replacement class representation.
 */
#include "xir/xxir_internal.h"
#include "base/xresource_limits.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"class native %d: %s\n",__LINE__,#c);exit(1);}} while(0)
#include "class_inheritance_cases.h"
XR_DATA const XrXirProgramSpec class_inheritance_0_program,class_inheritance_1_program,class_inheritance_2_program;
XR_DATA const uint32_t class_inheritance_0_metadata[4],class_inheritance_1_metadata[4],class_inheritance_2_metadata[4];
XR_DATA const uint32_t class_inheritance_1_default_test;
int main(void) {
    class_inheritance_golden();XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    for(unsigned mode=0;mode<3;++mode) {
        const uint32_t *words=mode==2?class_inheritance_2_metadata:mode?class_inheritance_1_metadata:class_inheritance_0_metadata;
        ClassInheritancePlan plan={words[0],words[1],words[2],words[3],mode?42:0,mode!=0,mode==2};
        XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,
            mode==2?&class_inheritance_2_program:mode?&class_inheritance_1_program:&class_inheritance_0_program,&program)==XR_XIR_OK);
        if(mode==1)class_inheritance_defaults_execute(program,class_inheritance_1_default_test);
        class_inheritance_execute(program,&plan);xr_xir_compile_program_drop(program);
        CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && before.held_bytes==after.held_bytes);
    }
    xr_compile_resources_release(context.resources);CHECK(!cell_owner_live && !cell_owner_bytes);
    puts("Class inheritance generated C output/receiver/physical retirement controls PASS");return 0;
}
