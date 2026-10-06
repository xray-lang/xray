/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_library_generics_native.c - Genuine generated-C Library execution
 */
#include "xir/xxir_program.h"
#include "xir/xxir_checked.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#include "xir_library_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_effects.c"
#include "xir_library_generics_oracle.h"
#include "generic-library.0.c.h"
#include "generic-library.1.c.h"
#include "generic-library.2.c.h"
#include "generic-library.3.c.h"
extern const XrXirProgramSpec generic_library_0_program;
extern const XrXirProgramSpec generic_library_1_program;
extern const XrXirProgramSpec generic_library_2_program;
extern const XrXirProgramSpec generic_library_3_program;
int main(void){LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    const XrXirProgramSpec *specs[]={&generic_library_0_program,&generic_library_1_program,&generic_library_2_program,&generic_library_3_program};
    const uint32_t *entries[]={generic_library_0_oracle_entries,generic_library_1_oracle_entries,generic_library_2_oracle_entries,generic_library_3_oracle_entries};
    for(unsigned mode=0;mode<4;++mode){
        XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&owner.context,specs[mode],&program)==XR_XIR_OK);
        library_generics_program_oracle(program,entries[mode]);
        printf("generic Library genuine native mode%u independent 42/NUL3/false/Enum73 escaped/physical0 PASS\n",mode);
    }
    library_compile_owner_drop(&owner);library_compile_observer_free();return 0;}
