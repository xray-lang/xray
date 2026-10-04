/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_module_state_failures.c - Execute complete initialization failure Programs
 *
 * KEY CONCEPT:
 *   VM and generated native code share independent sticky failure oracles.
 */

#include "xir/xxir_emit_c.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_source_program_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_module_state_failure_fixture.h"
#include "xir_module_state_failure_cases.h"
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==3);
    uint32_t first=argc==3?(uint32_t)strtoul(argv[2],NULL,10):2,last=argc==3?first:4;
    CHECK(first>=2 && last<=4);
    for(uint32_t scenario=first;scenario<=last;++scenario) {
        const XrXirCompileContext *context=source_program_owner(UINT64_C(64)*1024*1024,128000000);
        XrXirArtifact *artifact=state_failure_fixture(context,scenario);
        if(argc==3) {
            char prefix[32];CHECK(snprintf(prefix,sizeof(prefix),"state_failure_%u",scenario)>0);
            XrXirCSource source={0};CHECK(xr_xir_compile_emit_c(artifact,prefix,1024*1024,&source)==XR_XIR_OK);
            CHECK(!strstr(source.text,"({"));FILE *file=fopen(argv[1],"wb");CHECK(file);
            CHECK(fwrite(source.text,1,source.length,file)==source.length && !fclose(file));
            xr_xir_compile_c_source_free(&source);
        }
        XrXirProgram *program=NULL;
        CHECK(xr_xir_compile_vm_program_take(&artifact,&program)==XR_XIR_OK && !artifact && program);
        state_failure_pair(program,scenario);
    }
    source_program_owners_free();xr_free(runtime_owned);return 0;
}
