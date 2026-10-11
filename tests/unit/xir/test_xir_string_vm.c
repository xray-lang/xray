/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_string_vm.c - VM and mixed string calls with results outliving artifacts
 *
 * KEY CONCEPT:
 *   A complete code lease preserves real environments through Instance retirement.
 */
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "xir_string_fixture.h"
#include "xir_string_cases.h"
#include "xir_string_vm_program.h"
XR_DATA const XrXirCallEntry fixture_strings0_entries[5];
XR_DATA const XrXirCallEntry fixture_strings1_entries[5];
int main(void) {
    const XrXirCallEntry *tables[]={fixture_strings0_entries,fixture_strings1_entries};
    for (uint32_t mixed=0;mixed<2;++mixed)
    for (uint32_t variant=0;variant<2;++variant) for (uint32_t mode=0;mode<10;++mode) {
        SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
        XrXirArtifact *artifact=string_fixture(&owner.context,variant);
        XrXirRunContext context={100,1024,0,0,0,0};XrXirValue result={0};
        CHECK(xr_xir_compile_vm_run(artifact,0,&context,NULL,0,&result)==XR_XIR_RUN_BAD_ARTIFACT);
        XrXirProgram *program=NULL;
        CHECK(string_vm_program_take(&artifact,mixed?&tables[variant][1]:NULL,&program)==XR_XIR_OK &&
            program && !artifact);
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
    puts("VM and mixed native string execution and artifact-independent results passed");
    return 0;
}
