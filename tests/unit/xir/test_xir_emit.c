/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_emit.c - Scalar execution verification
 *
 * KEY CONCEPT:
 *   Validate emission admission and write real generated C for the native gate.
 */

#include "xir_execution_fixture.h"
#include "xir/xxir_emit_c.h"
#include <string.h>

int main(int argc, char **argv) {
    scalar_compile_begin();
    XrXirArtifact *checked = fixture_checked(&scalar_owner.context);
    XrXirCSource source = {0};
    CHECK(xr_xir_compile_emit_leaf_c(checked, "fixture", 1048576, &source) == XR_XIR_BAD_STAGE);
    CHECK(!source.text && !source.length);
    xr_xir_compile_artifact_free(checked);
    XrXirArtifact *artifact = fixture_lowered(&scalar_owner.context);
    CHECK(xr_xir_compile_emit_leaf_c(artifact, "invalid;", 1048576, &source) == XR_XIR_BAD_STRUCTURE);
    CHECK(xr_xir_compile_emit_leaf_c(artifact, "fixture", 1, &source) == XR_XIR_BUDGET);
    CHECK(!source.text && !source.length);
    CHECK(xr_xir_compile_emit_leaf_c(artifact, "fixture", 1048576, &source) == XR_XIR_OK);
    xr_xir_compile_artifact_free(artifact);
    CHECK(source.length == strlen(source.text));
    CHECK(!strstr(source.text, "({"));
    XrXirArtifact *uninitialized = uninitialized_leaf_fixture(&scalar_owner.context);
    XrXirCSource extra = {0};
    CHECK(xr_xir_compile_emit_leaf_c(uninitialized, "uninitialized", 65536, &extra) == XR_XIR_OK);
    xr_xir_compile_artifact_free(uninitialized);
    if (argc == 2) {
        FILE *file = fopen(argv[1], "wb");
        CHECK(file);
        CHECK(fwrite(source.text, 1, source.length, file) == source.length);
        CHECK(fwrite(extra.text, 1, extra.length, file) == extra.length);
        CHECK(fclose(file) == 0);
    } else CHECK(argc == 1);
    xr_xir_compile_c_source_free(&extra);
    xr_xir_compile_c_source_free(&source);
    CHECK(!source.text && !source.length);
    scalar_compile_owner_free(&scalar_owner);
    puts("XIR C emission and generated-output verifier passed");
    return 0;
}
