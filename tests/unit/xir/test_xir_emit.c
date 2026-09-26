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
    XrXirArtifact *checked = fixture_checked();
    XrXirCSource source;
    CHECK(xr_xir_emit_leaf_c(checked, "fixture", 65536, &source) == XR_XIR_BAD_STAGE);
    CHECK(!source.text && !source.length);
    xr_xir_artifact_free(checked);
    XrXirArtifact *artifact = fixture_lowered();
    CHECK(xr_xir_emit_leaf_c(artifact, "invalid;", 65536, &source) == XR_XIR_BAD_STRUCTURE);
    CHECK(xr_xir_emit_leaf_c(artifact, "fixture", 1, &source) == XR_XIR_BUDGET);
    CHECK(!source.text && !source.length);
    CHECK(xr_xir_emit_leaf_c(artifact, "fixture", 65536, &source) == XR_XIR_OK);
    xr_xir_artifact_free(artifact);
    CHECK(source.length == strlen(source.text));
    CHECK(!strstr(source.text, "({"));
    if (argc == 2) {
        FILE *file = fopen(argv[1], "wb");
        CHECK(file);
        CHECK(fwrite(source.text, 1, source.length, file) == source.length);
        CHECK(fclose(file) == 0);
    } else CHECK(argc == 1);
    xr_xir_c_source_free(&source);
    CHECK(!source.text && !source.length);
    puts("XIR C emission and generated-output verifier passed");
    return 0;
}
