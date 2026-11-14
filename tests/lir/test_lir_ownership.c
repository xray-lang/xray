/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_ownership.c - Final reference facts own their storage and fail closed
 */
#include "lir/xlir_ownership.h"
#include "lir/xlir_internal.h"
#include "lir/xlir_string_elide.h"
#include "lir/xlir_sccp.h"
#include "lir/xlir_verify.h"
#include "lir_array_fixture.h"
#include "lir_array_repeat_fixture.h"
#include "lir_array_ref_fixture.h"
#include "lir_ownership_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define OK(c) CHECK((c) == XR_LIR_OK)

static uint32_t count_kind(const XrLirOwnershipPlan *plan, XrLirOwnershipFactKind kind) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < xr_lir_ownership_fact_count(plan); ++i) {
        XrLirOwnershipFact fact;
        OK(xr_lir_ownership_fact_at(plan, i, &fact));
        count += fact.kind == kind;
    }
    return count;
}

static uint64_t digest(const XrLirOwnershipPlan *plan) {
    uint64_t hash = 0;
    for (uint32_t i = 0; i < xr_lir_ownership_fact_count(plan); ++i) {
        XrLirOwnershipFact f;
        OK(xr_lir_ownership_fact_at(plan, i, &f));
        uint32_t fields[] = {(uint32_t)f.kind, f.function, f.block, f.instruction,
            f.value, f.argument, f.edge, f.target_block, f.target_value, f.type,
            f.array_element, (uint32_t)f.array_operation};
        for (size_t p = 0; p < sizeof(fields) / sizeof(fields[0]); ++p)
            hash = hash * UINT64_C(16777619) + fields[p];
    }
    return hash;
}

static void arrays(XrLirType element) {
    XrLirModule *m = NULL;
    XrLirOwnershipPlan *plan = NULL;
    OK(array_fixture_create(element, &m));
    OK(xr_lir_ownership_plan_create(m, &plan, NULL));
    CHECK(xr_lir_ownership_function_count(plan) == 3);
    CHECK(count_kind(plan, XR_LIR_OWNERSHIP_SELECTED_EDGE_TRANSFER) == 2);
    CHECK(count_kind(plan, XR_LIR_OWNERSHIP_RETURN_TRANSFER) == 1);
    CHECK(count_kind(plan, XR_LIR_OWNERSHIP_BLOCK_OWNER) == 1);
    XrLirOwnershipFact edges[2] = {{0}};
    uint32_t edge_count = 0, next = 0;
    for (uint32_t function = 1; function <= 3; ++function) {
        XrLirOwnershipFunction view;
        OK(xr_lir_ownership_function_at(plan, function, &view));
        CHECK(!view.external && view.fact_offset == next);
        next += view.fact_count;
        CHECK(view.unmodeled == (function == 2 ? 0u : XR_LIR_OWNERSHIP_UNMODELED_CELL));
    }
    CHECK(next == xr_lir_ownership_fact_count(plan));
    for (uint32_t i = 0; i < next; ++i) {
        XrLirOwnershipFact fact;
        OK(xr_lir_ownership_fact_at(plan, i, &fact));
        if (fact.type == XR_LIR_ARRAY || fact.kind == XR_LIR_OWNERSHIP_ARRAY_OPERATION)
            CHECK(fact.array_element == element);
        if (fact.kind == XR_LIR_OWNERSHIP_SELECTED_EDGE_TRANSFER) {
            CHECK(edge_count < 2);
            edges[edge_count++] = fact;
        }
    }
    CHECK(edges[0].edge == 1 && edges[1].edge == 2 && edges[0].value == edges[1].value &&
        edges[0].target_block == edges[1].target_block && edges[0].target_value == edges[1].target_value &&
        edges[0].block == 1 && edges[0].target_block == 2);
    uint64_t before = digest(plan);
    XrLirOwnershipPlan *saved = plan;
    CHECK(xr_lir_ownership_plan_create(m, &plan, NULL) == XR_LIR_INVALID_ARGUMENT && plan == saved);
    xr_lir_module_destroy(m);
    CHECK(digest(plan) == before);
    XrLirOwnershipFact fact;
    OK(xr_lir_ownership_fact_at(plan, 0, &fact));
    fact.kind = XR_LIR_OWNERSHIP_RELEASE;
    CHECK(digest(plan) == before);
    CHECK(xr_lir_ownership_fact_at(plan, next, &fact) == XR_LIR_INVALID_ARGUMENT && !fact.kind);
    xr_lir_ownership_plan_destroy(plan);
}

static void copies(void) {
    XrLirType types[] = {XR_LIR_STRING, XR_LIR_MAP_STRING_I64};
    for (uint32_t t = 0; t < 2; ++t) {
        XrLirModule *m = NULL;
        XrLirOwnershipPlan *plan = NULL;
        OK(ownership_fixture(types[t], false, 2, &m));
        OK(xr_lir_ownership_plan_create(m, &plan, NULL));
        CHECK(count_kind(plan, XR_LIR_OWNERSHIP_PARAMETER_BORROW) == 2 &&
            count_kind(plan, XR_LIR_OWNERSHIP_RESULT_OWNER) == 2 &&
            count_kind(plan, XR_LIR_OWNERSHIP_RELEASE) == 1 &&
            count_kind(plan, XR_LIR_OWNERSHIP_RETURN_TRANSFER) == 1);
        if (types[t] == XR_LIR_STRING) {
            XrLirModule *elided = NULL;
            XrLirOwnershipPlan *final = NULL;
            OK(xr_lir_string_elide(m, &elided));
            OK(xr_lir_ownership_plan_create(elided, &final, NULL));
            CHECK(count_kind(final, XR_LIR_OWNERSHIP_RESULT_OWNER) == 1 &&
                count_kind(final, XR_LIR_OWNERSHIP_RETURN_TRANSFER) == 1 &&
                !count_kind(final, XR_LIR_OWNERSHIP_RELEASE));
            xr_lir_module_destroy(elided);
            CHECK(count_kind(final, XR_LIR_OWNERSHIP_PARAMETER_BORROW) == 2);
            xr_lir_ownership_plan_destroy(final);
        }
        xr_lir_module_destroy(m);
        CHECK(count_kind(plan, XR_LIR_OWNERSHIP_PARAMETER_BORROW) == 2);
        xr_lir_ownership_plan_destroy(plan);
    }
    XrLirModule *m = NULL;
    XrLirOwnershipPlan *plan = NULL;
    OK(ownership_fixture(XR_LIR_STRING, true, 2, &m));
    OK(xr_lir_ownership_plan_create(m, &plan, NULL));
    CHECK(count_kind(plan, XR_LIR_OWNERSHIP_APPEND_TRANSFER) == 1 &&
        count_kind(plan, XR_LIR_OWNERSHIP_OPERAND_BORROW) == 2 &&
        !count_kind(plan, XR_LIR_OWNERSHIP_RELEASE));
    xr_lir_module_destroy(m);
    xr_lir_ownership_plan_destroy(plan);
    m = NULL; plan = NULL;
    OK(ownership_fixture(XR_LIR_STRING, false, 24, &m));
    OK(xr_lir_ownership_plan_create(m, &plan, NULL));
    CHECK(count_kind(plan, XR_LIR_OWNERSHIP_RESULT_OWNER) == 24 &&
        count_kind(plan, XR_LIR_OWNERSHIP_RELEASE) == 23);
    uint64_t before = digest(plan);
    xr_lir_module_destroy(m);
    CHECK(digest(plan) == before);
    xr_lir_ownership_plan_destroy(plan);
}

static void other_shapes(void) {
    XrLirModule *m = NULL;
    XrLirOwnershipPlan *plan = NULL;
    OK(array_ref_fixture(XR_LIR_U8, &m));
    OK(xr_lir_ownership_plan_create(m, &plan, NULL));
    XrLirOwnershipFunction view;
    OK(xr_lir_ownership_function_at(plan, 2, &view));
    CHECK(view.unmodeled == XR_LIR_OWNERSHIP_UNMODELED_REFERENCE);
    xr_lir_module_destroy(m);
    xr_lir_ownership_plan_destroy(plan);
    m = NULL; plan = NULL;
    OK(array_repeat_fixture(XR_LIR_U64, &m));
    OK(xr_lir_ownership_plan_create(m, &plan, NULL));
    CHECK(count_kind(plan, XR_LIR_OWNERSHIP_ARRAY_OPERATION) == 1);
    for (uint32_t i = 0; i < xr_lir_ownership_fact_count(plan); ++i) {
        XrLirOwnershipFact fact;
        OK(xr_lir_ownership_fact_at(plan, i, &fact));
        if (fact.kind == XR_LIR_OWNERSHIP_ARRAY_OPERATION)
            CHECK(fact.array_operation == XR_LIR_ARRAY_OP_REPEAT && !fact.value &&
                !fact.type && fact.array_element == XR_LIR_U64);
    }
    xr_lir_module_destroy(m);
    xr_lir_ownership_plan_destroy(plan);
}

static void rebuilt_fact_identity(void) {
    XrLirModule *input = NULL, *rebuilt = NULL;
    XrLirOwnershipPlan *old = NULL, *final = NULL;
    OK(xr_lir_module_create(NULL, &input));
    XrLirType type = XR_LIR_STRING;
    XrLirFunctionSpec spec = {.name="choose", .name_length=6, .result=type,
        .parameters=&type, .parameter_count=1};
    XrLirFunction function = {0};
    XrLirBlock blocks[3] = {{0}};
    XrLirValue borrowed = {0}, owned = {0}, condition = {0}, parameters[2] = {{0}};
    OK(xr_lir_declare_function(input, &spec, &function));
    OK(xr_lir_define_function(input, function));
    OK(xr_lir_add_block(input, function, NULL, 0, &blocks[0]));
    for (uint32_t i = 1; i < 3; ++i) {
        OK(xr_lir_add_block(input, function, &type, 1, &blocks[i]));
        OK(xr_lir_block_parameter(input, blocks[i], 0, &parameters[i - 1]));
    }
    OK(xr_lir_set_entry(input, function, blocks[0]));
    OK(xr_lir_function_parameter(input, function, 0, &borrowed));
    XrLirInstruction instruction = {.op=XR_LIR_STRING_RETAIN, .type=type,
        .operands=&borrowed, .operand_count=1};
    OK(xr_lir_emit_value(input, blocks[0], &instruction, &owned));
    instruction = (XrLirInstruction){.op=XR_LIR_CONST, .type=XR_LIR_I1, .bits=0};
    OK(xr_lir_emit_value(input, blocks[0], &instruction, &condition));
    instruction = (XrLirInstruction){.op=XR_LIR_CONDBR, .type=XR_LIR_UNIT,
        .operands=&condition, .operand_count=1, .edge_count=2,
        .edges={{blocks[1], &owned, 1}, {blocks[2], &owned, 1}}};
    OK(xr_lir_set_terminator(input, blocks[0], &instruction));
    for (uint32_t i = 1; i < 3; ++i) {
        instruction = (XrLirInstruction){.op=XR_LIR_RET, .type=XR_LIR_UNIT,
            .operands=&parameters[i - 1], .operand_count=1};
        OK(xr_lir_set_terminator(input, blocks[i], &instruction));
    }
    OK(xr_lir_module_finish(input, NULL));
    OK(xr_lir_ownership_plan_create(input, &old, NULL));
    CHECK(count_kind(old, XR_LIR_OWNERSHIP_SELECTED_EDGE_TRANSFER) == 2 &&
        count_kind(old, XR_LIR_OWNERSHIP_RETURN_TRANSFER) == 2);
    uint32_t old_owner = owned.id, old_return = parameters[1].id;
    uint64_t old_digest = digest(old);
    OK(xr_lir_sccp(input, &rebuilt));
    CHECK(rebuilt && rebuilt != input);
    xr_lir_module_destroy(input);
    CHECK(digest(old) == old_digest);
    OK(xr_lir_ownership_plan_create(rebuilt, &final, NULL));
    OK(xr_lir_function_at(rebuilt, 0, &function));
    XrLirFunctionView shape;
    OK(xr_lir_function_view(rebuilt, function, &shape));
    CHECK(shape.block_count == 2 && count_kind(final, XR_LIR_OWNERSHIP_BLOCK_OWNER) == 1 &&
        count_kind(final, XR_LIR_OWNERSHIP_SELECTED_EDGE_TRANSFER) == 1 &&
        count_kind(final, XR_LIR_OWNERSHIP_RETURN_TRANSFER) == 1);
    uint32_t final_owner = 0, final_return = 0, edge_source = 0, edge_target = 0;
    for (uint32_t i = 0; i < xr_lir_ownership_fact_count(final); ++i) {
        XrLirOwnershipFact fact;
        OK(xr_lir_ownership_fact_at(final, i, &fact));
        CHECK(fact.function == function.id && fact.type == type);
        if (!fact.instruction) continue;
        XrLirBlock block;
        XrLirInstructionView actual;
        OK(xr_lir_block_at(rebuilt, function, fact.block - 1, &block));
        OK(xr_lir_instruction_view(rebuilt, block, fact.instruction - 1, &actual));
        if (fact.kind == XR_LIR_OWNERSHIP_RESULT_OWNER) {
            CHECK(actual.op == XR_LIR_STRING_RETAIN && fact.value == actual.result);
            final_owner = fact.value;
        } else if (fact.kind == XR_LIR_OWNERSHIP_SELECTED_EDGE_TRANSFER) {
            CHECK(actual.op == XR_LIR_BR && actual.edge_count == 1 && fact.edge == 1 && !fact.argument);
            CHECK(actual.edges[0].argument_count == 1 && fact.value == actual.edges[0].arguments[0] &&
                fact.target_block == actual.edges[0].target && fact.target_block == 2);
            XrLirBlockView target;
            OK(xr_lir_block_at(rebuilt, function, fact.target_block - 1, &block));
            OK(xr_lir_block_view(rebuilt, block, &target));
            CHECK(target.parameter_count == 1 && fact.target_value == target.parameters[0]);
            edge_source = fact.value;
            edge_target = fact.target_value;
        } else if (fact.kind == XR_LIR_OWNERSHIP_RETURN_TRANSFER) {
            CHECK(actual.op == XR_LIR_RET && actual.operand_count == 1 &&
                fact.value == actual.operands[0] && fact.block == 2);
            final_return = fact.value;
        }
    }
    CHECK(final_owner && final_return && final_owner == edge_source && final_return == edge_target &&
        final_owner != old_owner && final_return != old_return);
    uint64_t final_digest = digest(final);
    CHECK(final_digest != old_digest && digest(old) == old_digest &&
        count_kind(old, XR_LIR_OWNERSHIP_SELECTED_EDGE_TRANSFER) == 2);
    xr_lir_module_destroy(rebuilt);
    CHECK(digest(final) == final_digest && digest(old) == old_digest);
    xr_lir_ownership_plan_destroy(old);
    xr_lir_ownership_plan_destroy(final);
    printf("SCCP ownership IDs: owner %u -> %u, return %u -> %u; edges and RET 2 -> 1\n",
        old_owner, final_owner, old_return, final_return);
}

static void reject(const XrLirModule *m) {
    XrLirOwnershipPlan *plan = NULL;
    XrLirDiagnostic direct, captured;
    CHECK(xr_lir_verify(m, &direct) == XR_LIR_VERIFY_ERROR);
    CHECK(xr_lir_ownership_plan_create(m, &plan, &captured) == XR_LIR_VERIFY_ERROR && !plan);
    CHECK(direct.function == captured.function && direct.block == captured.block &&
        direct.instruction == captured.instruction && !strcmp(direct.message, captured.message));
}

static void malformed(void) {
    XrLirModule *m = NULL;
    XrLirOwnershipPlan *plan = NULL;
    OK(xr_lir_module_create(NULL, &m));
    CHECK(xr_lir_ownership_plan_create(m, &plan, NULL) == XR_LIR_INVALID_ARGUMENT && !plan);
    xr_lir_module_destroy(m);
    m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    OK(xr_lir_module_finish(m, NULL));
    OK(xr_lir_ownership_plan_create(m, &plan, NULL));
    CHECK(!xr_lir_ownership_function_count(plan) && !xr_lir_ownership_fact_count(plan));
    XrLirOwnershipFunction view;
    CHECK(xr_lir_ownership_function_at(plan, 1, &view) == XR_LIR_INVALID_ARGUMENT && !view.fact_count);
    CHECK(xr_lir_ownership_function_at(plan, 0, &view) == XR_LIR_INVALID_ARGUMENT);
    CHECK(xr_lir_ownership_function_at(plan, 1, NULL) == XR_LIR_INVALID_ARGUMENT);
    CHECK(xr_lir_ownership_fact_at(plan, 0, NULL) == XR_LIR_INVALID_ARGUMENT);
    xr_lir_module_destroy(m);
    xr_lir_ownership_plan_destroy(plan); plan = NULL;
    m = NULL;
    OK(ownership_fixture(XR_LIR_STRING, false, 2, &m));
    XrLirInstructionView *ret = &m->functions[0].blocks[0].instructions[3].view;
    uint32_t *operand = (uint32_t *)ret->operands, saved = *operand;
    *operand = m->functions[0].parameters[0];
    reject(m);
    *operand = saved;
    OK(xr_lir_verify(m, NULL));
    xr_lir_module_destroy(m);
    m = NULL;
    OK(array_fixture_create(XR_LIR_I64, &m));
    XrLirFunctionData *root = &m->functions[0];
    XrLirInstructionView *term = &root->blocks[0].instructions[root->blocks[0].instruction_count - 1].view;
    operand = (uint32_t *)term->edges[0].arguments; saved = *operand;
    *operand = 0;
    reject(m);
    *operand = root->blocks[0].instructions[2].view.result;
    reject(m);
    *operand = saved;
    XrLirType shape = root->values[saved - 1].array_element;
    root->values[saved - 1].array_element = XR_LIR_U8;
    reject(m);
    root->values[saved - 1].array_element = shape;
    OK(xr_lir_verify(m, NULL));
    xr_lir_module_destroy(m);
    CHECK(!xr_lir_ownership_function_count(NULL) && !xr_lir_ownership_fact_count(NULL));
    xr_lir_ownership_plan_destroy(NULL);
}

int main(void) {
    XrLirType elements[] = {XR_LIR_I64, XR_LIR_STRING, XR_LIR_U8, XR_LIR_U64};
    for (uint32_t i = 0; i < 4; ++i) arrays(elements[i]);
    copies(); other_shapes(); rebuilt_fact_identity(); malformed();
    puts("LIR ownership snapshots passed");
    return 0;
}
