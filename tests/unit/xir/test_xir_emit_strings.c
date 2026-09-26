/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_emit_strings.c - Generate actual native string code and reject malformed ownership
 *
 * KEY CONCEPT:
 *   Assert independent expected values and explicit ownership at every exit.
 */

#include "xir/xxir_emit_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_string_fixture.h"
int main(int argc, char **argv) {
    FILE *file = argc == 2 ? fopen(argv[1], "wb") : NULL;
    CHECK(argc == 1 || (argc == 2 && file));
    for (uint32_t mode = 0; mode < 2; ++mode) {
        XrXirArtifact *artifact = string_fixture(mode);
        const XrXirFunctionLayout *layout = xr_xir_artifact_layout(artifact, 0);
        CHECK(layout->owned_count == 4);
        uint32_t *offsets = (uint32_t *) layout->owned_offsets;
        uint32_t saved = offsets[0];
        offsets[0] = UINT32_MAX;
        CHECK(xr_xir_artifact_verify(artifact, NULL, NULL) == XR_XIR_BAD_LAYOUT);
        offsets[0] = saved;
        XrXirInstruction *ops = (XrXirInstruction *) xr_xir_artifact_module(artifact)->functions[0].instructions;
        CHECK(ops[1].op == XR_XIR_STRING_RETAIN);
        ops[1].op = XR_XIR_SCALAR_COPY;
        CHECK(xr_xir_artifact_verify(artifact, NULL, NULL) == XR_XIR_BAD_TYPE);
        ops[1].op = XR_XIR_STRING_RETAIN;
        ops[2].immediate = 3;
        CHECK(xr_xir_artifact_verify(artifact, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
        ops[2].immediate = 1;
        ops[2].args[0] = 4;
        CHECK(xr_xir_artifact_verify(artifact, NULL, NULL) == XR_XIR_BAD_VALUE);
        ops[2].args[0] = 3;
        char prefix[32];
        CHECK(snprintf(prefix, sizeof(prefix), "fixture_strings%u", mode) > 0);
        XrXirCSource source;
        CHECK(xr_xir_emit_leaf_c(artifact, prefix, 65536, &source) == XR_XIR_BAD_STAGE);
        CHECK(xr_xir_emit_c(artifact, prefix, 1, &source) == XR_XIR_BUDGET);
        CHECK(!source.text && !source.length);
        CHECK(xr_xir_emit_c(artifact, prefix, 65536, &source) == XR_XIR_OK);
        CHECK(strstr(source.text, "xr_xir_string_slot_clear") && !strstr(source.text, "({"));
        xr_xir_artifact_free(artifact);
        if (file) CHECK(fwrite(source.text, 1, source.length, file) == source.length);
        xr_xir_c_source_free(&source);
    }
    if (file) CHECK(fclose(file) == 0);
    puts("String XIR admission, ownership verification and native emission passed");
    return 0;
}
