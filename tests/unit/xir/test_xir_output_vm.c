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
    XrXirCallEntry entry;
    XrXirVmBinding binding;
    CHECK(xr_xir_vm_bind(artifact, 0, &binding, &entry) == XR_XIR_OK);
    output_cases(&entry);
    xr_xir_artifact_free(artifact);
    return 0;
}
