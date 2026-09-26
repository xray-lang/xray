/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_emit_output.c - Emit verified native output groups
 *
 * KEY CONCEPT:
 *   Real generated C is compiled before native results are accepted.
 */
#include "xir/xxir_emit_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_output_fixture.h"
int main(int argc, char **argv) {
    FILE *file = argc == 2 ? fopen(argv[1], "wb") : NULL;
    CHECK(argc == 1 || (argc == 2 && file));
    XrXirArtifact *artifact = output_fixture();
    XrXirInstruction *ops = (XrXirInstruction *) xr_xir_artifact_module(artifact)->functions[0].instructions;
    ops[0].immediate = -1;
    CHECK(xr_xir_artifact_verify(artifact, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    ops[0].immediate = 3;
    CHECK(xr_xir_artifact_verify(artifact, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    ops[0].immediate = 0;
    ops[1].args[1] = 2;
    CHECK(xr_xir_artifact_verify(artifact, NULL, NULL) == XR_XIR_BAD_VALUE);
    ops[1].args[1] = 1;
    XrXirCSource source;
    CHECK(xr_xir_emit_c(artifact, "fixture_output", 65536, &source) == XR_XIR_OK);
    CHECK(!strstr(source.text, "({"));
    xr_xir_artifact_free(artifact);
    if (file) {
        CHECK(fwrite(source.text, 1, source.length, file) == source.length);
        CHECK(fclose(file) == 0);
    }
    xr_xir_c_source_free(&source);
    return 0;
}
