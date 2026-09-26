/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_output_vm.c - Lowered VM output against independent bytes
 *
 * KEY CONCEPT:
 *   Formatting is validated separately from instruction execution.
 */
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_output_fixture.h"
#include "xir_output_cases.h"
int main(void) {
    XrXirArtifact *artifact = output_fixture();
    XrXirCallEntry entries[3];
    XrXirVmBinding bindings[3];
    for (uint32_t i = 0; i < 3; ++i)
        CHECK(xr_xir_vm_bind(artifact, i, &bindings[i], &entries[i]) == XR_XIR_OK);
    output_cases(entries);
    write_cases(entries);
    write_action_admission();
    xr_xir_artifact_free(artifact);
    return 0;
}
