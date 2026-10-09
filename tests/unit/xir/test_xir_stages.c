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

#include "xir_construction_fixture.h"
#include "xir/xxir.h"
#include "xir/xxir_types.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_constraint_proof.h"
#include "base/xsha256.h"
#include "base/xmalloc.h"
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

#include "xir_stage_context_owner.h"

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
    CHECK(xir_fixture_verify(&stage_context, &fixture->module, &diagnostic) == status);
    CHECK(diagnostic.status == status);
}

static void cumulative_verification_budget(void) {
    Fixture fixture; fixture_init(&fixture);
    XrXirCompileContext probe = stage_context_default(), initial = probe;
    XrCompileResourceStats before = stage_stats(&probe);
    XrXirDiagnostic diagnostic;
    CHECK(xir_fixture_verify(&probe, &fixture.module, &diagnostic) == XR_XIR_OK);
    CHECK(!memcmp(&probe, &initial, sizeof(probe)));
    XrCompileResourceStats after = stage_stats(&probe);
    uint64_t bytes = after.allocated_bytes - before.allocated_bytes;
    uint64_t work = after.work - before.work;
    CHECK(bytes && work && after.live_bytes == before.live_bytes);
    for (unsigned mode = 0; mode < 4; ++mode) {
        XrXirCompileContext context = stage_context_limited(bytes * 2 - (mode == 2),
            STAGE_LIVE_BYTES, work * 2 - (mode == 3));
        context.limits.functions = mode == 1 ? 0 : 1;
        XrXirCompileContext unchanged = context;
        XrXirStatus first = xir_fixture_verify(&context, &fixture.module, NULL);
        CHECK(first == (mode == 1 ? XR_XIR_BUDGET : XR_XIR_OK));
        if (mode != 1) {
            CHECK(xir_fixture_verify(&context, &fixture.module, &diagnostic) ==
                (mode ? XR_XIR_BUDGET : XR_XIR_OK));
            CHECK(diagnostic.status == (mode ? XR_XIR_BUDGET : XR_XIR_OK));
        }
        CHECK(!memcmp(&context, &unchanged, sizeof(context)));
        XrCompileResourceStats final = stage_stats(&context);
        CHECK(final.live_bytes == stage_owner_baseline.live_bytes);
        if (!mode) {
            CHECK(final.allocated_bytes == stage_owner_baseline.allocated_bytes + bytes * 2);
            CHECK(final.work == stage_owner_baseline.work + work * 2);
            CHECK(xir_fixture_verify(&context, &fixture.module, NULL) == XR_XIR_BUDGET);
            XrCompileResourceStats rejected = stage_stats(&context);
            CHECK(rejected.allocated_bytes >= final.allocated_bytes && rejected.work >= final.work);
        }
    }
    XrXirDiagnostic unchanged_diagnostic = diagnostic;
    CHECK(xir_fixture_verify(NULL, &fixture.module, &diagnostic) == XR_XIR_BAD_STRUCTURE);
    CHECK(!memcmp(&diagnostic, &unchanged_diagnostic, sizeof(diagnostic)));
}

static void graph_scratch_function_boundaries(void) {
    Fixture fixtures[4];
    XrXirFunction functions[4];
    for (uint32_t f = 0; f < 4; ++f) {
        fixture_init(&fixtures[f]);
        fixtures[f].name[4] = (char)('0' + f);
        functions[f] = fixtures[f].function;
    }
    /* Grow beyond one dominator word, then reuse smaller and equal shapes. */
    XrXirBlock large_blocks[65];
    XrXirInstruction large_ops[65];
    for (uint32_t b = 0; b < 65; ++b) {
        large_blocks[b] = (XrXirBlock){.first=b,.count=1};
        large_ops[b] = b < 64 ?
            (XrXirInstruction){.op=XR_XIR_JUMP,.type=XR_XIR_UNIT,.targets={b+1,0}} :
            (XrXirInstruction){.op=XR_XIR_RETURN,.type=XR_XIR_UNIT,.args={1,0}};
    }
    functions[1].blocks = large_blocks; functions[1].block_count = 65;
    functions[1].instructions = large_ops; functions[1].instruction_count = 65;
    /* In function 2, block 2 dominates block 1 despite reverse storage order.
     * The following function has sibling blocks: that fact must not survive. */
    fixtures[2].instructions[1] = (XrXirInstruction){.op=XR_XIR_JUMP,.type=XR_XIR_UNIT,.targets={2,0}};
    fixtures[2].instructions[5] = (XrXirInstruction){.op=XR_XIR_JUMP,.type=XR_XIR_UNIT,.targets={1,0}};
    fixtures[2].instructions[2].args[0] = 6;
    XrXirModule module = fixtures[0].module;
    module.functions = functions; module.function_count = 4;
    XrXirCompileContext context = stage_context_default();
    uint64_t baseline = stage_stats(&context).live_bytes;
    size_t physical_count = stage_physical_count, physical_bytes = stage_physical_bytes;
    XrXirDiagnostic diagnostic = {0};
    CHECK(xir_fixture_verify(&context, &module, &diagnostic) == XR_XIR_OK);
    CHECK(stage_stats(&context).live_bytes == baseline);
    CHECK(stage_physical_count == physical_count && stage_physical_bytes == physical_bytes);
    fixtures[3].instructions[2].args[0] = 6;
    CHECK(xir_fixture_verify(&context, &module, &diagnostic) == XR_XIR_BAD_DOMINANCE);
    CHECK(diagnostic.status == XR_XIR_BAD_DOMINANCE && diagnostic.function == 3 &&
        diagnostic.block == 1 && diagnostic.instruction == 2);
    CHECK(stage_stats(&context).live_bytes == baseline);
    CHECK(stage_physical_count == physical_count && stage_physical_bytes == physical_bytes);
    XrXirArtifact *rejected = NULL;
    CHECK(xir_fixture_check(&context, &module, &rejected, &diagnostic) == XR_XIR_BAD_DOMINANCE && !rejected);
    CHECK(diagnostic.status == XR_XIR_BAD_DOMINANCE && diagnostic.function == 3 &&
        diagnostic.block == 1 && diagnostic.instruction == 2);
    CHECK(stage_stats(&context).live_bytes == baseline);
    CHECK(stage_physical_count == physical_count && stage_physical_bytes == physical_bytes);
    fixtures[3].instructions[2].args[0] = 2;
    CHECK(xir_fixture_verify(&context, &module, &diagnostic) == XR_XIR_OK);
    CHECK(stage_stats(&context).live_bytes == baseline);
    CHECK(stage_physical_count == physical_count && stage_physical_bytes == physical_bytes);
}

static void transitions_and_lifetime(void) {
    Fixture fixture;
    fixture_init(&fixture);
    XrXirArtifact *checked = NULL, *lowered = NULL;
    CHECK(xir_fixture_check(&stage_context, &fixture.module, &checked, NULL) == XR_XIR_OK);
    CHECK(checked != NULL);
    const XrXirModule *module = xr_xir_compile_artifact_module(checked);
    CHECK(module->stage == XR_XIR_CHECKED);
    CHECK(module->functions != &fixture.function);
    CHECK(module->functions[0].name != fixture.name);
    CHECK(module->functions[0].parameters != fixture.parameters);
    CHECK(module->functions[0].blocks != fixture.blocks);
    CHECK(module->functions[0].instructions != fixture.instructions);
    memset(&fixture, 0xa5, sizeof(fixture));
    CHECK(xir_fixture_verify(&stage_context, module, NULL) == XR_XIR_OK);
    CHECK(memcmp(module->functions[0].name, "entry", 5) == 0);
    CHECK(xr_xir_compile_lower(checked, &fixture_target, &lowered, NULL) == XR_XIR_OK);
    CHECK(module->functions[0].instructions[0].op == XR_XIR_COPY);
    xr_xir_compile_artifact_free(checked);
    module = xr_xir_compile_artifact_module(lowered);
    CHECK(module->stage == XR_XIR_LOWERED);
    CHECK(module->functions[0].instructions[0].op == XR_XIR_SCALAR_COPY);
    CHECK(module->functions[0].instructions[4].immediate == 9);
    CHECK(xir_fixture_verify(&stage_context, module, NULL) == XR_XIR_OK);
    XrXirArtifact *occupied = lowered;
    CHECK(xr_xir_compile_lower(lowered, &fixture_target, &occupied, NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(occupied == lowered);
    XrXirArtifact *rejected = NULL;
    CHECK(xr_xir_compile_lower(lowered, &fixture_target, &rejected, NULL) == XR_XIR_BAD_STAGE);
    CHECK(rejected == NULL);
    CHECK(xir_fixture_check(&stage_context, module, &rejected, NULL) == XR_XIR_BAD_STAGE);
    CHECK(rejected == NULL);
    xr_xir_compile_artifact_free(lowered);
    xr_xir_compile_artifact_free(NULL);
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
    CHECK(xir_fixture_verify(&stage_context, &f.module, &diagnostic) == XR_XIR_BAD_DOMINANCE);
    CHECK(diagnostic.function == 0 && diagnostic.block == 2 && diagnostic.instruction == 5);
    XrXirArtifact *output = NULL;
    CHECK(xir_fixture_check(&stage_context, &f.module, &output, NULL) == XR_XIR_BAD_DOMINANCE);
    CHECK(!output);
    CHECK(xir_fixture_verify(&stage_context, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(xir_fixture_check(&stage_context, NULL, &output, NULL) == XR_XIR_BAD_STAGE);
    CHECK(xir_fixture_check(&stage_context, &f.module, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
}

static void budgets(void) {
    Fixture f;
    fixture_init(&f);
    XrXirCompileContext limits = stage_context_limited(STAGE_ALLOCATED_BYTES, STAGE_LIVE_BYTES, (1));
    CHECK(xir_fixture_verify(&limits, &f.module, NULL) == XR_XIR_BUDGET);
    limits = stage_context_limited(STAGE_ALLOCATED_BYTES, (1), STAGE_WORK);
    CHECK(xir_fixture_verify(&limits, &f.module, NULL) == XR_XIR_BUDGET);
    limits = stage_context_limited((1), STAGE_LIVE_BYTES, STAGE_WORK);
    CHECK(xir_fixture_verify(&limits, &f.module, NULL) == XR_XIR_BUDGET);
    limits = stage_context_default();
    limits.limits.functions = 0;
    CHECK(xir_fixture_verify(&limits, &f.module, NULL) == XR_XIR_BUDGET);
    limits = stage_context_default();
    limits.limits.parameters = 1;
    CHECK(xir_fixture_verify(&limits, &f.module, NULL) == XR_XIR_BUDGET);
    limits = stage_context_default();
    limits.limits.blocks = 2;
    CHECK(xir_fixture_verify(&limits, &f.module, NULL) == XR_XIR_BUDGET);
    limits = stage_context_default();
    limits.limits.instructions = 5;
    CHECK(xir_fixture_verify(&limits, &f.module, NULL) == XR_XIR_BUDGET);
    XrXirFunction functions[2] = {f.function, f.function};
    f.module.functions = functions;
    f.module.function_count = 2;
    limits.limits.instructions = 6;
    CHECK(xir_fixture_verify(&limits, &f.module, NULL) == XR_XIR_BUDGET);
    f.module.function_count = UINT32_MAX;
    CHECK(xir_fixture_verify(&stage_context, &f.module, NULL) == XR_XIR_BUDGET);
    fixture_init(&f);
    f.function.instruction_count = UINT32_MAX;
    CHECK(xir_fixture_verify(&stage_context, &f.module, NULL) == XR_XIR_BUDGET);
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
    CHECK(xir_fixture_verify(&stage_context, &module, NULL) == XR_XIR_OK);
    ops[0].immediate = 2;
    CHECK(xir_fixture_verify(&stage_context, &module, NULL) == XR_XIR_BAD_TYPE);
    ops[0].immediate = 1;
    ops[3].args[0] = 3;
    CHECK(xir_fixture_verify(&stage_context, &module, NULL) == XR_XIR_BAD_VALUE);
    XrXirCompileContext limits = stage_context_default();
    ops[3].args[0] = 0;
    limits = stage_context_limited(STAGE_ALLOCATED_BYTES, STAGE_LIVE_BYTES, 32);
    CHECK(xir_fixture_verify(&limits, &module, NULL) == XR_XIR_BUDGET);
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
    CHECK(xir_fixture_verify(&stage_context, &module, NULL) == XR_XIR_OK);
    ops[64] = (XrXirInstruction) {XR_XIR_BRANCH, XR_XIR_UNIT, {0, 0}, {65, 69}, 0, {0}};
    XrXirType boolean = XR_XIR_BOOL;
    function.parameters = &boolean;
    function.parameter_count = 1;
    ops[70].args[0] = definition + 1;
    CHECK(xir_fixture_verify(&stage_context, &module, NULL) == XR_XIR_BAD_DOMINANCE);
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
    CHECK(xir_fixture_check(&stage_context, &module, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_lower(checked, &fixture_target, &lowered, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_module(lowered)->functions[0].instructions[4].op == XR_XIR_SCALAR_COPY);
    xr_xir_compile_artifact_free(checked);
    xr_xir_compile_artifact_free(lowered);
    ops[3].args[1] = 4;
    CHECK(xir_fixture_verify(&stage_context, &module, NULL) == XR_XIR_BAD_TYPE);
}

static void numeric_admission(void) {
    for (XrXirOp op = XR_XIR_SUB_INT; op <= XR_XIR_SHR_INT; op = (XrXirOp) (op + 1)) {
        XrXirType parameters[] = {XR_XIR_I64, XR_XIR_I64};
        XrXirType result = op >= XR_XIR_NE_INT && op <= XR_XIR_GE_INT ? XR_XIR_BOOL : XR_XIR_I64;
        XrXirInstruction ops[] = {{op, result, {0, 1}, {0}, 0, {0}}, {XR_XIR_RETURN, XR_XIR_UNIT, {2}, {0}, 0, {0}}};
        const XrXirBlock block = {0, 2, 0, 0};
        XrXirFunction function = {"number", 6, parameters, 2, result, &block, 1, ops, 2, NULL, 0};
        XrXirModule module = {XR_XIR_BUILT, &function, 1, NULL, NULL, NULL, NULL, XR_XIR_PROGRAM, NULL};
        CHECK(xir_fixture_verify(&stage_context, &module, NULL) == XR_XIR_OK);
        parameters[1] = XR_XIR_BOOL;
        CHECK(xir_fixture_verify(&stage_context, &module, NULL) == XR_XIR_BAD_TYPE);
        parameters[1] = XR_XIR_I64; ops[0].type = XR_XIR_STRING;
        CHECK(xir_fixture_verify(&stage_context, &module, NULL) == XR_XIR_BAD_TYPE);
        ops[0].type = result; ops[0].immediate = 1;
        CHECK(xir_fixture_verify(&stage_context, &module, NULL) == XR_XIR_BAD_STRUCTURE);
        ops[0].immediate = 0; ops[0].args[1] = 2;
        CHECK(xir_fixture_verify(&stage_context, &module, NULL) != XR_XIR_OK);
    }
}

static void constructed_metadata(void) {
    XrXirCallableParameter input = {(XrXirType)256,0};
    XrXirTypeNode nodes[] = {
        {XR_XIR_TYPE_ARRAY,XR_XIR_STRING,NULL,0,XR_XIR_UNIT,0,0, {0}},
        {XR_XIR_TYPE_CELL,(XrXirType)256,NULL,0,XR_XIR_UNIT,0,0, {0}},
        {XR_XIR_TYPE_CALLABLE,XR_XIR_UNIT,&input,1,(XrXirType)256,XR_XIR_CALLABLE_ROOT_UNRESOLVED,0, {0}},
        {XR_XIR_TYPE_ARRAY,(XrXirType)258,NULL,0,XR_XIR_UNIT,0,0, {0}},
        {XR_XIR_TYPE_ARRAY,(XrXirType)(XR_XIR_TYPE_PARAMETER_LIMIT-1),NULL,0,XR_XIR_UNIT,0,65536, {0}}
    };
    XrXirTypes types = {nodes,5, NULL, NULL};
    XrXirCompileContext budget = stage_context_default();
    CHECK(xr_xir_compile_types_structure_verify(&budget, &types) == XR_XIR_OK);
    CHECK(xr_xir_type_is_array(&types,(XrXirType)256));
    CHECK(!xr_xir_type_is_callable(&types,(XrXirType)256));
    CHECK(xr_xir_type_is_cell(&types,(XrXirType)257));
    CHECK(xr_xir_type_is_callable(&types,(XrXirType)258));
    CHECK(!xr_xir_type_is_owned(NULL,(XrXirType)256));
    CHECK(!xr_xir_type_node(&types,(XrXirType)261));
    CHECK(!xr_xir_type_node(&types,(XrXirType)XR_XIR_CONSTRUCTED_TYPE_LIMIT));
    XrXirModule marker_module = {0}; marker_module.stage = XR_XIR_BUILT; marker_module.types = &types;
    XrXirProofContext marker_context = {&marker_module,{XR_XIR_CONTEXT_CLOSED,0,0}};
    CHECK(xr_xir_compile_type_markers_prove(&budget, &marker_context, (XrXirType)256, XR_XIR_CONSTRAINT_SENDABLE) == XR_XIR_OK);
    CHECK(xr_xir_compile_type_markers_prove(&budget, &marker_context, (XrXirType)259, XR_XIR_CONSTRAINT_SENDABLE) == XR_XIR_BAD_TYPE);
    budget = stage_context_limited(STAGE_ALLOCATED_BYTES, STAGE_LIVE_BYTES, 0);
    CHECK(xr_xir_compile_type_markers_prove(&budget, &marker_context, (XrXirType)256, XR_XIR_CONSTRAINT_SENDABLE) == XR_XIR_BUDGET);
    XrXirLayout layout;
    CHECK(xr_xir_compile_layout(&stage_context, &types, (XrXirType)256, &fixture_target, XR_XIR_LAYOUT_STORAGE, &layout) == XR_XIR_OK);
    CHECK(layout.size == 8 && layout.alignment == 8);
    CHECK(xr_xir_compile_layout(&stage_context, NULL, (XrXirType)256, &fixture_target, XR_XIR_LAYOUT_STORAGE, &layout) == XR_XIR_BAD_LAYOUT);
    CHECK(layout.size == 8 && layout.alignment == 8);
    CHECK(xr_xir_compile_layout(&stage_context, &types, (XrXirType)260, &fixture_target, XR_XIR_LAYOUT_STORAGE, &layout) == XR_XIR_BAD_LAYOUT);
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
        budget = stage_context_default();
        XrXirStatus status = xr_xir_compile_types_structure_verify(&budget, &types);
        if (attack == 4) CHECK(status == XR_XIR_OK); /* The original ID16 is now Rune. */
        else CHECK(status != XR_XIR_OK);
        memcpy(nodes,saved,sizeof(nodes)); input.type = (XrXirType)256;
    }
    nodes[0].element = (XrXirType) 17; /* The first unassigned scalar ID remains invalid. */
    budget = stage_context_default();
    CHECK(xr_xir_compile_types_structure_verify(&budget, &types) == XR_XIR_BAD_TYPE);
    nodes[0].element = XR_XIR_STRING;
    XrXirGeneric generic = {NULL,65536,NULL,0, NULL};
    XrXirModule context = {XR_XIR_BUILT,NULL,1,NULL,&generic,&types, NULL, XR_XIR_PROGRAM, NULL};
    CHECK(xr_xir_type_in_context(&context,0,(XrXirType)260));
    CHECK(xr_xir_type_in_context(&context,0,(XrXirType)(XR_XIR_TYPE_PARAMETER_LIMIT-1)));
    CHECK(!xr_xir_type_in_context(&context,0,(XrXirType)XR_XIR_TYPE_PARAMETER_LIMIT));
    generic.parameter_count = 65535;
    CHECK(!xr_xir_type_in_context(&context,0,(XrXirType)260));
    budget = stage_context_default();
    CHECK(xr_xir_compile_type_satisfies(&budget, &context, 0, (XrXirType)257, (XrXirConstraint){0}) == XR_XIR_BAD_TYPE);
    budget = stage_context_limited(1, STAGE_LIVE_BYTES, STAGE_WORK);
    CHECK(xr_xir_compile_types_structure_verify(&budget, &types) == XR_XIR_OK);
    XrXirTypes *denied_copy = NULL;
    CHECK(xr_xir_compile_types_clone(&budget, &types, &denied_copy) == XR_XIR_BUDGET && !denied_copy);
    budget = stage_context_limited(STAGE_ALLOCATED_BYTES, STAGE_LIVE_BYTES, (types.count));
    CHECK(xr_xir_compile_types_structure_verify(&budget, &types) == XR_XIR_BUDGET);
    XrXirTypes *copy = NULL;
    CHECK(xr_xir_compile_types_clone(&stage_context, &types, &copy) == XR_XIR_OK && copy && copy->nodes != nodes);
    CHECK(copy->nodes[2].parameters != &input);
    memset(nodes,0xCC,sizeof(nodes)); memset(&input,0xCC,sizeof(input));
    CHECK(xr_xir_compile_types_structure_verify(&stage_context, copy) == XR_XIR_OK);
    CHECK(copy->nodes[2].flags == 8u);
    xr_xir_compile_types_free(copy);
}

#include "xir_array_stage_cases.h"

#include "xir_nominal_fixture.h"
static void nominal_metadata_cases(void) {
    NominalFixture f; nominal_fixture(&f);
    XrXirCompileContext b = stage_context_default();
    CHECK(xr_xir_compile_nominal_structure_verify(&b, &f.table, NULL) == XR_XIR_OK);
    f.declarations[1].module = f.declarations[0].module;
    b = stage_context_default();
    CHECK(xr_xir_compile_nominal_structure_verify(&b, &f.table, NULL) == XR_XIR_BAD_STRUCTURE);
    nominal_fixture(&f); f.fields[1].name = f.fields[0].name;
    b = stage_context_default();
    CHECK(xr_xir_compile_nominal_structure_verify(&b, &f.table, NULL) == XR_XIR_BAD_STRUCTURE);
    nominal_fixture(&f); f.declarations[0].parameter_count = 0; f.declarations[0].constraints = NULL;
    b = stage_context_default();
    CHECK(xr_xir_compile_nominal_structure_verify(&b, &f.table, NULL) == XR_XIR_BAD_TYPE);
    nominal_fixture(&f); f.constraint.markers = 8;
    b = stage_context_default();
    CHECK(xr_xir_compile_nominal_structure_verify(&b, &f.table, NULL) == XR_XIR_OK);
    nominal_fixture(&f); f.constraint.markers = 64;
    b = stage_context_default();
    CHECK(xr_xir_compile_nominal_structure_verify(&b, &f.table, NULL) == XR_XIR_BAD_TYPE);
    nominal_fixture(&f); f.fields[0].flags = XR_XIR_FIELD_PRIVATE | XR_XIR_FIELD_PROTECTED;
    b = stage_context_default();
    CHECK(xr_xir_compile_nominal_structure_verify(&b, &f.table, NULL) == XR_XIR_BAD_STRUCTURE);
    nominal_fixture(&f); f.module[2] = 0;
    b = stage_context_default();
    CHECK(xr_xir_compile_nominal_structure_verify(&b, &f.table, NULL) == XR_XIR_BAD_STRUCTURE);
    nominal_fixture(&f); f.fields[0].type = XR_XIR_UNIT;
    b = stage_context_default();
    CHECK(xr_xir_compile_nominal_structure_verify(&b, &f.table, NULL) == XR_XIR_BAD_TYPE);
    nominal_fixture(&f);
    XrXirTypeNode node = {XR_XIR_TYPE_ARRAY, (XrXirType) (XR_XIR_TYPE_PARAMETER_BASE + 1), NULL, 0, XR_XIR_UNIT, 0, 2, {0}};
    XrXirTypes types = {&node, 1, NULL, NULL}; f.fields[0].type = (XrXirType) XR_XIR_CONSTRUCTED_TYPE_BASE;
    b = stage_context_default();
    CHECK(xr_xir_compile_nominal_structure_verify(&b, &f.table, &types) == XR_XIR_BAD_TYPE);
    node.element = (XrXirType) XR_XIR_TYPE_PARAMETER_BASE; node.parameter_span = 1;
    b = stage_context_default();
    CHECK(xr_xir_compile_nominal_structure_verify(&b, &f.table, &types) == XR_XIR_OK);
    node.kind = XR_XIR_TYPE_CELL;
    b = stage_context_default();
    CHECK(xr_xir_compile_nominal_structure_verify(&b, &f.table, &types) == XR_XIR_BAD_TYPE);
    nominal_fixture(&f); b = stage_context_limited(STAGE_ALLOCATED_BYTES, STAGE_LIVE_BYTES, (1));
    CHECK(xr_xir_compile_nominal_structure_verify(&b, &f.table, NULL) == XR_XIR_BUDGET && stage_stats(&b).work == stage_owner_baseline.work);
    b = stage_context_limited(1, STAGE_LIVE_BYTES, STAGE_WORK);
    CHECK(xr_xir_compile_nominal_structure_verify(&b, &f.table, NULL) == XR_XIR_OK);
    XrXirNominalTable *denied_copy = NULL;
    CHECK(xr_xir_compile_nominal_clone(&b, &f.table, NULL, &denied_copy) == XR_XIR_BUDGET && !denied_copy);
    b = stage_context_default();
    XrXirNominalTable *copy = NULL;
    CHECK(xr_xir_compile_nominal_clone(&b, &f.table, NULL, &copy) == XR_XIR_OK && copy);
    memset(&f, 0xCC, sizeof(f));
    CHECK(xr_xir_compile_nominal_structure_verify(&b, copy, NULL) == XR_XIR_OK);
    CHECK(copy->count == 2 && memcmp(copy->declarations[0].module.bytes, "alpha", 5) == 0);
    CHECK(copy->declarations[0].constraints[0].markers == XR_XIR_CONSTRAINT_SENDABLE);
    CHECK(memcmp(copy->declarations[0].fields[0].name.bytes, "value", 5) == 0);
    xr_xir_compile_nominal_free(copy); copy = NULL;
}

static XrXirStatus nominal_context_use(const XrXirTypes *types, XrXirType type,
    const XrXirConstraint *constraints, uint32_t count, XrXirCompileContext *budget) {
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
    XrXirGeneric generics[] = {{0},{0},{constraints,count,NULL,0, NULL}};
    XrXirModule module = {XR_XIR_BUILT,functions,3,&declarations,generics,types,NULL, XR_XIR_PROGRAM, NULL};
    XrXirProofContext context = {&module,{XR_XIR_CONTEXT_FUNCTION,2,0}};
    XrXirStatus status = xr_xir_compile_types_structure_verify(budget, types);
    return status == XR_XIR_OK ? xr_xir_compile_type_use_verify(budget, &context, type) : status;
}
static XrXirStatus nominal_catalog_semantics(const XrXirTypes *types, XrXirCompileContext *budget) {
    XrXirInstruction op = {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    XrXirBlock block = {0,1,0,0};
    XrXirInstruction entry_ops[] = {
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirBlock entry_block = {0,2,0,0};
    XrXirFunction functions[] = {
        {"alpha_init",10,NULL,0,XR_XIR_UNIT,&block,1,&op,1,NULL,0},
        {"other_init",10,NULL,0,XR_XIR_UNIT,&block,1,&op,1,NULL,0},
        {"entry",5,NULL,0,XR_XIR_I64,&entry_block,1,entry_ops,2,NULL,0}};
    uint32_t dependency = 1;
    XrXirSourceModule modules[] = {{"alpha",5,&dependency,1,0},{"other",5,NULL,0,1}};
    XrXirFunctionIdentity identities[] = {{0},{.module=1},{0}};
    XrXirDeclarations declarations = {modules,2,identities,NULL,0,NULL,0,0,2,NULL};
    XrXirModule module = {XR_XIR_BUILT,functions,3,&declarations,NULL,types,NULL, XR_XIR_PROGRAM, NULL};
    return xir_fixture_verify(budget, &module, NULL);
}
static void nominal_pool_ownership(void) {
    NominalFixture f; nominal_fixture(&f);
    XrXirTypes types = {NULL, 0, &f.table, NULL};
    XrXirCompileContext budget = stage_context_default();
    CHECK(xr_xir_compile_types_structure_verify(&budget, &types) == XR_XIR_OK);
    XrXirTypes *copy = NULL;
    CHECK(xr_xir_compile_types_clone(&stage_context, &types, &copy) == XR_XIR_OK && copy && copy->nominals);
    CHECK(copy->nominals != &f.table && copy->nominals->declarations != f.declarations);
    memset(&f, 0xCC, sizeof(f));
    budget = stage_context_default();
    CHECK(xr_xir_compile_types_structure_verify(&budget, copy) == XR_XIR_OK);
    CHECK(memcmp(copy->nominals->declarations[0].name.bytes, "Pair", 4) == 0);
    Fixture source; fixture_init(&source); source.module.types = copy;
    XrXirArtifact *artifact = NULL;
    CHECK(xir_fixture_check(&stage_context, &source.module, &artifact, NULL) == XR_XIR_BAD_STRUCTURE && !artifact);
    xr_xir_compile_types_free(copy);
    types.nominals = NULL;
    budget = stage_context_default();
    CHECK(xr_xir_compile_types_structure_verify(&budget, &types) == XR_XIR_BAD_STRUCTURE);
}

static void nominal_instance_metadata(void) {
    NominalFixture f; nominal_fixture(&f);
    XrXirType argument = (XrXirType) XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirTypeNode nodes[] = {{XR_XIR_TYPE_ARRAY, XR_XIR_STRING, NULL, 0, XR_XIR_UNIT, 0, 0, {0}},
        {XR_XIR_TYPE_NOMINAL, XR_XIR_UNIT, NULL, 0, XR_XIR_UNIT, 0, 0, {0, &argument, 1, NULL, 0}},
        {XR_XIR_TYPE_NOMINAL, XR_XIR_UNIT, NULL, 0, XR_XIR_UNIT, 0, 0, {1, &argument, 1, NULL, 0}}};
    XrXirTypes types = {nodes, 3, &f.table, NULL};
    XrXirCompileContext budget = stage_context_default();
    CHECK(xr_xir_compile_types_structure_verify(&budget, &types) == XR_XIR_OK);
    CHECK(nominal_catalog_semantics(&types, &budget) == XR_XIR_OK);
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
        if (mode == 10) {
            nodes[0].kind = XR_XIR_TYPE_CALLABLE; nodes[0].element = XR_XIR_UNIT;
            nodes[0].result = XR_XIR_STRING; nodes[0].flags = XR_XIR_CALLABLE_ROOT_UNRESOLVED;
        }
        budget = stage_context_default();
        CHECK(nominal_catalog_semantics(&types, &budget) != XR_XIR_OK);
        nodes[2] = saved; argument = saved_argument;
        nodes[0] = (XrXirTypeNode) {XR_XIR_TYPE_ARRAY, XR_XIR_STRING, NULL, 0, XR_XIR_UNIT, 0, 0, {0}};
    }
    nodes[0].nominal.declaration = 1; budget = stage_context_default();
    CHECK(xr_xir_compile_types_structure_verify(&budget, &types) == XR_XIR_BAD_STRUCTURE);
    nodes[0].nominal.declaration = 0;
    XrXirTypes *copy = NULL;
    CHECK(xr_xir_compile_types_clone(&stage_context, &types, &copy) == XR_XIR_OK);
    CHECK(copy->nodes[1].nominal.arguments != &argument);
    argument = XR_XIR_UNIT; memset(&f, 0xCC, sizeof(f)); memset(nodes, 0xCC, sizeof(nodes));
    budget = stage_context_default();
    CHECK(xr_xir_compile_types_structure_verify(&budget, copy) == XR_XIR_OK);
    CHECK(copy->nodes[1].nominal.arguments[0] == (XrXirType) XR_XIR_CONSTRUCTED_TYPE_BASE);
    xr_xir_compile_types_free(copy);
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
    XrXirCompileContext budget = stage_context_default();
    CHECK(xr_xir_compile_types_structure_verify(&budget, &types) == XR_XIR_OK);
    nodes[1].nominal.arguments = arguments; budget = stage_context_default();
    CHECK(xr_xir_compile_types_structure_verify(&budget, &types) == XR_XIR_BAD_STRUCTURE);
    for (uint32_t i = 0; i < 2; ++i) {
        f.declarations[i].parameter_count = 0; f.declarations[i].constraints = NULL;
        nodes[i].nominal = (XrXirNominalType) {i, NULL, 0, NULL, 0};
    }
    f.fields[0].type = XR_XIR_I64; budget = stage_context_default();
    CHECK(xr_xir_compile_types_structure_verify(&budget, &types) == XR_XIR_OK);
    types.nominals = NULL; budget = stage_context_default();
    CHECK(xr_xir_compile_types_structure_verify(&budget, &types) == XR_XIR_BAD_TYPE);
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
    XrXirCompileContext budget = stage_context_default();
    XrCompileResourceStats before = stage_stats(&budget);
    CHECK(nominal_context_use(&types, nominal, allowed, 2, &budget) == XR_XIR_OK);
    XrCompileResourceStats after = stage_stats(&budget);
    uint64_t bytes = after.allocated_bytes - before.allocated_bytes, work = after.work - before.work;
    CHECK(bytes > 0 && work > 0 && before.live_bytes == after.live_bytes);
    budget = stage_context_default();
    CHECK(nominal_context_use(&types, nominal, denied, 2, &budget) == XR_XIR_BAD_TYPE);
    budget = stage_context_default();
    CHECK(nominal_context_use(&types, nominal, allowed, 1, &budget) == XR_XIR_BAD_TYPE);
    for (unsigned mode = 0; mode < 3; ++mode) {
        budget = stage_context_limited(bytes - (mode == 1), STAGE_LIVE_BYTES, work - (mode == 2));
        XrXirCompileContext unchanged = budget;
        XrXirStatus status = nominal_context_use(&types, nominal, allowed, 2, &budget);
        CHECK(status == (mode ? XR_XIR_BUDGET : XR_XIR_OK));
        CHECK(!memcmp(&budget, &unchanged, sizeof(budget)));
        CHECK(stage_stats(&budget).live_bytes == stage_owner_baseline.live_bytes);
    }
    argument = (XrXirType) XR_XIR_CONSTRUCTED_TYPE_BASE; budget = stage_context_default();
    CHECK(nominal_context_use(&types, nominal, allowed, 2, &budget) == XR_XIR_OK);
    budget = stage_context_default();
    CHECK(nominal_context_use(&types, nominal, denied, 2, &budget) == XR_XIR_BAD_TYPE);
    argument = nominal; budget = stage_context_default();
    CHECK(nominal_context_use(&types, nominal, allowed, 2, &budget) == XR_XIR_BAD_TYPE);
    argument = (XrXirType) XR_XIR_CONSTRUCTED_TYPE_BASE;
    /* A well-formed pool still needs the separate use-site context proofs above. */
    budget = stage_context_default();
    CHECK(xr_xir_compile_types_structure_verify(&budget, &types) == XR_XIR_OK);
}

#include "xir_enum_metadata_fixture.h"
static void nominal_kind_boundaries(void) {
    EnumMetadataFixture f; enum_metadata_fixture(&f);
    XrXirTypeNode node = {0}; node.kind = XR_XIR_TYPE_NOMINAL;
    XrXirTypes types = {&node, 1, &f.table, NULL};
    XrXirType type = (XrXirType) XR_XIR_CONSTRUCTED_TYPE_BASE;
    CHECK(xr_xir_type_is_nominal(&types, type));
    CHECK(xr_xir_type_is_enum(&types, type) && !xr_xir_type_is_struct(&types, type));
    XrXirCompileContext budget = stage_context_default();
    XrXirNominalTable *projected = NULL;
    CHECK(xr_xir_compile_nominal_project(&budget, &f.table, &projected) == XR_XIR_OK);
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
    xr_xir_compile_nominal_free(projected);
}
static void enum_metadata_cases(void) {
    EnumMetadataFixture f; enum_metadata_fixture(&f);
    XrXirCompileContext b = stage_context_default();
    CHECK(xr_xir_compile_nominal_structure_verify(&b, &f.table, NULL) == XR_XIR_OK);
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
        b = stage_context_default(); XrXirCompileContext original = b;
        CHECK(xr_xir_compile_nominal_structure_verify(&b, &f.table, NULL) == XR_XIR_BAD_STRUCTURE);
        CHECK(!memcmp(&b, &original, sizeof(b)));
    }
    enum_metadata_fixture(&f); f.fields[0].type = (XrXirType) XR_XIR_TYPE_PARAMETER_BASE;
    b = stage_context_default(); CHECK(xr_xir_compile_nominal_structure_verify(&b, &f.table, NULL) == XR_XIR_BAD_TYPE);
    XrXirConstraint constraint = {.markers = XR_XIR_CONSTRAINT_SENDABLE};
    f.declaration.parameter_count = 1; f.declaration.constraints = &constraint;
    b = stage_context_default(); CHECK(xr_xir_compile_nominal_structure_verify(&b, &f.table, NULL) == XR_XIR_OK);
    enum_metadata_fixture(&f); XrXirNominalTable *copy = NULL, *projected = NULL, *second = NULL;
    b = stage_context_default(); CHECK(xr_xir_compile_nominal_clone(&b, &f.table, NULL, &copy) == XR_XIR_OK);
    memset(&f, 0xCC, sizeof(f));
    CHECK(xr_xir_compile_nominal_project(&b, copy, &projected) == XR_XIR_OK);
    xr_xir_compile_nominal_free(copy); copy = NULL;
    CHECK(xr_xir_compile_nominal_clone(&b, projected, NULL, &second) == XR_XIR_OK);
    xr_xir_compile_nominal_free(projected);
    CHECK(second->identities[0].kind == XR_XIR_NOMINAL_ENUM && second->identities[0].variant_count == 3);
    CHECK(!memcmp(second->identities[0].variants[2].name.bytes, "Right", 5));
    CHECK(second->identities[0].variants[2].field_begin == 1);
    xr_xir_compile_nominal_free(second);
    enum_metadata_fixture(&f); b = stage_context_limited(STAGE_ALLOCATED_BYTES, STAGE_LIVE_BYTES, (1));
    CHECK(xr_xir_compile_nominal_clone(&b, &f.table, NULL, &copy) == XR_XIR_BUDGET && !copy && stage_stats(&b).work > stage_owner_baseline.work);
    XrXirType fields[] = {XR_XIR_I64, XR_XIR_STRING};
    XrXirTypeNode node = {0}; node.kind = XR_XIR_TYPE_NOMINAL; node.nominal.fields = fields; node.nominal.field_count = 2;
    XrXirTypes types = {&node, 1, &f.table, NULL}; b = stage_context_default();
    CHECK(xr_xir_compile_types_structure_verify(&b, &types) == XR_XIR_OK);
}

static void error_filter_packets(const XrXirArtifact *checked) {
    XrXirCheckedPacket packet = {0}; XrXirArtifact *decoded = NULL;
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(&stage_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded); decoded = NULL;
    _Static_assert(XR_XIR_ERROR_IS == 103 && XR_XIR_ERROR_NARROW == 104, "error filter wire identity");
    const uint8_t needle[40] = {[0]=103, [4]=1, [25]=1};
    size_t offset = 0; uint32_t found = 0;
    for (size_t i = 64; i + 120 <= packet.length; ++i)
        if (!memcmp(packet.bytes+i,needle,sizeof(needle))) {offset=i; ++found;}
    CHECK(found == 1 && packet.bytes[offset+40] == XR_XIR_BRANCH && packet.bytes[offset+80] == 104);
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
        CHECK(xr_xir_compile_checked_read(&stage_context, packet.bytes, packet.length, &decoded, NULL) != XR_XIR_OK && !decoded);
        packet.bytes[position] = saved;
    }
    xr_xir_compile_checked_packet_free(&packet);
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
        XrXirFunctionIdentity identities[] = {{0, 0, 0, 0, 0, 0, XR_XIR_NON_MEMBER, 0, 0}, {1, 0, 0, 0, 0, 0, XR_XIR_NON_MEMBER, 0, 0}, {1, 0, 0, 0, 0, 0, XR_XIR_NON_MEMBER, 0, 0}, {1, 0, 0, 0, 0, 0, XR_XIR_NON_MEMBER, 0, 0}};
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
        XrXirStatus status = xir_fixture_check(&stage_context, &module, &checked, NULL);
        if (mode ? status == XR_XIR_OK : status != XR_XIR_OK) fprintf(stderr, "error filter mode %u status %u\n", mode, (unsigned) status);
        CHECK(mode ? status != XR_XIR_OK && !checked : status == XR_XIR_OK);
        if (!mode) {
            error_filter_packets(checked);
            CHECK(xr_xir_compile_lower(checked, &fixture_target, &lowered, NULL) == XR_XIR_OK);
            xr_xir_compile_artifact_free(lowered); xr_xir_compile_artifact_free(checked);
        }
    }
}

static void panic_handler_packets(const XrXirArtifact *checked) {
    XrXirCheckedPacket packet = {0};
    XrXirArtifact *decoded = NULL;
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(&stage_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_module(decoded)->functions[0].blocks[1].panic == 2);
    xr_xir_compile_artifact_free(decoded); decoded = NULL;
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
        CHECK(xr_xir_compile_checked_read(&stage_context, packet.bytes, packet.length, &decoded, NULL) != XR_XIR_OK && !decoded);
        packet.bytes[attacks[i].offset] = saved;
    }
    xr_xir_compile_checked_packet_free(&packet);
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
        XrXirStatus status = xir_fixture_check(&stage_context, &module, &checked, NULL);
        CHECK(mode ? status != XR_XIR_OK && !checked : status == XR_XIR_OK);
        if (!mode) {
            CHECK(xr_xir_compile_lower(checked, &fixture_target, &lowered, NULL) == XR_XIR_OK);
            CHECK(xr_xir_compile_artifact_module(lowered)->functions[0].blocks[1].panic == 2);
            panic_handler_packets(checked);
            xr_xir_compile_artifact_free(lowered);
            xr_xir_compile_artifact_free(checked);
        }
    }
}

#include "xir_class_core_cases.h"
#include "xir_class_array_core_cases.h"
#include "xir_method_owner_cases.h"

int main(void) {
    stage_context = stage_context_default();
    method_owner_cases();
    class_core_cases();
    class_array_core_cases();
    panic_handler_structure();
    error_filter_guards();
    nominal_kind_boundaries();
    enum_metadata_cases();
    cumulative_verification_budget();
    graph_scratch_function_boundaries();
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
    stage_contexts_free();
    puts("XIR stage, CFG, type, budget, and metadata lifetime assertions passed");
    return 0;
}
