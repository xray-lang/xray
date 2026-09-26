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
        fixture->blocks[b] = (XrXirBlock) {b * 2, 2};
    fixture->instructions[0] = (XrXirInstruction) {XR_XIR_COPY, XR_XIR_I64, {1, 0}, {0, 0}, 0};
    fixture->instructions[1] = (XrXirInstruction) {XR_XIR_BRANCH, XR_XIR_UNIT, {0, 0}, {1, 2}, 0};
    fixture->instructions[2] = (XrXirInstruction) {XR_XIR_ADD_I64, XR_XIR_I64, {2, 1}, {0, 0}, 0};
    fixture->instructions[3] = (XrXirInstruction) {XR_XIR_RETURN, XR_XIR_UNIT, {4, 0}, {0, 0}, 0};
    fixture->instructions[4] = (XrXirInstruction) {XR_XIR_CONST_I64, XR_XIR_I64, {0, 0}, {0, 0}, 9};
    fixture->instructions[5] = (XrXirInstruction) {XR_XIR_RETURN, XR_XIR_UNIT, {6, 0}, {0, 0}, 0};
    fixture->function = (XrXirFunction) {fixture->name, 5, fixture->parameters, 2,
        XR_XIR_I64, fixture->blocks, 3, fixture->instructions, 6, NULL, 0};
    fixture->module = (XrXirModule) {XR_XIR_BUILT, &fixture->function, 1, NULL};
}

static void expect(Fixture *fixture, XrXirStatus status) {
    XrXirDiagnostic diagnostic;
    CHECK(xr_xir_verify(&fixture->module, NULL, &diagnostic) == status);
    CHECK(diagnostic.status == status);
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
    f.instructions[0] = (XrXirInstruction) {XR_XIR_RETURN, XR_XIR_UNIT, {1, 0}, {0, 0}, 0};
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
        {XR_XIR_CONST_BOOL, XR_XIR_BOOL, {0, 0}, {0, 0}, 1},
        {XR_XIR_JUMP, XR_XIR_UNIT, {0, 0}, {2, 0}, 0},
        {XR_XIR_JUMP, XR_XIR_UNIT, {0, 0}, {2, 0}, 0},
        {XR_XIR_BRANCH, XR_XIR_UNIT, {0, 0}, {1, 3}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0, 0}, {0, 0}, 0},
    };
    XrXirBlock blocks[] = {{0, 2}, {2, 1}, {3, 1}, {4, 1}};
    XrXirFunction function = {"loop", 4, NULL, 0, XR_XIR_UNIT, blocks, 4, ops, 5, NULL, 0};
    XrXirModule module = {XR_XIR_BUILT, &function, 1, NULL};
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
        blocks[b] = (XrXirBlock) {next, b == 65 ? 2 : 1};
        if (b == 65) {
            definition = next;
            ops[next++] = (XrXirInstruction) {XR_XIR_CONST_I64, XR_XIR_I64, {0, 0}, {0, 0}, 42};
        }
        if (b == 69)
            ops[next++] = (XrXirInstruction) {XR_XIR_RETURN, XR_XIR_UNIT, {definition, 0}, {0, 0}, 0};
        else
            ops[next++] = (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0, 0}, {b + 1, 0}, 0};
    }
    XrXirFunction function = {"wide", 4, NULL, 0, XR_XIR_I64, blocks, 70, ops, 71, NULL, 0};
    XrXirModule module = {XR_XIR_BUILT, &function, 1, NULL};
    CHECK(xr_xir_verify(&module, NULL, NULL) == XR_XIR_OK);
    ops[64] = (XrXirInstruction) {XR_XIR_BRANCH, XR_XIR_UNIT, {0, 0}, {65, 69}, 0};
    XrXirType boolean = XR_XIR_BOOL;
    function.parameters = &boolean;
    function.parameter_count = 1;
    ops[70].args[0] = definition + 1;
    CHECK(xr_xir_verify(&module, NULL, NULL) == XR_XIR_BAD_DOMINANCE);
}

static void reverse_storage_and_boolean_values(void) {
    XrXirInstruction ops[] = {
        {XR_XIR_JUMP, XR_XIR_UNIT, {0, 0}, {2, 0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {4, 0}, {0, 0}, 0},
        {XR_XIR_CONST_I64, XR_XIR_I64, {0, 0}, {0, 0}, 7},
        {XR_XIR_EQ_I64, XR_XIR_BOOL, {2, 2}, {0, 0}, 0},
        {XR_XIR_COPY, XR_XIR_BOOL, {3, 0}, {0, 0}, 0},
        {XR_XIR_JUMP, XR_XIR_UNIT, {0, 0}, {1, 0}, 0},
    };
    XrXirBlock blocks[] = {{0, 1}, {1, 1}, {2, 4}};
    XrXirFunction function = {"reverse", 7, NULL, 0, XR_XIR_BOOL, blocks, 3, ops, 6, NULL, 0};
    XrXirModule module = {XR_XIR_BUILT, &function, 1, NULL};
    XrXirArtifact *checked = NULL, *lowered = NULL;
    CHECK(xr_xir_check(&module, NULL, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_lower(checked, &fixture_target, NULL, &lowered, NULL) == XR_XIR_OK);
    CHECK(xr_xir_artifact_module(lowered)->functions[0].instructions[4].op == XR_XIR_SCALAR_COPY);
    xr_xir_artifact_free(checked);
    xr_xir_artifact_free(lowered);
    ops[3].args[1] = 4;
    CHECK(xr_xir_verify(&module, NULL, NULL) == XR_XIR_BAD_TYPE);
}

int main(void) {
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
