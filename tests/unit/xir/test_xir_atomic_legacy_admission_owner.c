/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_atomic_legacy_admission_owner.c - Current compiler graph for preserved Atomic responsibilities
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_types.h"
#include "xir/xxir_declarations.h"
#include "shared/xnative_declaration.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_library_compile_owner.h"
#include "xir_atomic_nominal_cases.h"
#include "xir/xxir_operand_roles.h"
#include "xir_atomic_instruction_cases.h"
#include "xir_atomic_legacy_admission_fixture.h"
#include "xir_atomic_legacy_admission_cases.h"
#include "xir_atomic_type_wire_cases.h"
int main(void){
    (void)atomic_nominal_cases;(void)atomic_instruction_pipeline;
    XrCompileResources *none=NULL;source_program_compile_attempts=0;source_program_compile_fail_at=0;source_program_compile_injected=false;
    CHECK(xr_compile_resources_new(&library_compile_limits,&none)==XR_COMPILE_RESOURCE_OUT_OF_MEMORY && !none);
    CHECK(source_program_compile_injected&&!source_program_compile_live&&!source_program_compile_bytes);source_program_compile_fail_at=SIZE_MAX;
    legacy_admission_cases();legacy_types_cases();library_compile_observer_free();return 0;
}
