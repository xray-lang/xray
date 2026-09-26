/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_string_vm.c - VM and mixed string calls with results outliving artifacts
 *
 * KEY CONCEPT:
 *   Assert independent expected values and explicit ownership at every exit.
 */

#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_string_fixture.h"
#include "xir_string_cases.h"
XR_DATA const XrXirCallEntry fixture_strings0_entries[3];
XR_DATA const XrXirCallEntry fixture_strings1_entries[3];
int main(void) {
    const XrXirCallEntry *tables[] = {fixture_strings0_entries, fixture_strings1_entries};
    for (uint32_t mixed = 0; mixed < 2; ++mixed)
    for (uint32_t variant = 0; variant < 2; ++variant) for (uint32_t mode = 0; mode < 9; ++mode) {
        XrXirArtifact *artifact = string_fixture(variant);
        XrXirCallEntry entries[3];
        XrXirVmBinding bindings[3];
        for (uint32_t i = 0; i < 3; ++i)
            CHECK(xr_xir_vm_bind(artifact, i, &bindings[i], &entries[i]) == XR_XIR_OK);
        if (mixed) entries[1] = tables[variant][1];
        XrXirRunContext context = {100, 1024, 0, 0, 0, 0};
        XrXirValue result = {0};
        CHECK(xr_xir_vm_run(artifact, 0, &context, NULL, 0, &result) == XR_XIR_RUN_BAD_ARTIFACT);
        XrXirValue value = string_cases(entries, variant, mode);
        xr_xir_artifact_free(artifact);
        if (value.type == XR_XIR_STRING) string_bytes(&value, string_expected, sizeof(string_expected) - 1);
        xr_xir_value_drop(&value);
    }
    puts("VM and mixed native string execution and artifact-independent results passed");
    return 0;
}
