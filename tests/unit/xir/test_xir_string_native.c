/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_string_native.c - Native string results without VM or Source dependencies
 *
 * KEY CONCEPT:
 *   Generated complete descriptors are sealed before real Instance execution.
 */
#include "xir/xxir_program.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "xir_string_cases.h"
XR_DATA const XrXirProgramSpec fixture_strings0_program;
XR_DATA const XrXirProgramSpec fixture_strings1_program;
int main(void) {
    const XrXirProgramSpec *specs[]={&fixture_strings0_program,&fixture_strings1_program};
    for (uint32_t variant=0;variant<2;++variant) for (uint32_t mode=0;mode<10;++mode) {
        SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
        XrXirProgram *program=NULL;
        CHECK(specs[variant]->entry_count==5 && specs[variant]->declarations->functions[0].exported &&
            specs[variant]->declarations->functions[2].exported);
        CHECK(xr_xir_compile_program_seal(&owner.context,specs[variant],&program)==XR_XIR_OK && program);
        XrXirValue values[2];
        for (uint32_t i=0;i<2;++i) values[i]=string_cases(program,variant,mode);
        xr_xir_compile_program_drop(program);
        for (uint32_t i=0;i<2;++i) {
        XrXirValue value=values[i];
        if (value.type==XR_XIR_STRING) string_bytes(&value,string_expected,sizeof(string_expected)-1);
        else if (value.type) {
            XrCompileResourceStats held={0};
            CHECK(xr_compile_resources_stats(owner.context.resources,&held)==XR_COMPILE_RESOURCE_OK);
            CHECK(held.live_bytes>owner.baseline.live_bytes);
            XrXirDomain *reader=NULL;CHECK(xr_xir_domain_new(65536,&reader)==XR_XIR_VALUE_OK);
            string_error_bytes(&value,reader);xr_xir_domain_drop(reader);
        }
        xr_xir_value_drop(&value);
        }
        source_fixture_owner_free(&owner);
    }
    puts("Native Unicode string calls, output, suspension, failure and owned results passed");
    return 0;
}
