/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_stage_cases.h - Array roles, frozen layouts and hostile metadata
 *
 * KEY CONCEPT:
 *   Logical places never gain ordinary ownership through a forged use.
 */
#ifndef XIR_ARRAY_STAGE_CASES_H
#define XIR_ARRAY_STAGE_CASES_H
#include "xir_array_metadata_fixture.h"
#include "xir/xxir_operand_roles.h"

static void array_stage_layout(void) {
    XirArrayMetadataFixture f; xir_array_metadata_init(&f);
    CHECK(xr_xir_verify(&f.module, NULL, NULL) == XR_XIR_OK);
    CHECK(xr_xir_place_kind(&f.functions[1], 3) == XR_XIR_PLACE_LOCAL);
    CHECK(xr_xir_place_kind(&f.functions[1], 5) == XR_XIR_PLACE_CELL);
    CHECK(xr_xir_place_kind(&f.functions[1], 6) == XR_XIR_PLACE_SLOT);
    CHECK(xr_xir_place_kind(&f.functions[1], UINT32_MAX) == XR_XIR_PLACE_NONE);
    CHECK(xr_xir_place_kind(&f.functions[2], 0) == XR_XIR_PLACE_NONE);
    XrXirArtifact *checked = NULL, *lowered = NULL;
    CHECK(xr_xir_check(&f.module, NULL, &checked, NULL) == XR_XIR_OK);
    memset(&f, 0xCC, sizeof(f));
    CHECK(xr_xir_lower(checked, &fixture_target, NULL, &lowered, NULL) == XR_XIR_OK);
    const XrXirFunctionLayout *layout = xr_xir_artifact_layout(lowered, 1);
    CHECK(layout->slot_count == 14 && layout->frame_bytes == 72 && layout->outgoing_count == 2);
    CHECK(layout->offsets[5] == UINT32_MAX && layout->offsets[6] == UINT32_MAX);
    CHECK(layout->owned_count == 3 && layout->owned_offsets[0] == 16 &&
        layout->owned_offsets[1] == 24 && layout->owned_offsets[2] == 32);
    CHECK(xr_xir_artifact_module(lowered)->functions[1].instructions[3].op == XR_XIR_OWNED_LOCAL_NEW);
    uint32_t *offsets = (uint32_t *) layout->offsets;
    offsets[5] = 40;
    CHECK(xr_xir_artifact_verify(lowered, NULL, NULL) == XR_XIR_BAD_LAYOUT);
    offsets[5] = UINT32_MAX;
    uint32_t *owned = (uint32_t *) layout->owned_offsets;
    owned[0] = UINT32_MAX;
    CHECK(xr_xir_artifact_verify(lowered, NULL, NULL) == XR_XIR_BAD_LAYOUT);
    owned[0] = 16;
    CHECK(xr_xir_artifact_verify(lowered, NULL, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(lowered); lowered = NULL;
    XrXirBudget budget = xr_xir_default_budget(); budget.frame_bytes = 103;
    CHECK(xr_xir_lower(checked, &fixture_target, &budget, &lowered, NULL) == XR_XIR_BUDGET && !lowered);
    budget.frame_bytes = 104;
    CHECK(xr_xir_lower(checked, &fixture_target, &budget, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(lowered); xr_xir_artifact_free(checked);
}

static void array_stage_attacks(void) {
    for (uint32_t attack = 0; attack < 30; ++attack) {
        XirArrayMetadataFixture f; xir_array_metadata_init(&f);
        XrXirType array = (XrXirType) 256;
        if (attack == 0) f.ops[2].type = XR_XIR_STRING;
        if (attack == 1) f.ops[5].type = (XrXirType) 257;
        if (attack == 2) f.ops[5].args[0] = 2;
        if (attack == 3) f.ops[6].immediate = 1;
        if (attack == 4) f.ops[6].type = (XrXirType) 258;
        if (attack == 5) f.ops[9].args[1] = 2;
        if (attack == 6) f.ops[2].args[0] = 1;
        if (attack == 7) f.ops[9].args[0] = 1;
        if (attack == 8) f.functions[1].operand_count = 6;
        if (attack == 9) f.operands[2] = 2;
        if (attack == 10) f.operands[0] = 5;
        if (attack == 11) f.ops[10].args[0] = 2;
        if (attack == 12) f.ops[7] = (XrXirInstruction) {XR_XIR_CONST_BOOL, XR_XIR_BOOL, {0}, {0}, 0, {0}};
        if (attack == 13) f.ops[8].type = XR_XIR_BOOL;
        if (attack == 14) f.operands[4] = 2;
        if (attack == 15) f.ops[5].targets[0] = 1;
        if (attack == 16) f.ops[12].args[1] = 1;
        if (attack == 17) { f.operands[2] = 6; f.slot.mutable = 0; }
        if (attack == 18) f.ops[11] = (XrXirInstruction) {XR_XIR_COPY, array, {5}, {0}, 0, {0}};
        if (attack == 19) f.ops[11] = (XrXirInstruction) {XR_XIR_LOCAL_READ, array, {5}, {0}, 0, {0}};
        if (attack == 20) f.ops[11] = (XrXirInstruction) {XR_XIR_SLOT_STORE, XR_XIR_UNIT, {6}, {0}, 0, {0}};
        if (attack == 21) f.ops[13].args[0] = 5;
        if (attack == 22) f.ops[11] = (XrXirInstruction) {XR_XIR_CELL_WRITE, XR_XIR_UNIT, {4, 5}, {0}, 0, {0}};
        if (attack == 23) f.ops[11] = (XrXirInstruction) {XR_XIR_LOCAL_NEW, array, {6}, {0}, 0, {0}};
        if (attack == 24 || attack == 25) {
            f.functions[1].operand_count = 6; f.operands[5] = 5;
            f.ops[11] = (XrXirInstruction) {attack == 24 ? XR_XIR_CALL : XR_XIR_FUNCTION_REF,
                attack == 24 ? XR_XIR_I64 : (XrXirType) 258, {5, 1}, {0}, 2, {0}};
        }
        if (attack == 26) f.operands[4] = 12;
        if (attack == 27) f.ops[8].args[1] = UINT32_MAX;
        if (attack == 28) f.ops[2].immediate = 1;
        if (attack == 29) f.init[0].args[0] = 1;
        XrXirStatus status = xr_xir_verify(&f.module, NULL, NULL);
        if (status == XR_XIR_OK) fprintf(stderr, "array metadata attack accepted: %u\n", attack);
        CHECK(status != XR_XIR_OK);
        XrXirArtifact *artifact = (XrXirArtifact *) (uintptr_t) 1;
        CHECK(xr_xir_check(&f.module, NULL, &artifact, NULL) != XR_XIR_OK && !artifact);
    }
    XirArrayMetadataFixture f; xir_array_metadata_init(&f);
    f.operands[2] = 6; /* A mutable slot is independently a writable receiver. */
    CHECK(xr_xir_verify(&f.module, NULL, NULL) == XR_XIR_OK);
    f.ops[8].args[0] = 2; /* Ordinary values remain readable snapshots. */
    CHECK(xr_xir_verify(&f.module, NULL, NULL) == XR_XIR_OK);
    f.slot.mutable = 0; f.operands[2] = 3;
    CHECK(xr_xir_verify(&f.module, NULL, NULL) == XR_XIR_OK);
}

static void array_phi_place_rejection(void) {
    XirArrayMetadataFixture f; xir_array_metadata_init(&f);
    XrXirBlock blocks[] = {{0, 5, 0, 0}, {5, 1, 0, 0}, {6, 1, 0, 0}, {7, 3, 0, 0}};
    f.ops[0] = (XrXirInstruction) {XR_XIR_CONST_BOOL, XR_XIR_BOOL, {0}, {0}, 1, {0}};
    f.ops[1] = (XrXirInstruction) {XR_XIR_ARRAY_NEW, (XrXirType) 256, {0}, {0}, 0, {0}};
    f.ops[2] = (XrXirInstruction) {XR_XIR_CELL_NEW, (XrXirType) 257, {1}, {0}, 0, {0}};
    f.ops[3] = (XrXirInstruction) {XR_XIR_CELL_PLACE, (XrXirType) 256, {2}, {0}, 0, {0}};
    f.ops[4] = (XrXirInstruction) {XR_XIR_BRANCH, XR_XIR_UNIT, {0}, {1, 2}, 0, {0}};
    f.ops[5] = (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {3}, 0, {0}};
    f.ops[6] = f.ops[5];
    f.ops[7] = (XrXirInstruction) {XR_XIR_PHI, (XrXirType) 256, {0, 4}, {0}, 0, {0}};
    f.ops[8] = (XrXirInstruction) {XR_XIR_ARRAY_LEN, XR_XIR_I64, {7}, {0}, 0, {0}};
    f.ops[9] = (XrXirInstruction) {XR_XIR_RETURN, XR_XIR_UNIT, {8}, {0}, 0, {0}};
    f.functions[1].blocks = blocks; f.functions[1].block_count = 4;
    f.functions[1].instruction_count = 10; f.functions[1].operand_count = 4;
    f.operands[0] = 1; f.operands[1] = 1; f.operands[2] = 2; f.operands[3] = 1;
    CHECK(xr_xir_verify(&f.module, NULL, NULL) == XR_XIR_OK);
    f.operands[1] = 3;
    CHECK(xr_xir_verify(&f.module, NULL, NULL) == XR_XIR_BAD_VALUE);
}

static void array_compact_layout(void) {
    const XrXirType types[] = {XR_XIR_BOOL, XR_XIR_I8, XR_XIR_U8, XR_XIR_I16, XR_XIR_U16,
        XR_XIR_I32, XR_XIR_U32, XR_XIR_I64, XR_XIR_U64, XR_XIR_F32, XR_XIR_F64,
        XR_XIR_STRING, XR_XIR_ATOMIC_I64, (XrXirType) 256, (XrXirType) 258};
    const uint32_t widths[] = {1, 1, 1, 2, 2, 4, 4, 8, 8, 4, 8, 8, 8, 8, 8};
    XirArrayMetadataFixture f; xir_array_metadata_init(&f);
    for (uint32_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        XrXirLayout layout = {0};
        CHECK(xr_xir_layout(&f.types, types[i], &fixture_target, XR_XIR_LAYOUT_STORAGE, &layout) == XR_XIR_OK);
        CHECK(layout.size == widths[i] && layout.alignment == widths[i]);
        CHECK(xr_xir_layout(&f.types, types[i], &fixture_target, XR_XIR_LAYOUT_BOXED, &layout) == XR_XIR_OK);
        CHECK(layout.size == 16 && layout.alignment == 8);
    }
}

static void array_metadata_cases(void) {
    array_stage_layout(); array_stage_attacks(); array_phi_place_rejection(); array_compact_layout();
}
#endif // XIR_ARRAY_STAGE_CASES_H
