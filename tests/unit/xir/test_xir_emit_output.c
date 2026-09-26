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
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_output_fixture.h"
static void range_admission(XrXirArtifact *artifact) {
    XrXirFunction *function = (XrXirFunction *) xr_xir_artifact_module(artifact)->functions;
    XrXirInstruction *ops = (XrXirInstruction *) function->instructions;
    ops[0].args[0] = 1;
    CHECK(xr_xir_artifact_verify(artifact, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    ops[0].args[0] = 0;
    ops[4].args[0] = 1;
    CHECK(xr_xir_artifact_verify(artifact, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    ops[4].args[0] = 3;
    CHECK(xr_xir_artifact_verify(artifact, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    ops[4].args[0] = 2;
    ++function->operand_count;
    CHECK(xr_xir_artifact_verify(artifact, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    --function->operand_count;
    XrXirFunctionLayout *layout = (XrXirFunctionLayout *) xr_xir_artifact_layout(artifact, 0);
    CHECK(layout->outgoing_count == 2);
    --layout->outgoing_count;
    CHECK(xr_xir_artifact_verify(artifact, NULL, NULL) == XR_XIR_BAD_LAYOUT);
    layout->outgoing_count += 2;
    CHECK(xr_xir_artifact_verify(artifact, NULL, NULL) == XR_XIR_BAD_LAYOUT);
    --layout->outgoing_count;
    CHECK(xr_xir_artifact_verify(artifact, NULL, NULL) == XR_XIR_OK);
}
static void write_admission(XrXirArtifact *artifact) {
    const XrXirFunction *function = &xr_xir_artifact_module(artifact)->functions[1];
    XrXirInstruction *ops = (XrXirInstruction *) function->instructions;
    ops[0].immediate = 3;
    CHECK(xr_xir_artifact_verify(artifact, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    ops[0].immediate = 2;
    ops[0].type = XR_XIR_UNIT;
    CHECK(xr_xir_artifact_verify(artifact, NULL, NULL) == XR_XIR_BAD_TYPE);
    ops[0].type = XR_XIR_BOOL;
    XrXirType *type = (XrXirType *) function->parameters;
    *type = XR_XIR_I64;
    CHECK(xr_xir_artifact_verify(artifact, NULL, NULL) == XR_XIR_BAD_TYPE);
    *type = XR_XIR_STRING;
    ops[0].args[1] = 1;
    CHECK(xr_xir_artifact_verify(artifact, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    ops[0].args[1] = 0;
    CHECK(xr_xir_artifact_verify(artifact, NULL, NULL) == XR_XIR_OK);
}
static void wide_boundary(void) {
    uint32_t *operands = xr_calloc(65537, sizeof(*operands)); CHECK(operands);
    XrXirInstruction ops[] = {
        {XR_XIR_PRINT, XR_XIR_UNIT, {0, 65536}, {0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0}
    };
    XrXirType type = XR_XIR_I64;
    XrXirBlock block = {0, 2};
    XrXirFunction function = {"wide", 4, &type, 1, XR_XIR_UNIT, &block, 1, ops, 2, operands, 65536};
    XrXirModule module = {XR_XIR_BUILT, &function, 1, NULL, NULL};
    XrXirArtifact *checked = NULL, *lowered = NULL;
    CHECK(xr_xir_check(&module, NULL, &checked, NULL) == XR_XIR_OK);
    operands[65535] = 99;
    CHECK(xr_xir_check(&module, NULL, &lowered, NULL) == XR_XIR_BAD_VALUE && !lowered);
    CHECK(xr_xir_artifact_verify(checked, NULL, NULL) == XR_XIR_OK);
    operands[65535] = 0;
    ++ops[0].args[1]; ++function.operand_count;
    CHECK(xr_xir_check(&module, NULL, &lowered, NULL) == XR_XIR_BAD_STRUCTURE && !lowered);
    --ops[0].args[1]; --function.operand_count;
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    XrXirBudget budget = xr_xir_default_budget();
    budget.frame_bytes = 8 + 65536 * sizeof(XrXirValue) - 1;
    CHECK(xr_xir_lower(checked, &target, &budget, &lowered, NULL) == XR_XIR_BUDGET && !lowered);
    ++budget.frame_bytes;
    CHECK(xr_xir_lower(checked, &target, &budget, &lowered, NULL) == XR_XIR_OK);
    CHECK(xr_xir_artifact_layout(lowered, 0)->outgoing_count == 65536);
    xr_xir_artifact_free(lowered); xr_xir_artifact_free(checked); xr_free(operands);
}
int main(int argc, char **argv) {
    FILE *file = argc == 2 ? fopen(argv[1], "wb") : NULL;
    CHECK(argc == 1 || (argc == 2 && file));
    XrXirArtifact *artifact = output_fixture();
    range_admission(artifact);
    write_admission(artifact);
    wide_boundary();
    XrXirInstruction *ops = (XrXirInstruction *) xr_xir_artifact_module(artifact)->functions[0].instructions;
    ops[0].immediate = -1;
    CHECK(xr_xir_artifact_verify(artifact, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    ops[0].immediate = 3;
    CHECK(xr_xir_artifact_verify(artifact, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    ops[0].immediate = 0;
    uint32_t *operands = (uint32_t *) xr_xir_artifact_module(artifact)->functions[0].operands;
    operands[1] = 2;
    CHECK(xr_xir_artifact_verify(artifact, NULL, NULL) == XR_XIR_BAD_VALUE);
    operands[1] = 1;
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
