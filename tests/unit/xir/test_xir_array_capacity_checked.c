/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_array_capacity_checked.c - Independent capacity operation graphs
 *
 * KEY CONCEPT:
 *   READ paths never turn into owned reserve inputs, and decoded shapes reverify.
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_operand_roles.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#include "xir_array_metadata_fixture.h"
_Static_assert(XR_XIR_ARRAY_CAPACITY == 149 && XR_XIR_ARRAY_WITH_CAPACITY == 150 &&
    XR_XIR_ARRAY_RESERVE == 151, "Capacity operation ordinals are fixed");
_Static_assert(XR_XIR_OP_COUNT == 153 && XR_XIR_CHECKED_SCHEMA == 28 &&
    XR_XIR_CHECKED_CONTRACT == 73, "Capacity operation framing is fixed");

static void capacity_checked_fixture(XirArrayMetadataFixture *f) {
    xir_array_metadata_init(f); XrXirType array = (XrXirType)256, cell = (XrXirType)257;
    f->nodes[2] = (XrXirTypeNode){.kind = XR_XIR_TYPE_ARRAY, .element = XR_XIR_STRING};
    f->ops[0] = (XrXirInstruction){XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 17, {0}};
    f->ops[1] = (XrXirInstruction){XR_XIR_ARRAY_WITH_CAPACITY, array, {0}, {0}, 0, {0}};
    f->ops[2] = (XrXirInstruction){XR_XIR_CELL_NEW, cell, {1}, {0}, 0, {0}};
    f->ops[3] = (XrXirInstruction){XR_XIR_CELL_PLACE, array, {2}, {0}, 0, {0}};
    f->ops[4] = (XrXirInstruction){XR_XIR_ARRAY_CAPACITY, XR_XIR_I64, {3}, {0}, 0, {0}};
    f->ops[5] = (XrXirInstruction){XR_XIR_PLACE_READ, array, {3}, {0}, 0, {0}};
    f->ops[6] = (XrXirInstruction){XR_XIR_ARRAY_RESERVE, array, {5, 0}, {0}, 0, {0}};
    f->ops[7] = (XrXirInstruction){XR_XIR_PLACE_WRITE, XR_XIR_UNIT, {3, 6}, {0}, 0, {0}};
    f->ops[8] = (XrXirInstruction){XR_XIR_ARRAY_CAPACITY, XR_XIR_I64, {6}, {0}, 0, {0}};
    f->ops[9] = (XrXirInstruction){XR_XIR_RETURN, XR_XIR_UNIT, {8}, {0}, 0, {0}};
    f->blocks[1].count = 10; f->functions[1].instruction_count = 10;
    f->functions[1].operands = NULL; f->functions[1].operand_count = 0;
}
static void capacity_checked_attacks(const XrXirCompileContext *context) {
    for (unsigned attack = 0; attack < 15; ++attack) {
        XirArrayMetadataFixture f; capacity_checked_fixture(&f);
        if (attack == 0) f.ops[4].type = XR_XIR_BOOL;
        if (attack == 1) f.ops[4].args[1] = 1;
        if (attack == 2) f.ops[4].targets[0] = 1;
        if (attack == 3) f.ops[4].immediate = 1;
        if (attack == 4) f.ops[4].type_arguments[1] = 1;
        if (attack == 5) f.ops[1].type = XR_XIR_STRING;
        if (attack == 6) f.ops[1].args[0] = 3;
        if (attack == 7) f.ops[1].args[1] = 1;
        if (attack == 8) f.ops[6].args[0] = 3;
        if (attack == 9) f.ops[6].args[1] = 5;
        if (attack == 10) f.ops[6].type = (XrXirType)258;
        if (attack == 11) f.ops[6].targets[1] = 1;
        if (attack == 12) f.ops[6].immediate = -1;
        if (attack == 13) f.ops[0].type = XR_XIR_I32;
        if (attack == 14) f.ops[7].args[1] = 3;
        CHECK(xir_fixture_verify(context, &f.module, NULL) != XR_XIR_OK);
        XrXirArtifact *out = NULL;
        CHECK(xir_fixture_check(context, &f.module, &out, NULL) != XR_XIR_OK && !out);
    }
}
int main(void) {
    SourceFixtureOwner owner = {0}; source_fixture_owner_new(&owner);
    CHECK(xr_xir_operand_role(XR_XIR_ARRAY_CAPACITY, 0) == XR_XIR_OPERAND_READ);
    CHECK(xr_xir_operand_role(XR_XIR_ARRAY_WITH_CAPACITY, 0) == XR_XIR_OPERAND_VALUE);
    CHECK(xr_xir_operand_role(XR_XIR_ARRAY_RESERVE, 0) == XR_XIR_OPERAND_VALUE);
    CHECK(xr_xir_operand_role(XR_XIR_ARRAY_RESERVE, 1) == XR_XIR_OPERAND_VALUE);
    XirArrayMetadataFixture f; capacity_checked_fixture(&f);
    CHECK(xir_fixture_verify(&owner.context, &f.module, NULL) == XR_XIR_OK);
    XrXirArtifact *checked = NULL, *read = NULL, *specialized = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0};
    CHECK(xir_fixture_check(&owner.context, &f.module, &checked, NULL) == XR_XIR_OK);
    memset(&f, 0xCC, sizeof(f));
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_compile_checked_read(&owner.context, packet.bytes, packet.length, &read, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xCC, packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_artifact_verify(read, NULL) == XR_XIR_OK);
    XrXirInstruction *ops = (XrXirInstruction *)xr_xir_compile_artifact_module(read)->functions[1].instructions;
    ops[6].args[0] = 3;
    CHECK(xr_xir_compile_artifact_verify(read, NULL) == XR_XIR_BAD_VALUE);
    ops[6].args[0] = 5;
    CHECK(xr_xir_compile_specialize(read, &specialized, NULL) == XR_XIR_OK);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(specialized, &target, &lowered, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_verify(lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(lowered); xr_xir_compile_artifact_free(specialized); xr_xir_compile_artifact_free(read);
    capacity_checked_attacks(&owner.context); source_fixture_owner_free(&owner);
    puts("Array capacity independent Built/Checked/replay/specialization/Lowered shape and place attacks PASS");
    return 0;
}
