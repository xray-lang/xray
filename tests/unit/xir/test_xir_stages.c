/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_stages.c - Adversarial typed control flow and metadata lifetime tests
 *
 * KEY CONCEPT:
 *   Hand-authored graphs exercise admission independently of any IR producer.
 */

#include "xir/xxir.h"
#include "xir/xxir_types.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_constraint_proof.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const XrXirTarget fixture_target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

typedef struct Fixture {
    char name[5];
    XrXirType parameters[2];
    XrXirBlock blocks[3];
    XrXirInstruction instructions[6];
    XrXirFunction function;
    XrXirModule module;
} Fixture;

static void fixture_init(Fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    memcpy(fixture->name, "entry", 5);
    fixture->parameters[0] = XR_XIR_BOOL;
    fixture->parameters[1] = XR_XIR_I64;
    for (uint32_t b = 0; b < 3; ++b)
        fixture->blocks[b] = (XrXirBlock) {b * 2, 2, 0, 0};
    fixture->instructions[0] = (XrXirInstruction) {XR_XIR_COPY, XR_XIR_I64, {1, 0}, {0, 0}, 0, {0}};
    fixture->instructions[1] = (XrXirInstruction) {XR_XIR_BRANCH, XR_XIR_UNIT, {0, 0}, {1, 2}, 0, {0}};
    fixture->instructions[2] = (XrXirInstruction) {XR_XIR_ADD_INT, XR_XIR_I64, {2, 1}, {0, 0}, 0, {0}};
    fixture->instructions[3] = (XrXirInstruction) {XR_XIR_RETURN, XR_XIR_UNIT, {4, 0}, {0, 0}, 0, {0}};
    fixture->instructions[4] = (XrXirInstruction) {XR_XIR_CONST_INT, XR_XIR_I64, {0, 0}, {0, 0}, 9, {0}};
    fixture->instructions[5] = (XrXirInstruction) {XR_XIR_RETURN, XR_XIR_UNIT, {6, 0}, {0, 0}, 0, {0}};
    fixture->function = (XrXirFunction) {fixture->name, 5, fixture->parameters, 2,
        XR_XIR_I64, fixture->blocks, 3, fixture->instructions, 6, NULL, 0};
    fixture->module = (XrXirModule) {XR_XIR_BUILT, &fixture->function, 1, NULL, NULL, NULL, NULL, XR_XIR_PROGRAM, NULL};
}

static void expect(Fixture *fixture, XrXirStatus status) {
    XrXirDiagnostic diagnostic;
    CHECK(xr_xir_verify(&fixture->module, NULL, &diagnostic) == status);
    CHECK(diagnostic.status == status);
}

static void cumulative_verification_budget(void) {
    Fixture fixture;
    fixture_init(&fixture);
    XrXirBudget initial = xr_xir_default_budget(), remaining = initial;
    XrXirDiagnostic diagnostic;
    CHECK(xr_xir_verify_remaining(&fixture.module, &remaining, &diagnostic) == XR_XIR_OK);
    CHECK(remaining.functions == initial.functions - 1);
    CHECK(remaining.parameters == initial.parameters - 2);
    CHECK(remaining.blocks == initial.blocks - 3);
    CHECK(remaining.instructions == initial.instructions - 6);
    uint64_t bytes = initial.metadata_bytes - remaining.metadata_bytes;
    uint64_t work = initial.work - remaining.work;
    CHECK(bytes && work);
    CHECK(remaining.scratch_bytes == initial.scratch_bytes);
    CHECK(remaining.frame_bytes == initial.frame_bytes);
    for (unsigned mode = 0; mode < 4; ++mode) {
        remaining = initial;
        remaining.functions = 2;
        remaining.metadata_bytes = bytes * 2;
        remaining.work = work * 2;
        if (mode == 1) --remaining.functions;
        if (mode == 2) --remaining.metadata_bytes;
        if (mode == 3) --remaining.work;
        CHECK(xr_xir_verify_remaining(&fixture.module, &remaining, NULL) == XR_XIR_OK);
        CHECK(xr_xir_verify_remaining(&fixture.module, &remaining, &diagnostic) ==
              (mode ? XR_XIR_BUDGET : XR_XIR_OK));
        CHECK(diagnostic.status == (mode ? XR_XIR_BUDGET : XR_XIR_OK));
        if (!mode) CHECK(!remaining.functions && !remaining.metadata_bytes && !remaining.work);
    }
    remaining = initial;
    CHECK(xr_xir_verify(&fixture.module, &remaining, NULL) == XR_XIR_OK);
    CHECK(memcmp(&remaining, &initial, sizeof(initial)) == 0);
    CHECK(xr_xir_verify_remaining(&fixture.module, NULL, &diagnostic) == XR_XIR_BAD_STRUCTURE);
    CHECK(diagnostic.status == XR_XIR_BAD_STRUCTURE);
}

static void transitions_and_lifetime(void) {
    Fixture fixture;
    fixture_init(&fixture);
    XrXirArtifact *checked = NULL, *lowered = NULL;
    CHECK(xr_xir_check(&fixture.module, NULL, &checked, NULL) == XR_XIR_OK);
    CHECK(checked != NULL);
    const XrXirModule *module = xr_xir_artifact_module(checked);
    CHECK(module->stage == XR_XIR_CHECKED);
    CHECK(module->functions != &fixture.function);
    CHECK(module->functions[0].name != fixture.name);
    CHECK(module->functions[0].parameters != fixture.parameters);
    CHECK(module->functions[0].blocks != fixture.blocks);
    CHECK(module->functions[0].instructions != fixture.instructions);
    memset(&fixture, 0xa5, sizeof(fixture));
    CHECK(xr_xir_verify(module, NULL, NULL) == XR_XIR_OK);
    CHECK(memcmp(module->functions[0].name, "entry", 5) == 0);
    CHECK(xr_xir_lower(checked, &fixture_target, NULL, &lowered, NULL) == XR_XIR_OK);
    CHECK(module->functions[0].instructions[0].op == XR_XIR_COPY);
    xr_xir_artifact_free(checked);
    module = xr_xir_artifact_module(lowered);
    CHECK(module->stage == XR_XIR_LOWERED);
    CHECK(module->functions[0].instructions[0].op == XR_XIR_SCALAR_COPY);
    CHECK(module->functions[0].instructions[4].immediate == 9);
    CHECK(xr_xir_verify(module, NULL, NULL) == XR_XIR_OK);
    XrXirArtifact *rejected = lowered;
    CHECK(xr_xir_lower(lowered, &fixture_target, NULL, &rejected, NULL) == XR_XIR_BAD_STAGE);
    CHECK(rejected == NULL);
    CHECK(xr_xir_check(module, NULL, &rejected, NULL) == XR_XIR_BAD_STAGE);
    CHECK(rejected == NULL);
    xr_xir_artifact_free(lowered);
    xr_xir_artifact_free(NULL);
}

static void malformed_inputs(void) {
    Fixture f;
    fixture_init(&f);
    expect(&f, XR_XIR_OK);
    f.module.stage = (XrXirStage) 3;
    expect(&f, XR_XIR_BAD_STAGE);
    f.module.stage = XR_XIR_LOWERED;
    expect(&f, XR_XIR_BAD_STAGE);
    f.module.stage = XR_XIR_BUILT;
    f.instructions[0].op = XR_XIR_SCALAR_COPY;
    expect(&f, XR_XIR_BAD_STAGE);
    f.instructions[0].op = (XrXirOp) -1;
    expect(&f, XR_XIR_BAD_STRUCTURE);
    f.instructions[0].op = XR_XIR_OP_COUNT;
    expect(&f, XR_XIR_BAD_STRUCTURE);
    fixture_init(&f);
    f.instructions[1].args[0] = 1;
    expect(&f, XR_XIR_BAD_TYPE);
    fixture_init(&f);
    f.instructions[2].args[0] = 6;
    expect(&f, XR_XIR_BAD_DOMINANCE);
    f.instructions[2].args[0] = 4;
    expect(&f, XR_XIR_BAD_DOMINANCE);
    f.instructions[2].args[0] = 3;
    expect(&f, XR_XIR_BAD_VALUE);
    f.instructions[2].args[0] = UINT32_MAX;
    expect(&f, XR_XIR_BAD_VALUE);
    fixture_init(&f);
    f.instructions[1].targets[0] = 0;
    expect(&f, XR_XIR_BAD_STRUCTURE);
    f.instructions[1].targets[0] = 3;
    expect(&f, XR_XIR_BAD_STRUCTURE);
    f.instructions[1].targets[0] = 2;
    expect(&f, XR_XIR_BAD_STRUCTURE);
    fixture_init(&f);
    f.blocks[1].first = 1;
    expect(&f, XR_XIR_BAD_STRUCTURE);
    fixture_init(&f);
    f.blocks[2].count = UINT32_MAX;
    expect(&f, XR_XIR_BAD_STRUCTURE);
    fixture_init(&f);
    f.instructions[3].op = XR_XIR_COPY;
    f.instructions[3].type = XR_XIR_I64;
    expect(&f, XR_XIR_BAD_STRUCTURE);
    fixture_init(&f);
    f.instructions[0] = (XrXirInstruction) {XR_XIR_RETURN, XR_XIR_UNIT, {1, 0}, {0, 0}, 0, {0}};
    expect(&f, XR_XIR_BAD_STRUCTURE);
    fixture_init(&f);
    f.function.result = XR_XIR_BOOL;
    expect(&f, XR_XIR_BAD_TYPE);
    fixture_init(&f);
    f.instructions[0].type = (XrXirType) -1;
    expect(&f, XR_XIR_BAD_TYPE);
    fixture_init(&f);
    f.instructions[0].args[1] = 1;
    expect(&f, XR_XIR_BAD_STRUCTURE);
    fixture_init(&f);
    f.instructions[0].targets[0] = 1;
    expect(&f, XR_XIR_BAD_STRUCTURE);
    fixture_init(&f);
    f.instructions[0].immediate = 1;
    expect(&f, XR_XIR_BAD_STRUCTURE);
    fixture_init(&f);
    f.name[1] = 0;
    expect(&f, XR_XIR_BAD_STRUCTURE);
    fixture_init(&f);
    f.function.parameters = NULL;
    expect(&f, XR_XIR_BAD_STRUCTURE);
    fixture_init(&f);
    XrXirDiagnostic diagnostic;
    f.instructions[5].args[0] = 4;
    CHECK(xr_xir_verify(&f.module, NULL, &diagnostic) == XR_XIR_BAD_DOMINANCE);
    CHECK(diagnostic.function == 0 && diagnostic.block == 2 && diagnostic.instruction == 5);
    XrXirArtifact *output = (XrXirArtifact *) &f;
    CHECK(xr_xir_check(&f.module, NULL, &output, NULL) == XR_XIR_BAD_DOMINANCE);
    CHECK(output == NULL);
    CHECK(xr_xir_verify(NULL, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(xr_xir_check(NULL, NULL, &output, NULL) == XR_XIR_BAD_STAGE);
    CHECK(xr_xir_check(&f.module, NULL, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
}

static void budgets(void) {
    Fixture f;
    fixture_init(&f);
    XrXirBudget limits = xr_xir_default_budget();
    limits.work = 1;
    CHECK(xr_xir_verify(&f.module, &limits, NULL) == XR_XIR_BUDGET);
    limits = xr_xir_default_budget();
    limits.scratch_bytes = 1;
    CHECK(xr_xir_verify(&f.module, &limits, NULL) == XR_XIR_BUDGET);
    limits = xr_xir_default_budget();
    limits.metadata_bytes = 1;
    CHECK(xr_xir_verify(&f.module, &limits, NULL) == XR_XIR_BUDGET);
    limits = xr_xir_default_budget();
    limits.functions = 0;
    CHECK(xr_xir_verify(&f.module, &limits, NULL) == XR_XIR_BUDGET);
    limits = xr_xir_default_budget();
    limits.parameters = 1;
    CHECK(xr_xir_verify(&f.module, &limits, NULL) == XR_XIR_BUDGET);
    limits = xr_xir_default_budget();
    limits.blocks = 2;
    CHECK(xr_xir_verify(&f.module, &limits, NULL) == XR_XIR_BUDGET);
    limits = xr_xir_default_budget();
    limits.instructions = 5;
    CHECK(xr_xir_verify(&f.module, &limits, NULL) == XR_XIR_BUDGET);
    XrXirFunction functions[2] = {f.function, f.function};
    f.module.functions = functions;
    f.module.function_count = 2;
    limits.instructions = 6;
    CHECK(xr_xir_verify(&f.module, &limits, NULL) == XR_XIR_BUDGET);
    f.module.function_count = UINT32_MAX;
    CHECK(xr_xir_verify(&f.module, NULL, NULL) == XR_XIR_BUDGET);
    fixture_init(&f);
    f.function.instruction_count = UINT32_MAX;
    CHECK(xr_xir_verify(&f.module, NULL, NULL) == XR_XIR_BUDGET);
}

static void loops_and_storage_order(void) {
    XrXirInstruction ops[] = {
        {XR_XIR_CONST_BOOL, XR_XIR_BOOL, {0, 0}, {0, 0}, 1, {0}},
        {XR_XIR_JUMP, XR_XIR_UNIT, {0, 0}, {2, 0}, 0, {0}},
        {XR_XIR_JUMP, XR_XIR_UNIT, {0, 0}, {2, 0}, 0, {0}},
        {XR_XIR_BRANCH, XR_XIR_UNIT, {0, 0}, {1, 3}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0, 0}, {0, 0}, 0, {0}},
    };
    XrXirBlock blocks[] = {{0, 2, 0, 0}, {2, 1, 0, 0}, {3, 1, 0, 0}, {4, 1, 0, 0}};
    XrXirFunction function = {"loop", 4, NULL, 0, XR_XIR_UNIT, blocks, 4, ops, 5, NULL, 0};
    XrXirModule module = {XR_XIR_BUILT, &function, 1, NULL, NULL, NULL, NULL, XR_XIR_PROGRAM, NULL};
    CHECK(xr_xir_verify(&module, NULL, NULL) == XR_XIR_OK);
    ops[0].immediate = 2;
    CHECK(xr_xir_verify(&module, NULL, NULL) == XR_XIR_BAD_TYPE);
    ops[0].immediate = 1;
    ops[3].args[0] = 3;
    CHECK(xr_xir_verify(&module, NULL, NULL) == XR_XIR_BAD_VALUE);
    XrXirBudget limits = xr_xir_default_budget();
    ops[3].args[0] = 0;
    limits.work = 32;
    CHECK(xr_xir_verify(&module, &limits, NULL) == XR_XIR_BUDGET);
}

static void dominance_word_boundary(void) {
    XrXirBlock blocks[70];
    XrXirInstruction ops[71] = {0};
    uint32_t next = 0, definition = 0;
    for (uint32_t b = 0; b < 70; ++b) {
        blocks[b] = (XrXirBlock) {next, b == 65 ? 2 : 1, 0, 0};
        if (b == 65) {
            definition = next;
            ops[next++] = (XrXirInstruction) {XR_XIR_CONST_INT, XR_XIR_I64, {0, 0}, {0, 0}, 42, {0}};
        }
        if (b == 69)
            ops[next++] = (XrXirInstruction) {XR_XIR_RETURN, XR_XIR_UNIT, {definition, 0}, {0, 0}, 0, {0}};
        else
            ops[next++] = (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0, 0}, {b + 1, 0}, 0, {0}};
    }
    XrXirFunction function = {"wide", 4, NULL, 0, XR_XIR_I64, blocks, 70, ops, 71, NULL, 0};
    XrXirModule module = {XR_XIR_BUILT, &function, 1, NULL, NULL, NULL, NULL, XR_XIR_PROGRAM, NULL};
    CHECK(xr_xir_verify(&module, NULL, NULL) == XR_XIR_OK);
    ops[64] = (XrXirInstruction) {XR_XIR_BRANCH, XR_XIR_UNIT, {0, 0}, {65, 69}, 0, {0}};
    XrXirType boolean = XR_XIR_BOOL;
    function.parameters = &boolean;
    function.parameter_count = 1;
    ops[70].args[0] = definition + 1;
    CHECK(xr_xir_verify(&module, NULL, NULL) == XR_XIR_BAD_DOMINANCE);
}

static void reverse_storage_and_boolean_values(void) {
    XrXirInstruction ops[] = {
        {XR_XIR_JUMP, XR_XIR_UNIT, {0, 0}, {2, 0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {4, 0}, {0, 0}, 0, {0}},
        {XR_XIR_CONST_INT, XR_XIR_I64, {0, 0}, {0, 0}, 7, {0}},
        {XR_XIR_EQ_INT, XR_XIR_BOOL, {2, 2}, {0, 0}, 0, {0}},
        {XR_XIR_COPY, XR_XIR_BOOL, {3, 0}, {0, 0}, 0, {0}},
        {XR_XIR_JUMP, XR_XIR_UNIT, {0, 0}, {1, 0}, 0, {0}},
    };
    XrXirBlock blocks[] = {{0, 1, 0, 0}, {1, 1, 0, 0}, {2, 4, 0, 0}};
    XrXirFunction function = {"reverse", 7, NULL, 0, XR_XIR_BOOL, blocks, 3, ops, 6, NULL, 0};
    XrXirModule module = {XR_XIR_BUILT, &function, 1, NULL, NULL, NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrXirArtifact *checked = NULL, *lowered = NULL;
    CHECK(xr_xir_check(&module, NULL, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_lower(checked, &fixture_target, NULL, &lowered, NULL) == XR_XIR_OK);
    CHECK(xr_xir_artifact_module(lowered)->functions[0].instructions[4].op == XR_XIR_SCALAR_COPY);
    xr_xir_artifact_free(checked);
    xr_xir_artifact_free(lowered);
    ops[3].args[1] = 4;
    CHECK(xr_xir_verify(&module, NULL, NULL) == XR_XIR_BAD_TYPE);
}

static void numeric_admission(void) {
    for (XrXirOp op = XR_XIR_SUB_INT; op <= XR_XIR_SHR_INT; op = (XrXirOp) (op + 1)) {
        XrXirType parameters[] = {XR_XIR_I64, XR_XIR_I64};
        XrXirType result = op >= XR_XIR_NE_INT && op <= XR_XIR_GE_INT ? XR_XIR_BOOL : XR_XIR_I64;
        XrXirInstruction ops[] = {{op, result, {0, 1}, {0}, 0, {0}}, {XR_XIR_RETURN, XR_XIR_UNIT, {2}, {0}, 0, {0}}};
        const XrXirBlock block = {0, 2, 0, 0};
        XrXirFunction function = {"number", 6, parameters, 2, result, &block, 1, ops, 2, NULL, 0};
        XrXirModule module = {XR_XIR_BUILT, &function, 1, NULL, NULL, NULL, NULL, XR_XIR_PROGRAM, NULL};
        CHECK(xr_xir_verify(&module, NULL, NULL) == XR_XIR_OK);
        parameters[1] = XR_XIR_BOOL;
        CHECK(xr_xir_verify(&module, NULL, NULL) == XR_XIR_BAD_TYPE);
        parameters[1] = XR_XIR_I64; ops[0].type = XR_XIR_STRING;
        CHECK(xr_xir_verify(&module, NULL, NULL) == XR_XIR_BAD_TYPE);
        ops[0].type = result; ops[0].immediate = 1;
        CHECK(xr_xir_verify(&module, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
        ops[0].immediate = 0; ops[0].args[1] = 2;
        CHECK(xr_xir_verify(&module, NULL, NULL) != XR_XIR_OK);
    }
}

static void constructed_metadata(void) {
    XrXirCallableParameter input = {(XrXirType)256,0};
    XrXirTypeNode nodes[] = {
        {XR_XIR_TYPE_ARRAY,XR_XIR_STRING,NULL,0,XR_XIR_UNIT,0,0, {0}},
        {XR_XIR_TYPE_CELL,(XrXirType)256,NULL,0,XR_XIR_UNIT,0,0, {0}},
        {XR_XIR_TYPE_CALLABLE,XR_XIR_UNIT,&input,1,(XrXirType)256,0,0, {0}},
        {XR_XIR_TYPE_ARRAY,(XrXirType)258,NULL,0,XR_XIR_UNIT,0,0, {0}},
        {XR_XIR_TYPE_ARRAY,(XrXirType)(XR_XIR_TYPE_PARAMETER_LIMIT-1),NULL,0,XR_XIR_UNIT,0,65536, {0}}
    };
    XrXirTypes types = {nodes,5, NULL, NULL};
    XrXirBudget budget = xr_xir_default_budget();
    CHECK(xr_xir_types_structure_verify(&types,&budget) == XR_XIR_OK);
    CHECK(xr_xir_type_is_array(&types,(XrXirType)256));
    CHECK(!xr_xir_type_is_callable(&types,(XrXirType)256));
    CHECK(xr_xir_type_is_cell(&types,(XrXirType)257));
    CHECK(xr_xir_type_is_callable(&types,(XrXirType)258));
    CHECK(!xr_xir_type_is_owned(NULL,(XrXirType)256));
    CHECK(!xr_xir_type_node(&types,(XrXirType)261));
    CHECK(!xr_xir_type_node(&types,(XrXirType)XR_XIR_CONSTRUCTED_TYPE_LIMIT));
    XrXirModule marker_module = {0}; marker_module.stage = XR_XIR_BUILT; marker_module.types = &types;
    XrXirProofContext marker_context = {&marker_module,{XR_XIR_CONTEXT_CLOSED,0,0}};
    CHECK(xr_xir_type_markers_prove(&marker_context,(XrXirType)256,XR_XIR_CONSTRAINT_SENDABLE,&budget) == XR_XIR_OK);
    CHECK(xr_xir_type_markers_prove(&marker_context,(XrXirType)259,XR_XIR_CONSTRAINT_SENDABLE,&budget) == XR_XIR_BAD_TYPE);
    budget.work = 0;
    CHECK(xr_xir_type_markers_prove(&marker_context,(XrXirType)256,XR_XIR_CONSTRAINT_SENDABLE,&budget) == XR_XIR_BUDGET);
    XrXirLayout layout;
    CHECK(xr_xir_layout(&types,(XrXirType)256,&fixture_target,XR_XIR_LAYOUT_STORAGE,&layout) == XR_XIR_OK);
    CHECK(layout.size == 8 && layout.alignment == 8);
    CHECK(xr_xir_layout(NULL,(XrXirType)256,&fixture_target,XR_XIR_LAYOUT_STORAGE,&layout) == XR_XIR_BAD_LAYOUT);
    CHECK(!layout.size && !layout.alignment);
    CHECK(xr_xir_layout(&types,(XrXirType)260,&fixture_target,XR_XIR_LAYOUT_STORAGE,&layout) == XR_XIR_BAD_LAYOUT);
    for (unsigned attack = 0; attack < 18; ++attack) {
        XrXirTypeNode saved[5]; memcpy(saved,nodes,sizeof(saved));
        if (attack == 0) nodes[0].kind = 0;
        if (attack == 1) nodes[0].kind = 4;
        if (attack == 2) nodes[0].element = (XrXirType)256;
        if (attack == 3) nodes[0].element = (XrXirType)258;
        if (attack == 4) nodes[0].element = (XrXirType)(XR_XIR_PANIC_INFO + 1);
        if (attack == 5) nodes[0].element = (XrXirType)255;
        if (attack == 6) nodes[0].element = (XrXirType)XR_XIR_TYPE_PARAMETER_LIMIT;
        if (attack == 7) nodes[0].element = (XrXirType)0x40000003u;
        if (attack == 8) nodes[0].element = XR_XIR_UNIT;
        if (attack == 9) nodes[0].flags = 1;
        if (attack == 10) nodes[0].parameters = &input;
        if (attack == 11) nodes[0].result = XR_XIR_I64;
        if (attack == 12) nodes[0].parameter_span = 1;
        if (attack == 13) nodes[4].parameter_span = 65535;
        if (attack == 14) nodes[3] = nodes[0];
        if (attack == 15) nodes[3].element = (XrXirType)257;
        if (attack == 16) { nodes[3].kind = XR_XIR_TYPE_CELL; nodes[3].element = (XrXirType)257; }
        if (attack == 17) input.type = (XrXirType)257;
        budget = xr_xir_default_budget();
        CHECK(xr_xir_types_structure_verify(&types,&budget) != XR_XIR_OK);
        memcpy(nodes,saved,sizeof(nodes)); input.type = (XrXirType)256;
    }
    XrXirGeneric generic = {NULL,65536,NULL,0};
    XrXirModule context = {XR_XIR_BUILT,NULL,1,NULL,&generic,&types, NULL, XR_XIR_PROGRAM, NULL};
    CHECK(xr_xir_type_in_context(&context,0,(XrXirType)260));
    CHECK(xr_xir_type_in_context(&context,0,(XrXirType)(XR_XIR_TYPE_PARAMETER_LIMIT-1)));
    CHECK(!xr_xir_type_in_context(&context,0,(XrXirType)XR_XIR_TYPE_PARAMETER_LIMIT));
    generic.parameter_count = 65535;
    CHECK(!xr_xir_type_in_context(&context,0,(XrXirType)260));
    budget = xr_xir_default_budget();
    CHECK(xr_xir_type_satisfies(&context,0,(XrXirType)257,(XrXirConstraint){0},&budget) == XR_XIR_BAD_TYPE);
    budget = xr_xir_default_budget(); budget.metadata_bytes = 1;
    CHECK(xr_xir_types_structure_verify(&types,&budget) == XR_XIR_BUDGET);
    budget = xr_xir_default_budget(); budget.work = types.count;
    CHECK(xr_xir_types_structure_verify(&types,&budget) == XR_XIR_BUDGET);
    XrXirTypes *copy = NULL;
    CHECK(xr_xir_types_clone(&types,&copy) == XR_XIR_OK && copy && copy->nodes != nodes);
    CHECK(copy->nodes[2].parameters != &input);
    memset(nodes,0xCC,sizeof(nodes)); memset(&input,0xCC,sizeof(input));
    budget = xr_xir_default_budget();
    CHECK(xr_xir_types_structure_verify(copy,&budget) == XR_XIR_OK);
    xr_xir_types_free(copy);
}

#include "xir_array_stage_cases.h"

#include "xir_nominal_fixture.h"
static void nominal_metadata_cases(void) {
    NominalFixture f; nominal_fixture(&f);
    XrXirBudget b = xr_xir_default_budget();
    CHECK(xr_xir_nominal_structure_verify(&f.table, NULL, &b) == XR_XIR_OK);
    f.declarations[1].module = f.declarations[0].module;
    b = xr_xir_default_budget();
    CHECK(xr_xir_nominal_structure_verify(&f.table, NULL, &b) == XR_XIR_BAD_STRUCTURE);
    nominal_fixture(&f); f.fields[1].name = f.fields[0].name;
    b = xr_xir_default_budget();
    CHECK(xr_xir_nominal_structure_verify(&f.table, NULL, &b) == XR_XIR_BAD_STRUCTURE);
    nominal_fixture(&f); f.declarations[0].parameter_count = 0; f.declarations[0].constraints = NULL;
    b = xr_xir_default_budget();
    CHECK(xr_xir_nominal_structure_verify(&f.table, NULL, &b) == XR_XIR_BAD_TYPE);
    nominal_fixture(&f); f.constraint.markers = 4;
    b = xr_xir_default_budget();
    CHECK(xr_xir_nominal_structure_verify(&f.table, NULL, &b) == XR_XIR_BAD_TYPE);
    nominal_fixture(&f); f.fields[0].flags = XR_XIR_FIELD_PRIVATE | XR_XIR_FIELD_PROTECTED;
    b = xr_xir_default_budget();
    CHECK(xr_xir_nominal_structure_verify(&f.table, NULL, &b) == XR_XIR_BAD_STRUCTURE);
    nominal_fixture(&f); f.module[2] = 0;
    b = xr_xir_default_budget();
    CHECK(xr_xir_nominal_structure_verify(&f.table, NULL, &b) == XR_XIR_BAD_STRUCTURE);
    nominal_fixture(&f); f.fields[0].type = XR_XIR_UNIT;
    b = xr_xir_default_budget();
    CHECK(xr_xir_nominal_structure_verify(&f.table, NULL, &b) == XR_XIR_BAD_TYPE);
    nominal_fixture(&f);
    XrXirTypeNode node = {XR_XIR_TYPE_ARRAY, (XrXirType) (XR_XIR_TYPE_PARAMETER_BASE + 1), NULL, 0, XR_XIR_UNIT, 0, 2, {0}};
    XrXirTypes types = {&node, 1, NULL, NULL}; f.fields[0].type = (XrXirType) XR_XIR_CONSTRUCTED_TYPE_BASE;
    b = xr_xir_default_budget();
    CHECK(xr_xir_nominal_structure_verify(&f.table, &types, &b) == XR_XIR_BAD_TYPE);
    node.element = (XrXirType) XR_XIR_TYPE_PARAMETER_BASE; node.parameter_span = 1;
    b = xr_xir_default_budget();
    CHECK(xr_xir_nominal_structure_verify(&f.table, &types, &b) == XR_XIR_OK);
    node.kind = XR_XIR_TYPE_CELL;
    b = xr_xir_default_budget();
    CHECK(xr_xir_nominal_structure_verify(&f.table, &types, &b) == XR_XIR_BAD_TYPE);
    nominal_fixture(&f); b = xr_xir_default_budget(); b.work = 1;
    CHECK(xr_xir_nominal_structure_verify(&f.table, NULL, &b) == XR_XIR_BUDGET && b.work == 1);
    b = xr_xir_default_budget(); b.metadata_bytes = 1;
    CHECK(xr_xir_nominal_structure_verify(&f.table, NULL, &b) == XR_XIR_BUDGET && b.metadata_bytes == 1);
    b = xr_xir_default_budget();
    XrXirNominalTable *copy = NULL;
    CHECK(xr_xir_nominal_clone(&f.table, NULL, &b, &copy) == XR_XIR_OK && copy);
    memset(&f, 0xCC, sizeof(f));
    b = xr_xir_default_budget();
    CHECK(xr_xir_nominal_structure_verify(copy, NULL, &b) == XR_XIR_OK);
    CHECK(copy->count == 2 && memcmp(copy->declarations[0].module.bytes, "alpha", 5) == 0);
    CHECK(copy->declarations[0].constraints[0].markers == XR_XIR_CONSTRAINT_SENDABLE);
    CHECK(memcmp(copy->declarations[0].fields[0].name.bytes, "value", 5) == 0);
    xr_xir_nominal_free(copy);
}

static XrXirStatus nominal_context_use(const XrXirTypes *types, XrXirType type,
    const XrXirConstraint *constraints, uint32_t count, XrXirBudget *budget) {
    XrXirInstruction op = {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    XrXirBlock block = {0,1,0,0};
    XrXirFunction functions[] = {
        {"alpha_init",10,NULL,0,XR_XIR_UNIT,&block,1,&op,1,NULL,0},
        {"other_init",10,NULL,0,XR_XIR_UNIT,&block,1,&op,1,NULL,0},
        {"use",3,NULL,0,XR_XIR_UNIT,&block,1,&op,1,NULL,0}};
    uint32_t dependency = 1;
    XrXirSourceModule modules[] = {{"alpha",5,&dependency,1,0},{"other",5,NULL,0,1}};
    XrXirFunctionIdentity identities[] = {{0},{.module=1},{0}};
    XrXirDeclarations declarations = {modules,2,identities,NULL,0,NULL,0,0,0,NULL};
    XrXirGeneric generics[] = {{0},{0},{constraints,count,NULL,0}};
    XrXirModule module = {XR_XIR_BUILT,functions,3,&declarations,generics,types,NULL, XR_XIR_PROGRAM, NULL};
    XrXirProofContext context = {&module,{XR_XIR_CONTEXT_FUNCTION,2,0}};
    XrXirStatus status = xr_xir_types_structure_verify(types,budget);
    return status == XR_XIR_OK ? xr_xir_type_use_verify(&context,type,budget) : status;
}
static XrXirStatus nominal_catalog_semantics(const XrXirTypes *types, XrXirBudget *budget) {
    XrXirInstruction op = {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    XrXirBlock block = {0,1,0,0};
    XrXirFunction functions[] = {
        {"alpha_init",10,NULL,0,XR_XIR_UNIT,&block,1,&op,1,NULL,0},
        {"other_init",10,NULL,0,XR_XIR_UNIT,&block,1,&op,1,NULL,0}};
    uint32_t dependency = 1;
    XrXirSourceModule modules[] = {{"alpha",5,&dependency,1,0},{"other",5,NULL,0,1}};
    XrXirFunctionIdentity identities[] = {{0},{.module=1}};
    XrXirDeclarations declarations = {modules,2,identities,NULL,0,NULL,0,0,0,NULL};
    XrXirModule module = {XR_XIR_BUILT,functions,2,&declarations,NULL,types,NULL, XR_XIR_PROGRAM, NULL};
    return xr_xir_verify(&module,budget,NULL);
}
static void nominal_pool_ownership(void) {
    NominalFixture f; nominal_fixture(&f);
    XrXirTypes types = {NULL, 0, &f.table, NULL};
    XrXirBudget budget = xr_xir_default_budget();
    CHECK(xr_xir_types_structure_verify(&types, &budget) == XR_XIR_OK);
    XrXirTypes *copy = NULL;
    CHECK(xr_xir_types_clone(&types, &copy) == XR_XIR_OK && copy && copy->nominals);
    CHECK(copy->nominals != &f.table && copy->nominals->declarations != f.declarations);
    memset(&f, 0xCC, sizeof(f));
    budget = xr_xir_default_budget();
    CHECK(xr_xir_types_structure_verify(copy, &budget) == XR_XIR_OK);
    CHECK(memcmp(copy->nominals->declarations[0].name.bytes, "Pair", 4) == 0);
    Fixture source; fixture_init(&source); source.module.types = copy;
    XrXirArtifact *artifact = NULL;
    CHECK(xr_xir_check(&source.module, NULL, &artifact, NULL) == XR_XIR_BAD_STRUCTURE && !artifact);
    xr_xir_types_free(copy);
    types.nominals = NULL;
    budget = xr_xir_default_budget();
    CHECK(xr_xir_types_structure_verify(&types, &budget) == XR_XIR_BAD_STRUCTURE);
}

static void nominal_instance_metadata(void) {
    NominalFixture f; nominal_fixture(&f);
    XrXirType argument = (XrXirType) XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirTypeNode nodes[] = {{XR_XIR_TYPE_ARRAY, XR_XIR_STRING, NULL, 0, XR_XIR_UNIT, 0, 0, {0}},
        {XR_XIR_TYPE_NOMINAL, XR_XIR_UNIT, NULL, 0, XR_XIR_UNIT, 0, 0, {0, &argument, 1, NULL, 0}},
        {XR_XIR_TYPE_NOMINAL, XR_XIR_UNIT, NULL, 0, XR_XIR_UNIT, 0, 0, {1, &argument, 1, NULL, 0}}};
    XrXirTypes types = {nodes, 3, &f.table, NULL};
    XrXirBudget budget = xr_xir_default_budget();
    CHECK(xr_xir_types_structure_verify(&types, &budget) == XR_XIR_OK);
    for (unsigned mode = 0; mode < 11; ++mode) {
        XrXirTypeNode saved = nodes[2]; XrXirType saved_argument = argument;
        if (mode == 0) nodes[2].nominal.declaration = 2;
        if (mode == 1) nodes[2].nominal.declaration = 0;
        if (mode == 2) nodes[2].nominal.argument_count = 0;
        if (mode == 3) nodes[2].nominal.arguments = NULL;
        if (mode == 4) nodes[2].element = XR_XIR_I64;
        if (mode == 5) nodes[2].parameter_span = 1;
        if (mode == 6) argument = XR_XIR_UNIT;
        if (mode == 7) argument = (XrXirType) XR_XIR_TYPE_PARAMETER_BASE;
        if (mode == 8) argument = (XrXirType) (XR_XIR_CONSTRUCTED_TYPE_BASE + 2);
        if (mode == 9) nodes[0].kind = XR_XIR_TYPE_CELL;
        if (mode == 10) { nodes[0].kind = XR_XIR_TYPE_CALLABLE; nodes[0].element = XR_XIR_UNIT; nodes[0].result = XR_XIR_STRING; }
        budget = xr_xir_default_budget();
        CHECK(nominal_catalog_semantics(&types, &budget) != XR_XIR_OK);
        nodes[2] = saved; argument = saved_argument;
        nodes[0] = (XrXirTypeNode) {XR_XIR_TYPE_ARRAY, XR_XIR_STRING, NULL, 0, XR_XIR_UNIT, 0, 0, {0}};
    }
    nodes[0].nominal.declaration = 1; budget = xr_xir_default_budget();
    CHECK(xr_xir_types_structure_verify(&types, &budget) == XR_XIR_BAD_STRUCTURE);
    nodes[0].nominal.declaration = 0;
    XrXirTypes *copy = NULL;
    CHECK(xr_xir_types_clone(&types, &copy) == XR_XIR_OK);
    CHECK(copy->nodes[1].nominal.arguments != &argument);
    argument = XR_XIR_UNIT; memset(&f, 0xCC, sizeof(f)); memset(nodes, 0xCC, sizeof(nodes));
    budget = xr_xir_default_budget();
    CHECK(xr_xir_types_structure_verify(copy, &budget) == XR_XIR_OK);
    CHECK(copy->nodes[1].nominal.arguments[0] == (XrXirType) XR_XIR_CONSTRUCTED_TYPE_BASE);
    xr_xir_types_free(copy);
}

static void nominal_argument_identity(void) {
    NominalFixture f; nominal_fixture(&f);
    XrXirConstraint constraints[] = {{0}, {0}};
    for (uint32_t i = 0; i < 2; ++i) {
        f.declarations[i].parameter_count = 2; f.declarations[i].constraints = constraints;
    }
    XrXirType arguments[] = {XR_XIR_I64, XR_XIR_STRING, XR_XIR_STRING, XR_XIR_I64};
    XrXirTypeNode nodes[] = {
        {XR_XIR_TYPE_NOMINAL, XR_XIR_UNIT, NULL, 0, XR_XIR_UNIT, 0, 0, {0, arguments, 2, NULL, 0}},
        {XR_XIR_TYPE_NOMINAL, XR_XIR_UNIT, NULL, 0, XR_XIR_UNIT, 0, 0, {0, arguments + 2, 2, NULL, 0}}};
    XrXirTypes types = {nodes, 2, &f.table, NULL};
    XrXirBudget budget = xr_xir_default_budget();
    CHECK(xr_xir_types_structure_verify(&types, &budget) == XR_XIR_OK);
    nodes[1].nominal.arguments = arguments; budget = xr_xir_default_budget();
    CHECK(xr_xir_types_structure_verify(&types, &budget) == XR_XIR_BAD_STRUCTURE);
    for (uint32_t i = 0; i < 2; ++i) {
        f.declarations[i].parameter_count = 0; f.declarations[i].constraints = NULL;
        nodes[i].nominal = (XrXirNominalType) {i, NULL, 0, NULL, 0};
    }
    f.fields[0].type = XR_XIR_I64; budget = xr_xir_default_budget();
    CHECK(xr_xir_types_structure_verify(&types, &budget) == XR_XIR_OK);
    types.nominals = NULL; budget = xr_xir_default_budget();
    CHECK(xr_xir_types_structure_verify(&types, &budget) == XR_XIR_BAD_TYPE);
}

static void nominal_context_proofs(void) {
    NominalFixture f; nominal_fixture(&f);
    XrXirType argument = (XrXirType) (XR_XIR_TYPE_PARAMETER_BASE + 1);
    XrXirTypeNode nodes[] = {
        {XR_XIR_TYPE_ARRAY, argument, NULL, 0, XR_XIR_UNIT, 0, 2, {0}},
        {XR_XIR_TYPE_NOMINAL, XR_XIR_UNIT, NULL, 0, XR_XIR_UNIT, 0, 2, {0, &argument, 1, NULL, 0}}};
    XrXirTypes types = {nodes, 2, &f.table, NULL};
    const XrXirType nominal = (XrXirType) (XR_XIR_CONSTRUCTED_TYPE_BASE + 1);
    XrXirConstraint allowed[] = {{0}, {.markers = XR_XIR_CONSTRAINT_SENDABLE}}, denied[] = {{.markers = XR_XIR_CONSTRAINT_SENDABLE}, {0}};
    XrXirBudget initial = xr_xir_default_budget(), budget = initial;
    CHECK(nominal_context_use(&types, nominal, allowed, 2, &budget) == XR_XIR_OK);
    uint64_t bytes = initial.metadata_bytes - budget.metadata_bytes, work = initial.work - budget.work;
    CHECK(bytes > 0 && work > 0);
    budget = initial;
    CHECK(nominal_context_use(&types, nominal, denied, 2, &budget) == XR_XIR_BAD_TYPE);
    budget = initial;
    CHECK(nominal_context_use(&types, nominal, allowed, 1, &budget) == XR_XIR_BAD_TYPE);
    for (unsigned mode = 0; mode < 3; ++mode) {
        budget = initial; budget.metadata_bytes = bytes; budget.work = work;
        if (mode == 1) --budget.metadata_bytes;
        if (mode == 2) --budget.work;
        XrXirStatus status = nominal_context_use(&types, nominal, allowed, 2, &budget);
        if (status != (mode ? XR_XIR_BUDGET : XR_XIR_OK))
            fprintf(stderr,"nominal context mode=%u bytes=%llu work=%llu status=%u remaining=%llu/%llu\n",
                mode,(unsigned long long)bytes,(unsigned long long)work,(unsigned)status,
                (unsigned long long)budget.metadata_bytes,(unsigned long long)budget.work);
        CHECK(status == (mode ? XR_XIR_BUDGET : XR_XIR_OK));
    }
    argument = (XrXirType) XR_XIR_CONSTRUCTED_TYPE_BASE; budget = initial;
    CHECK(nominal_context_use(&types, nominal, allowed, 2, &budget) == XR_XIR_OK);
    budget = initial;
    CHECK(nominal_context_use(&types, nominal, denied, 2, &budget) == XR_XIR_BAD_TYPE);
    argument = nominal; budget = initial;
    CHECK(nominal_context_use(&types, nominal, allowed, 2, &budget) == XR_XIR_BAD_TYPE);
    argument = (XrXirType) XR_XIR_CONSTRUCTED_TYPE_BASE;
    /* A well-formed pool still needs the separate use-site context proofs above. */
    budget = initial;
    CHECK(xr_xir_types_structure_verify(&types, &budget) == XR_XIR_OK);
}

#include "xir_enum_metadata_fixture.h"
static void nominal_kind_boundaries(void) {
    EnumMetadataFixture f; enum_metadata_fixture(&f);
    XrXirTypeNode node = {0}; node.kind = XR_XIR_TYPE_NOMINAL;
    XrXirTypes types = {&node, 1, &f.table, NULL};
    XrXirType type = (XrXirType) XR_XIR_CONSTRUCTED_TYPE_BASE;
    CHECK(xr_xir_type_is_nominal(&types, type));
    CHECK(xr_xir_type_is_enum(&types, type) && !xr_xir_type_is_struct(&types, type));
    XrXirBudget budget = xr_xir_default_budget();
    XrXirNominalTable *projected = NULL;
    CHECK(xr_xir_nominal_project(&f.table, &budget, &projected) == XR_XIR_OK);
    if (!projected) return;
    types.nominals = projected;
    CHECK(xr_xir_type_is_enum(&types, type) && !xr_xir_type_is_struct(&types, type));
    XrXirNominalIdentity identity = projected->identities[0];
    XrXirNominalTable table = {NULL, 1, &identity}; types.nominals = &table;
    identity.kind = XR_XIR_NOMINAL_STRUCT;
    CHECK(xr_xir_type_is_struct(&types, type) && !xr_xir_type_is_enum(&types, type));
    identity.kind = UINT32_MAX;
    CHECK(!xr_xir_type_is_struct(&types, type) && !xr_xir_type_is_enum(&types, type));
    table.declarations = &f.declaration;
    CHECK(!xr_xir_type_is_struct(&types, type) && !xr_xir_type_is_enum(&types, type));
    table.identities = NULL;
    f.declaration.kind = XR_XIR_NOMINAL_STRUCT;
    CHECK(xr_xir_type_is_struct(&types, type) && !xr_xir_type_is_enum(&types, type));
    node.nominal.declaration = 1;
    CHECK(!xr_xir_type_is_struct(&types, type) && !xr_xir_type_is_enum(&types, type));
    node.nominal.declaration = 0; node.kind = XR_XIR_TYPE_ARRAY;
    CHECK(!xr_xir_type_is_struct(&types, type) && !xr_xir_type_is_enum(&types, type));
    node.kind = XR_XIR_TYPE_NOMINAL; table.declarations = NULL;
    CHECK(!xr_xir_type_is_struct(&types, type) && !xr_xir_type_is_enum(&types, type));
    types.nominals = NULL;
    CHECK(!xr_xir_type_is_struct(&types, type) && !xr_xir_type_is_enum(&types, type));
    CHECK(!xr_xir_type_is_struct(NULL, type) && !xr_xir_type_is_enum(NULL, type));
    CHECK(!xr_xir_type_is_struct(&types, XR_XIR_I64) && !xr_xir_type_is_enum(&types, XR_XIR_I64));
    xr_xir_nominal_free(projected);
}
static void enum_metadata_cases(void) {
    EnumMetadataFixture f; enum_metadata_fixture(&f);
    XrXirBudget b = xr_xir_default_budget();
    CHECK(xr_xir_nominal_structure_verify(&f.table, NULL, &b) == XR_XIR_OK);
    for (unsigned bad = 0; bad < 9; ++bad) {
        enum_metadata_fixture(&f);
        if (bad == 0) f.declaration.kind = 2;
        if (bad == 1) f.declaration.kind = XR_XIR_NOMINAL_STRUCT;
        if (bad == 2) { f.declaration.variants = NULL; f.declaration.variant_count = 0; }
        if (bad == 3) f.variants[2].field_begin = 0;
        if (bad == 4) f.variants[2].field_count = UINT32_MAX;
        if (bad == 5) f.variants[2].name = f.variants[1].name;
        if (bad == 6) f.fields[0].flags = XR_XIR_FIELD_PRIVATE;
        if (bad == 7) { f.variants[1].field_count = 2; f.variants[2].field_begin = 2; f.variants[2].field_count = 0; }
        if (bad == 8) f.variants[2].field_count = 0;
        b = xr_xir_default_budget(); XrXirBudget original = b;
        CHECK(xr_xir_nominal_structure_verify(&f.table, NULL, &b) == XR_XIR_BAD_STRUCTURE);
        CHECK(!memcmp(&b, &original, sizeof(b)));
    }
    enum_metadata_fixture(&f); f.fields[0].type = (XrXirType) XR_XIR_TYPE_PARAMETER_BASE;
    b = xr_xir_default_budget(); CHECK(xr_xir_nominal_structure_verify(&f.table, NULL, &b) == XR_XIR_BAD_TYPE);
    XrXirConstraint constraint = {.markers = XR_XIR_CONSTRAINT_SENDABLE};
    f.declaration.parameter_count = 1; f.declaration.constraints = &constraint;
    b = xr_xir_default_budget(); CHECK(xr_xir_nominal_structure_verify(&f.table, NULL, &b) == XR_XIR_OK);
    enum_metadata_fixture(&f); XrXirNominalTable *copy = NULL, *projected = NULL, *second = NULL;
    b = xr_xir_default_budget(); CHECK(xr_xir_nominal_clone(&f.table, NULL, &b, &copy) == XR_XIR_OK);
    memset(&f, 0xCC, sizeof(f));
    b = xr_xir_default_budget(); CHECK(xr_xir_nominal_project(copy, &b, &projected) == XR_XIR_OK);
    xr_xir_nominal_free(copy);
    b = xr_xir_default_budget(); CHECK(xr_xir_nominal_clone(projected, NULL, &b, &second) == XR_XIR_OK);
    xr_xir_nominal_free(projected);
    CHECK(second->identities[0].kind == XR_XIR_NOMINAL_ENUM && second->identities[0].variant_count == 3);
    CHECK(!memcmp(second->identities[0].variants[2].name.bytes, "Right", 5));
    CHECK(second->identities[0].variants[2].field_begin == 1);
    xr_xir_nominal_free(second);
    enum_metadata_fixture(&f); b = xr_xir_default_budget(); b.work = 1;
    CHECK(xr_xir_nominal_clone(&f.table, NULL, &b, &copy) == XR_XIR_BUDGET && !copy && b.work == 1);
    XrXirType fields[] = {XR_XIR_I64, XR_XIR_STRING};
    XrXirTypeNode node = {0}; node.kind = XR_XIR_TYPE_NOMINAL; node.nominal.fields = fields; node.nominal.field_count = 2;
    XrXirTypes types = {&node, 1, &f.table, NULL}; b = xr_xir_default_budget();
    CHECK(xr_xir_types_structure_verify(&types, &b) == XR_XIR_OK);
}

static void error_filter_packets(const XrXirArtifact *checked) {
    XrXirCheckedPacket packet = {0}; XrXirArtifact *decoded = NULL;
    CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OK);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(decoded); decoded = NULL;
    _Static_assert(XR_XIR_ERROR_IS == 95 && XR_XIR_ERROR_NARROW == 96, "error filter wire identity");
    const uint8_t needle[40] = {[0]=95, [4]=1, [25]=1};
    size_t offset = 0; uint32_t found = 0;
    for (size_t i = 64; i + 120 <= packet.length; ++i)
        if (!memcmp(packet.bytes+i,needle,sizeof(needle))) {offset=i; ++found;}
    CHECK(found == 1 && packet.bytes[offset+40] == XR_XIR_BRANCH && packet.bytes[offset+80] == 96);
    for (unsigned mode = 0; mode < 6; ++mode) {
        size_t position = mode == 0 ? offset+88 : mode == 1 ? offset+24 : mode == 2 ? offset+56 :
            mode == 3 ? offset+60 : mode == 4 ? offset+80 : 12;
        uint8_t saved = packet.bytes[position];
        packet.bytes[position] = mode == 0 ? 1 : mode == 1 ? 1 : mode == 2 ? 2 :
            mode == 3 ? 1 : mode == 4 ? XR_XIR_COPY : 31;
        XrSHA256Context hash; xr_sha256_init(&hash);
        xr_sha256_update(&hash,packet.bytes,32);
        xr_sha256_update(&hash,packet.bytes+64,packet.length-64);
        xr_sha256_final(&hash,packet.bytes+32);
        CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&decoded,NULL) != XR_XIR_OK && !decoded);
        packet.bytes[position] = saved;
    }
    xr_xir_checked_packet_free(&packet);
}

static void error_filter_guards(void) {
    for (unsigned mode = 0; mode < 9; ++mode) {
        EnumMetadataFixture f; enum_metadata_fixture(&f);
        XrXirType fields[] = {XR_XIR_I64, XR_XIR_STRING};
        XrXirTypeNode node = {.kind = XR_XIR_TYPE_NOMINAL, .nominal = {0, NULL, 0, fields, 2}};
        XrXirTypes types = {&node, 1, &f.table, NULL};
        XrXirType parameters[] = {XR_XIR_ERROR, XR_XIR_ERROR};
        XrXirInstruction ops[] = {
            {XR_XIR_ERROR_IS, XR_XIR_BOOL, {0}, {0}, 256, {0}},
            {XR_XIR_BRANCH, XR_XIR_UNIT, {2}, {1, 2}, 0, {0}},
            {XR_XIR_ERROR_NARROW, (XrXirType)256, {0}, {0}, 0, {0}},
            {XR_XIR_ENUM_TAG, XR_XIR_I64, {4}, {0}, 0, {0}},
            {XR_XIR_RETURN, XR_XIR_UNIT, {5}, {0}, 0, {0}},
            {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, -1, {0}},
            {XR_XIR_RETURN, XR_XIR_UNIT, {7}, {0}, 0, {0}}};
        XrXirBlock blocks[] = {{0, 2, 0, 0}, {2, 3, 0, 0}, {5, 2, 0, 0}};
        XrXirFunction function = {"filter", 6, parameters, 2, XR_XIR_I64, blocks, 3, ops, 7, NULL, 0};
        XrXirInstruction ret = {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}};
        XrXirBlock empty = {0, 1, 0, 0}, entry_block = {0, 2, 0, 0};
        XrXirInstruction entry_ops[] = {{XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 0, {0}},
            {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}};
        XrXirFunction functions[] = {
            {"init_alpha", 10, NULL, 0, XR_XIR_UNIT, &empty, 1, &ret, 1, NULL, 0},
            {"init_root", 9, NULL, 0, XR_XIR_UNIT, &empty, 1, &ret, 1, NULL, 0},
            {"entry", 5, NULL, 0, XR_XIR_I64, &entry_block, 1, entry_ops, 2, NULL, 0}, function};
        uint32_t dependency = 0;
        XrXirSourceModule modules[] = {{"alpha", 5, NULL, 0, 0}, {"root", 4, &dependency, 1, 1}};
        XrXirFunctionIdentity identities[] = {{0, 0, 0, 0, 0, 0, XR_XIR_NON_MEMBER}, {1, 0, 0, 0, 0, 0, XR_XIR_NON_MEMBER}, {1, 0, 0, 0, 0, 0, XR_XIR_NON_MEMBER}, {1, 0, 0, 0, 0, 0, XR_XIR_NON_MEMBER}};
        XrXirDeclarations declarations = {modules, 2, identities, NULL, 0, NULL, 0, 1, 2, NULL};
        XrXirModule module = {XR_XIR_BUILT, functions, 4, &declarations, NULL, &types, NULL, XR_XIR_PROGRAM, NULL};
        if (mode == 1) ops[2].args[0] = 1;
        if (mode == 2) { ops[1].targets[0] = 2; ops[1].targets[1] = 1; }
        if (mode == 3) ops[0].immediate = XR_XIR_I64;
        if (mode == 4) ops[0].immediate = INT64_MAX;
        if (mode == 5) ops[0] = (XrXirInstruction){XR_XIR_CONST_BOOL, XR_XIR_BOOL, {0}, {0}, 1, {0}};
        if (mode == 6) parameters[0] = (XrXirType)256;
        if (mode == 7) { ops[5] = (XrXirInstruction){XR_XIR_JUMP, XR_XIR_UNIT, {0}, {1}, 0, {0}}; blocks[2].count = 1; function.instruction_count = 6; }
        if (mode == 8) f.declaration.exported = false;
        functions[3] = function;
        XrXirArtifact *checked = NULL, *lowered = NULL;
        XrXirStatus status = xr_xir_check(&module, NULL, &checked, NULL);
        if (mode ? status == XR_XIR_OK : status != XR_XIR_OK) fprintf(stderr, "error filter mode %u status %u\n", mode, (unsigned) status);
        CHECK(mode ? status != XR_XIR_OK && !checked : status == XR_XIR_OK);
        if (!mode) {
            error_filter_packets(checked);
            CHECK(xr_xir_lower(checked, &fixture_target, NULL, &lowered, NULL) == XR_XIR_OK);
            xr_xir_artifact_free(lowered); xr_xir_artifact_free(checked);
        }
    }
}

static void panic_handler_packets(const XrXirArtifact *checked) {
    XrXirCheckedPacket packet = {0};
    XrXirArtifact *decoded = NULL;
    CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OK);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OK);
    CHECK(xr_xir_artifact_module(decoded)->functions[0].blocks[1].panic == 2);
    xr_xir_artifact_free(decoded); decoded = NULL;
    CHECK(packet.length == 497 && packet.bytes[121] == 2);
    CHECK(packet.bytes[349] == XR_XIR_PANIC_CATCH && packet.bytes[389] == XR_XIR_COPY);
    const struct { size_t offset; uint8_t value; } attacks[] = {
        {105, 2}, {121, 1}, {121, 3}, {121, 0}, {137, 2},
        {349, XR_XIR_COPY}, {389, XR_XIR_PANIC_CATCH}, {397, 2},
        {8, XR_XIR_CHECKED_SCHEMA - 1}, {12, XR_XIR_CHECKED_CONTRACT - 1}};
    for (size_t i = 0; i < sizeof(attacks) / sizeof(attacks[0]); ++i) {
        uint8_t saved = packet.bytes[attacks[i].offset];
        packet.bytes[attacks[i].offset] = attacks[i].value;
        XrSHA256Context hash; xr_sha256_init(&hash);
        xr_sha256_update(&hash, packet.bytes, 32);
        xr_sha256_update(&hash, packet.bytes + 64, packet.length - 64);
        xr_sha256_final(&hash, packet.bytes + 32);
        CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) != XR_XIR_OK && !decoded);
        packet.bytes[attacks[i].offset] = saved;
    }
    xr_xir_checked_packet_free(&packet);
}

static void panic_handler_structure(void) {
    Fixture ordinary;
    fixture_init(&ordinary);
    expect(&ordinary, XR_XIR_OK);
    for (unsigned mode = 0; mode < 9; ++mode) {
        XrXirBlock blocks[] = {{0, 2, 0, 0}, {2, 3, 2, 0}, {5, 3, 0, 0}};
        XrXirInstruction ops[] = {
            {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 9, {0}},
            {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {1}, 0, {0}},
            {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 0, {0}},
            {XR_XIR_DIV_INT, XR_XIR_I64, {0, 2}, {0}, 0, {0}},
            {XR_XIR_RETURN, XR_XIR_UNIT, {3}, {0}, 0, {0}},
            {XR_XIR_PANIC_CATCH, XR_XIR_UNIT, {0}, {0}, 0, {0}},
            {XR_XIR_COPY, XR_XIR_I64, {0}, {0}, 0, {0}},
            {XR_XIR_RETURN, XR_XIR_UNIT, {6}, {0}, 0, {0}}};
        XrXirFunction function = {"panic", 5, NULL, 0, XR_XIR_I64, blocks, 3, ops, 8, NULL, 0};
        XrXirModule module = {XR_XIR_BUILT, &function, 1, NULL, NULL, NULL, NULL, XR_XIR_PROGRAM, NULL};
        if (mode == 1) blocks[0].panic = 2;
        if (mode == 2) blocks[1].panic = 1;
        if (mode == 3) blocks[1].panic = 3;
        if (mode == 4) ops[4] = (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {2}, 0, {0}};
        if (mode == 5) ops[5] = (XrXirInstruction) {XR_XIR_COPY, XR_XIR_I64, {0}, {0}, 0, {0}};
        if (mode == 6) { XrXirInstruction temporary = ops[5]; ops[5] = ops[6]; ops[6] = temporary; }
        if (mode == 7) blocks[1].panic = 0;
        if (mode == 8) ops[6].args[0] = 2;
        XrXirArtifact *checked = NULL, *lowered = NULL;
        XrXirStatus status = xr_xir_check(&module, NULL, &checked, NULL);
        CHECK(mode ? status != XR_XIR_OK && !checked : status == XR_XIR_OK);
        if (!mode) {
            CHECK(xr_xir_lower(checked, &fixture_target, NULL, &lowered, NULL) == XR_XIR_OK);
            CHECK(xr_xir_artifact_module(lowered)->functions[0].blocks[1].panic == 2);
            panic_handler_packets(checked);
            xr_xir_artifact_free(lowered);
            xr_xir_artifact_free(checked);
        }
    }
}

#include "xir_class_core_cases.h"
#include "xir_class_array_core_cases.h"
#include "xir_method_owner_cases.h"

int main(void) {
    method_owner_cases();
    class_core_cases();
    class_array_core_cases();
    panic_handler_structure();
    error_filter_guards();
    nominal_kind_boundaries();
    enum_metadata_cases();
    cumulative_verification_budget();
    nominal_context_proofs();
    nominal_argument_identity();
    nominal_instance_metadata();
    nominal_pool_ownership();
    nominal_metadata_cases();
    array_metadata_cases();
    constructed_metadata();
    numeric_admission();
    for (uint32_t op = 1; op < XR_XIR_OP_COUNT; ++op)
        CHECK(xr_xir_op_name((XrXirOp) op) != NULL);
    CHECK(xr_xir_op_name(XR_XIR_INVALID) == NULL);
    CHECK(xr_xir_op_name((XrXirOp) UINT32_MAX) == NULL);
    transitions_and_lifetime();
    malformed_inputs();
    budgets();
    loops_and_storage_order();
    dominance_word_boundary();
    reverse_storage_and_boolean_values();
    puts("XIR stage, CFG, type, budget, and metadata lifetime assertions passed");
    return 0;
}
