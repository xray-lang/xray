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
#include "lir/xlir_value_layout.h"
#include "lir/xlir_cell.h"
#include "lir_array_fixture.h"
#include "lir_array_repeat_fixture.h"
#include "lir_array_ref_fixture.h"
#include "lir_ownership_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define OK(c) CHECK((c) == XR_LIR_OK)
#include "lir_record_fixture.h"
#include "lir_string_fixture.h"

static uint32_t count_kind(const XrLirOwnershipPlan *plan, XrLirOwnershipFactKind kind) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < xr_lir_ownership_fact_count(plan); ++i) {
        XrLirOwnershipFact fact;
        OK(xr_lir_ownership_fact_at(plan, i, &fact));
        count += fact.kind == kind;
    }
    return count;
}
static void compare_internal(const XrLirOwnershipRequest *request, const XrLirOwnershipPlan *public_plan) {
    XrLirOwnershipPlan *internal = NULL;
    OK(xr_lir_internal_ownership_collect(request,&internal,NULL));
    CHECK(xr_lir_ownership_function_count(internal) == xr_lir_ownership_function_count(public_plan));
    CHECK(xr_lir_ownership_fact_count(internal) == xr_lir_ownership_fact_count(public_plan));
    for (uint32_t i = 1; i <= xr_lir_ownership_function_count(internal); ++i) {
        XrLirOwnershipFunction a, b;
        OK(xr_lir_ownership_function_at(internal,i,&a)); OK(xr_lir_ownership_function_at(public_plan,i,&b));
        CHECK(a.fact_offset == b.fact_offset && a.fact_count == b.fact_count &&
            a.unmodeled == b.unmodeled && a.external == b.external &&
            a.region_offset == b.region_offset && a.region_count == b.region_count);
    }
    for (uint32_t i = 0; i < xr_lir_ownership_fact_count(internal); ++i) {
        XrLirOwnershipFact a, b;
        OK(xr_lir_ownership_fact_at(internal,i,&a)); OK(xr_lir_ownership_fact_at(public_plan,i,&b));
        CHECK(a.kind == b.kind && a.function == b.function && a.block == b.block && a.instruction == b.instruction &&
            a.value == b.value && a.argument == b.argument && a.edge == b.edge && a.target_block == b.target_block &&
            a.target_value == b.target_value && a.type == b.type && a.array_element == b.array_element &&
            a.array_operation == b.array_operation && a.map_operation == b.map_operation &&
            a.callable_transport==b.callable_transport && a.callable_operation==b.callable_operation &&
            a.callable_function==b.callable_function && a.callable_scope==b.callable_scope &&
            a.callable_materialize==b.callable_materialize && a.transfer_mode==b.transfer_mode &&
            a.string_operation==b.string_operation && a.forwarding_mode==b.forwarding_mode &&
            a.forwarding_drop==b.forwarding_drop);
        if (a.kind==XR_LIR_OWNERSHIP_SELECTED_EDGE_TRANSFER || a.kind==XR_LIR_OWNERSHIP_RETURN_TRANSFER ||
            a.kind==XR_LIR_OWNERSHIP_APPEND_TRANSFER) CHECK(a.transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_MOVE);
        if (a.kind==XR_LIR_OWNERSHIP_PARAMETER_BORROW || a.kind==XR_LIR_OWNERSHIP_OPERAND_BORROW)
            CHECK(a.transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_BORROW);
    }
    xr_lir_ownership_plan_destroy(internal);
}

static uint64_t digest(const XrLirOwnershipPlan *plan) {
    uint64_t hash = 0;
    for (uint32_t i = 0; i < xr_lir_ownership_fact_count(plan); ++i) {
        XrLirOwnershipFact f;
        OK(xr_lir_ownership_fact_at(plan, i, &f));
        uint32_t fields[] = {(uint32_t)f.kind, f.function, f.block, f.instruction,
            f.value, f.argument, f.edge, f.target_block, f.target_value, f.type,
            f.array_element, (uint32_t)f.array_operation, (uint32_t)f.map_operation,
            (uint32_t)f.callable_transport,(uint32_t)f.callable_operation,f.callable_function,f.callable_scope,f.callable_materialize,
            (uint32_t)f.transfer_mode,(uint32_t)f.string_operation,(uint32_t)f.forwarding_mode,f.forwarding_drop};
        for (size_t p = 0; p < sizeof(fields) / sizeof(fields[0]); ++p)
            hash = hash * UINT64_C(16777619) + fields[p];
    }
    return hash;
}

static XrLirOwnershipPlan *constructed_plan(const XrLirModule *m,
    const XrLirValueLayoutKey *roots, size_t count) {
    XrtBillingOwner *billing=NULL; XrLirValueLayouts *layouts=NULL;
    CHECK(xrt_billing_owner_new(NULL,SIZE_MAX,&billing)==XRT_MANAGED_OK);
    size_t baseline=xrt_billing_owner_stats(billing).bytes;
    CHECK(xr_lir_value_layouts_create(m,roots,count,billing,&layouts)==XR_LIR_VALUE_LAYOUT_OK);
    const XrLirOwnershipTypeFact *types=NULL; uint32_t type_count=0;
    CHECK(xr_lir_value_layouts_ownership(layouts,&types,&type_count)==XR_LIR_VALUE_LAYOUT_OK);
    for (uint32_t i=0;i<type_count;++i) {
        const XrtValuePlan *received=NULL; XrtValuePlanSpec view;
        CHECK(xr_lir_value_layouts_get(layouts,(XrLirValueLayoutKey){types[i].type,0,0},&received)==XR_LIR_VALUE_LAYOUT_OK);
        CHECK(xrt_value_plan_view(received,&view)==XRT_MANAGED_OK && types[i].has_owned==(view.owned_count!=0));
    }
    XrLirOwnershipPlan *plan=NULL;
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){m,types,type_count},&plan,NULL));
    xrt_billing_owner_close(billing);
    xr_lir_value_layouts_destroy(layouts);
    CHECK(xrt_billing_owner_stats(billing).bytes==baseline && xrt_billing_owner_stats(billing).blocks==1);
    xrt_billing_owner_drop(billing);
    return plan;
}

static uint32_t typed_count(const XrLirOwnershipPlan *plan,XrLirOwnershipFactKind kind,XrLirType type) {
    uint32_t count=0;
    for (uint32_t i=0;i<xr_lir_ownership_fact_count(plan);++i) {
        XrLirOwnershipFact fact; OK(xr_lir_ownership_fact_at(plan,i,&fact));
        count+=fact.kind==kind&&fact.type==type;
    }
    return count;
}

static void arrays(XrLirType element) {
    XrLirModule *m = NULL;
    XrLirOwnershipPlan *plan = NULL;
    OK(array_fixture_create(element, &m));
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=m}, &plan, NULL));
    XrLirType forbidden = 0;
    CHECK(xr_lir_nullable_type(m,XR_LIR_I64,&forbidden) == XR_LIR_INVALID_STATE && !forbidden);
    compare_internal(&(XrLirOwnershipRequest){.module=m},plan);
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
    CHECK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=m}, &plan, NULL) == XR_LIR_INVALID_ARGUMENT && plan == saved);
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
        OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=m}, &plan, NULL));
        compare_internal(&(XrLirOwnershipRequest){.module=m},plan);
        CHECK(count_kind(plan, XR_LIR_OWNERSHIP_PARAMETER_BORROW) == 2 &&
            count_kind(plan, XR_LIR_OWNERSHIP_RESULT_OWNER) == 2 &&
            count_kind(plan, XR_LIR_OWNERSHIP_RELEASE) == 1 &&
            count_kind(plan, XR_LIR_OWNERSHIP_RETURN_TRANSFER) == 1);
        if (types[t] == XR_LIR_STRING) {
            XrLirModule *elided = NULL;
            XrLirOwnershipPlan *final = NULL;
            OK(xr_lir_string_elide(m, &elided));
            OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=elided}, &final, NULL));
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
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=m}, &plan, NULL));
    CHECK(count_kind(plan, XR_LIR_OWNERSHIP_APPEND_TRANSFER) == 1 &&
        count_kind(plan, XR_LIR_OWNERSHIP_OPERAND_BORROW) == 2 &&
        !count_kind(plan, XR_LIR_OWNERSHIP_RELEASE));
    xr_lir_module_destroy(m);
    xr_lir_ownership_plan_destroy(plan);
    m = NULL; plan = NULL;
    OK(ownership_fixture(XR_LIR_STRING, false, 24, &m));
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=m}, &plan, NULL));
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
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=m}, &plan, NULL));
    XrLirOwnershipFunction view;
    OK(xr_lir_ownership_function_at(plan, 2, &view));
    CHECK(view.unmodeled == XR_LIR_OWNERSHIP_UNMODELED_REFERENCE);
    xr_lir_module_destroy(m);
    xr_lir_ownership_plan_destroy(plan);
    m = NULL; plan = NULL;
    OK(array_repeat_fixture(XR_LIR_U64, &m));
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=m}, &plan, NULL));
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
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=input}, &old, NULL));
    CHECK(count_kind(old, XR_LIR_OWNERSHIP_SELECTED_EDGE_TRANSFER) == 2 &&
        count_kind(old, XR_LIR_OWNERSHIP_RETURN_TRANSFER) == 2);
    uint32_t old_owner = owned.id, old_return = parameters[1].id;
    uint64_t old_digest = digest(old);
    OK(xr_lir_sccp(input, &rebuilt));
    CHECK(rebuilt && rebuilt != input);
    xr_lir_module_destroy(input);
    CHECK(digest(old) == old_digest);
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=rebuilt}, &final, NULL));
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
    CHECK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=m}, &plan, &captured) == XR_LIR_VERIFY_ERROR && !plan);
    CHECK(direct.function == captured.function && direct.block == captured.block &&
        direct.instruction == captured.instruction && !strcmp(direct.message, captured.message));
}

static void malformed(void) {
    XrLirModule *m = NULL;
    XrLirOwnershipPlan *plan = NULL;
    OK(xr_lir_module_create(NULL, &m));
    CHECK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=m}, &plan, NULL) == XR_LIR_INVALID_ARGUMENT && !plan);
    xr_lir_module_destroy(m);
    m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    OK(xr_lir_module_finish(m, NULL));
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=m}, &plan, NULL));
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
    m=NULL;
    OK(ownership_fixture(XR_LIR_STRING,false,2,&m));
    CHECK(!m->type_count);
    XrLirType original=m->functions[0].values[0].type;
    XrLirType invalid[]={XR_LIR_TYPE_COUNT,UINT32_MAX};
    for (size_t i=0;i<sizeof(invalid)/sizeof(*invalid);++i) {
        m->functions[0].values[0].type=invalid[i];
        reject(m);
        XrLirDiagnostic diagnostic;
        CHECK(xr_lir_verify(m,&diagnostic)==XR_LIR_VERIFY_ERROR&&strstr(diagnostic.message,"SSA value type is invalid"));
    }
    m->functions[0].values[0].type=original; OK(xr_lir_verify(m,NULL)); xr_lir_module_destroy(m);
    puts("invalid constructed value ordinals reject before ownership metadata lookup");
    CHECK(!xr_lir_ownership_function_count(NULL) && !xr_lir_ownership_fact_count(NULL));
    xr_lir_ownership_plan_destroy(NULL);
}

static void boxed_lifetimes(void) {
    XrLirFunction root={0};
    XrLirModule *m=record_fixture(&root);
    XrLirFunctionData *f=&m->functions[root.id-1];
    XrLirInstructionView *field=&f->blocks[0].instructions[8].view;
    CHECK(field->op==XR_LIR_RECORD_FIELD);
    uint32_t *operand=(uint32_t *)field->operands, saved=*operand;
    *operand=f->blocks[0].instructions[4].view.result;
    XrLirDiagnostic diagnostic;
    CHECK(xr_lir_verify(m,&diagnostic)==XR_LIR_VERIFY_ERROR &&
        strstr(diagnostic.message,"Managed use follows release"));
    *operand=saved; OK(xr_lir_verify(m,NULL));
    XrLirFunctionData *helper=&m->functions[1];
    XrLirInstructionView *ret=&helper->blocks[0].instructions[1].view;
    CHECK(ret->op==XR_LIR_RET);
    operand=(uint32_t *)ret->operands; saved=*operand; *operand=helper->parameters[0];
    CHECK(xr_lir_verify(m,&diagnostic)==XR_LIR_VERIFY_ERROR &&
        strstr(diagnostic.message,"Managed transfer requires an owned reference"));
    *operand=saved; OK(xr_lir_verify(m,NULL));
    XrLirOwnershipPlan *plan=NULL;
    CHECK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=m},&plan,&diagnostic)==XR_LIR_INVALID_ARGUMENT &&
        !plan && strstr(diagnostic.message,"constructed ownership facts are missing"));
    xr_lir_module_destroy(m);
    puts("boxed use-after-release and borrowed return reject in the common verifier");
}

static void boxed_selected_edges(void) {
    XrLirModule *m=NULL; OK(xr_lir_module_create(NULL,&m));
    XrLirType type; XrLirRecordField field={"value",5,XR_LIR_STRING, 0};
    OK(xr_lir_record_type(m,
        &(XrLirRecordIdentity){.module="tests.lir.ownership", .module_length=sizeof("tests.lir.ownership")-1,
            .name="SelectedRecord", .name_length=sizeof("SelectedRecord")-1},
        &field,1,&type));
    XrLirType parameters[]={type,XR_LIR_I1};
    XrLirFunction function={0}; XrLirFunctionSpec spec={.name="record_edges",.name_length=12,
        .parameters=parameters,.parameter_count=2,.result=type};
    OK(xr_lir_declare_function(m,&spec,&function)); OK(xr_lir_define_function(m,function));
    XrLirBlock blocks[3]={{0}}; XrLirValue input={0},condition={0},owners[2]={{0}};
    OK(xr_lir_add_block(m,function,NULL,0,&blocks[0]));
    OK(xr_lir_set_entry(m,function,blocks[0]));
    OK(xr_lir_function_parameter(m,function,0,&input)); OK(xr_lir_function_parameter(m,function,1,&condition));
    for (uint32_t i=0;i<2;++i) {
        XrLirInstruction copy={.op=XR_LIR_VALUE_RETAIN,.type=type,.operands=&input,.operand_count=1};
        OK(xr_lir_emit_value(m,blocks[0],&copy,&owners[i]));
    }
    XrLirType block_types[]={type,type};
    for (uint32_t i=1;i<3;++i) {
        XrLirValue first={0},second={0};
        OK(xr_lir_add_block(m,function,block_types,2,&blocks[i]));
        OK(xr_lir_block_parameter(m,blocks[i],0,&first)); OK(xr_lir_block_parameter(m,blocks[i],1,&second));
        XrLirInstruction drop={.op=XR_LIR_VALUE_DROP,.type=XR_LIR_UNIT,.operands=&first,.operand_count=1};
        OK(xr_lir_emit_effect(m,blocks[i],&drop));
        XrLirInstruction ret={.op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=&second,.operand_count=1};
        OK(xr_lir_set_terminator(m,blocks[i],&ret));
    }
    XrLirValue reverse[]={owners[1],owners[0]};
    XrLirInstruction branch={.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,.operands=&condition,.operand_count=1,.edge_count=2,
        .edges={{blocks[1],owners,2},{blocks[2],reverse,2}}};
    OK(xr_lir_set_terminator(m,blocks[0],&branch)); OK(xr_lir_module_finish(m,NULL));
    XrLirValueLayoutKey key={type,0,0};
    XrLirOwnershipPlan *plan=constructed_plan(m,&key,1);
    CHECK(typed_count(plan,XR_LIR_OWNERSHIP_PARAMETER_BORROW,type)==1 &&
        typed_count(plan,XR_LIR_OWNERSHIP_RESULT_OWNER,type)==2 &&
        typed_count(plan,XR_LIR_OWNERSHIP_BLOCK_OWNER,type)==4 &&
        typed_count(plan,XR_LIR_OWNERSHIP_SELECTED_EDGE_TRANSFER,type)==4 &&
        typed_count(plan,XR_LIR_OWNERSHIP_RELEASE,type)==2 &&
        typed_count(plan,XR_LIR_OWNERSHIP_RETURN_TRANSFER,type)==2);
    for (uint32_t i=0;i<xr_lir_ownership_fact_count(plan);++i) {
        XrLirOwnershipFact fact; OK(xr_lir_ownership_fact_at(plan,i,&fact)); CHECK(fact.type==type&&!fact.array_element);
        if (fact.kind!=XR_LIR_OWNERSHIP_SELECTED_EDGE_TRANSFER) continue;
        CHECK(fact.block==1&&fact.instruction==3&&fact.edge>=1&&fact.edge<=2&&fact.argument<2);
        const XrLirEdgeView *edge=&m->functions[0].blocks[0].instructions[2].view.edges[fact.edge-1];
        CHECK(fact.value==edge->arguments[fact.argument]&&fact.target_block==edge->target&&
            fact.target_value==m->functions[0].blocks[edge->target-1].parameters[fact.argument]);
    }
    uint64_t before=digest(plan);
    XrLirInstructionView *term=&m->functions[0].blocks[0].instructions[2].view;
    uint32_t *arguments=(uint32_t *)term->edges[0].arguments, saved=arguments[1]; arguments[1]=arguments[0];
    XrLirDiagnostic diagnostic;
    CHECK(xr_lir_verify(m,&diagnostic)==XR_LIR_VERIFY_ERROR&&strstr(diagnostic.message,"edge duplicates"));
    arguments[1]=saved;
    arguments=(uint32_t *)term->edges[1].arguments; saved=arguments[0]; arguments[0]=input.id;
    CHECK(xr_lir_verify(m,&diagnostic)==XR_LIR_VERIFY_ERROR&&strstr(diagnostic.message,"edge requires a distinct owned reference"));
    arguments[0]=saved; OK(xr_lir_verify(m,NULL)); xr_lir_module_destroy(m);
    CHECK(digest(plan)==before); xr_lir_ownership_plan_destroy(plan);
    puts("boxed selected edges preserve distinct real owners and reject borrowed transfers");
}

static void constructed_snapshots(void) {
    XrLirFunction root={0}; XrLirModule *m=record_fixture(&root);
    XrLirType record=m->functions[1].result, optional=m->types[record-XR_LIR_TYPE_COUNT].fields[0].type;
    XrLirValueLayoutKey key={record,0,0};
    XrLirOwnershipPlan *plan=constructed_plan(m,&key,1);
    CHECK(typed_count(plan,XR_LIR_OWNERSHIP_PARAMETER_BORROW,record)==1 &&
        typed_count(plan,XR_LIR_OWNERSHIP_RESULT_OWNER,record)==3 &&
        typed_count(plan,XR_LIR_OWNERSHIP_RELEASE,record)==2 &&
        typed_count(plan,XR_LIR_OWNERSHIP_RETURN_TRANSFER,record)==1);
    CHECK(typed_count(plan,XR_LIR_OWNERSHIP_RESULT_OWNER,optional)==2 &&
        typed_count(plan,XR_LIR_OWNERSHIP_RELEASE,optional)==2);
    for (uint32_t function=1;function<=2;++function) {
        XrLirOwnershipFunction view; OK(xr_lir_ownership_function_at(plan,function,&view)); CHECK(!view.unmodeled);
    }
    uint64_t before=digest(plan); xr_lir_module_destroy(m); CHECK(digest(plan)==before);
    xr_lir_ownership_plan_destroy(plan);
    puts("Record and managed Nullable facts survive layout, billing and final IR retirement");
}

static void constructed_requests(void) {
    XrLirFunction root={0}; XrLirModule *m=record_fixture(&root);
    XrLirValueLayoutKey key={m->functions[1].result,0,0};
    XrtBillingOwner *billing=NULL; XrLirValueLayouts *layouts=NULL;
    CHECK(xrt_billing_owner_new(NULL,SIZE_MAX,&billing)==XRT_MANAGED_OK);
    CHECK(xr_lir_value_layouts_create(m,&key,1,billing,&layouts)==XR_LIR_VALUE_LAYOUT_OK);
    const XrLirOwnershipTypeFact *types=NULL; uint32_t count=0;
    CHECK(xr_lir_value_layouts_ownership(layouts,&types,&count)==XR_LIR_VALUE_LAYOUT_OK&&count==2);
    XrLirOwnershipTypeFact changed[2]={types[0],types[1]};
    XrLirOwnershipRequest request={m,changed,2}; XrLirOwnershipPlan *plan=NULL;
    changed[1]=changed[0]; CHECK(xr_lir_ownership_plan_create(&request,&plan,NULL)==XR_LIR_INVALID_ARGUMENT&&!plan);
    changed[1]=types[1]; changed[0].type=XR_LIR_I64;
    CHECK(xr_lir_ownership_plan_create(&request,&plan,NULL)==XR_LIR_INVALID_ARGUMENT&&!plan);
    changed[0]=types[0]; changed[1].type=XR_LIR_TYPE_COUNT+xr_lir_constructed_type_count(m);
    CHECK(xr_lir_ownership_plan_create(&request,&plan,NULL)==XR_LIR_INVALID_ARGUMENT&&!plan);
    changed[1]=types[1]; request.type_count=1;
    CHECK(xr_lir_ownership_plan_create(&request,&plan,NULL)==XR_LIR_INVALID_ARGUMENT&&!plan);
    request.type_count=2; OK(xr_lir_ownership_plan_create(&request,&plan,NULL));
    uint64_t before=digest(plan); memset(changed,0xa5,sizeof(changed));
    xr_lir_value_layouts_destroy(layouts); xrt_billing_owner_drop(billing); xr_lir_module_destroy(m);
    CHECK(digest(plan)==before); xr_lir_ownership_plan_destroy(plan);
    puts("constructed facts reject duplicate, illegal and missing rows and own their input vector");
}

static void pod_function(XrLirModule *m,XrLirType type,const char *name) {
    XrLirFunction f={0}; XrLirFunctionSpec spec={.name=name,.name_length=(uint32_t)strlen(name),
        .parameters=&type,.parameter_count=1,.result=type};
    OK(xr_lir_declare_function(m,&spec,&f)); OK(xr_lir_define_function(m,f));
    XrLirBlock block={0}; XrLirValue input={0},result={0};
    OK(xr_lir_add_block(m,f,NULL,0,&block)); OK(xr_lir_set_entry(m,f,block)); OK(xr_lir_function_parameter(m,f,0,&input));
    result=input;
    if (xr_lir_boxed_type(m,type)) {
        XrLirInstruction copy={.op=XR_LIR_VALUE_RETAIN,.type=type,.operands=&input,.operand_count=1};
        OK(xr_lir_emit_value(m,block,&copy,&result));
    }
    XrLirInstruction ret={.op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=&result,.operand_count=1};
    OK(xr_lir_set_terminator(m,block,&ret));
}

static void pod_facts(void) {
    XrLirModule *m=NULL; OK(xr_lir_module_create(NULL,&m));
    XrLirType record,optional,integer_optional; XrLirRecordField field={"score",5,XR_LIR_I64, 0};
    OK(xr_lir_record_type(m,
        &(XrLirRecordIdentity){.module="tests.lir.ownership", .module_length=sizeof("tests.lir.ownership")-1,
            .name="PodRecord", .name_length=sizeof("PodRecord")-1},
        &field,1,&record)); OK(xr_lir_nullable_type(m,record,&optional));
    OK(xr_lir_nullable_type(m,XR_LIR_I64,&integer_optional));
    pod_function(m,record,"pod_record"); pod_function(m,optional,"pod_record_optional"); pod_function(m,integer_optional,"pod_i64_optional");
    OK(xr_lir_module_finish(m,NULL));
    XrLirValueLayoutKey keys[]={{record,0,0},{optional,0,0},{integer_optional,0,0}};
    XrLirOwnershipPlan *plan=constructed_plan(m,keys,3); CHECK(!xr_lir_ownership_fact_count(plan));
    for (uint32_t i=1;i<=3;++i) {
        XrLirOwnershipFunction view; OK(xr_lir_ownership_function_at(plan,i,&view)); CHECK(!view.unmodeled&&!view.fact_count);
    }
    XrLirInstructionView *ret=&m->functions[0].blocks[0].instructions[1].view;
    uint32_t *operand=(uint32_t *)ret->operands,saved=*operand; *operand=m->functions[0].parameters[0];
    XrLirDiagnostic diagnostic;
    CHECK(xr_lir_verify(m,&diagnostic)==XR_LIR_VERIFY_ERROR&&strstr(diagnostic.message,"Managed transfer requires"));
    *operand=saved; OK(xr_lir_verify(m,NULL)); xr_lir_module_destroy(m);
    CHECK(!xr_lir_ownership_fact_count(plan)); xr_lir_ownership_plan_destroy(plan);
    puts("POD Record and Nullable emit no RC events while boxed IR lifetime remains checked");
}

static void unmodeled_constructed(void) {
    XrLirModule *m=NULL; OK(xr_lir_module_create(NULL,&m));
    XrLirType callable,cell,fixed;
    OK(xr_lir_callable_type(m,&(XrLirCallableType){.result=XR_LIR_I64,.flags=XR_LIR_CALLABLE_NO_SUSPEND},&callable));
    OK(xr_lir_cell_type(m,(XrLirCellType){XR_LIR_I64,0},&cell)); OK(xr_lir_fixed_array_type(m,XR_LIR_U8,4,&fixed));
    XrLirType parameters[]={callable,cell,fixed}; XrLirFunction function={0};
    XrLirFunctionSpec spec={.name="unmodeled",.name_length=9,.parameters=parameters,.parameter_count=3,.result=XR_LIR_UNIT};
    OK(xr_lir_declare_function(m,&spec,&function)); OK(xr_lir_define_function(m,function));
    XrLirBlock block={0}; OK(xr_lir_add_block(m,function,NULL,0,&block)); OK(xr_lir_set_entry(m,function,block));
    XrLirInstruction ret={.op=XR_LIR_RET,.type=XR_LIR_UNIT}; OK(xr_lir_set_terminator(m,block,&ret)); OK(xr_lir_module_finish(m,NULL));
    XrLirOwnershipPlan *plan=NULL; OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=m},&plan,NULL));
    XrLirOwnershipFunction view; OK(xr_lir_ownership_function_at(plan,1,&view));
    CHECK(view.unmodeled==(XR_LIR_OWNERSHIP_UNMODELED_CELL|XR_LIR_OWNERSHIP_UNMODELED_CALLABLE|XR_LIR_OWNERSHIP_UNMODELED_CONSTRUCTED));
    xr_lir_module_destroy(m); xr_lir_ownership_plan_destroy(plan);
}
static void string_bindings(void) {
    XrLirModule *module=NULL; OK(string_ref_fixture(&module));
    XrLirOwnershipRequest request={.module=module}; XrLirOwnershipPlan *plan=NULL;
    OK(xr_lir_ownership_plan_create(&request,&plan,NULL)); compare_internal(&request,plan);
    CHECK(count_kind(plan,XR_LIR_OWNERSHIP_STRING_OPERATION)==1 &&
        count_kind(plan,XR_LIR_OWNERSHIP_BINDING_NEW)==1 &&
        count_kind(plan,XR_LIR_OWNERSHIP_BINDING_BORROW)==2 &&
        count_kind(plan,XR_LIR_OWNERSHIP_BINDING_READ)==1 &&
        count_kind(plan,XR_LIR_OWNERSHIP_BINDING_RELEASE)==1);
    XrLirOwnershipFunction helper; OK(xr_lir_ownership_function_at(plan,2,&helper)); CHECK(!helper.unmodeled);
    bool parameter=false,append=false;
    for (uint32_t n=0;n<xr_lir_ownership_fact_count(plan);++n) {
        XrLirOwnershipFact fact; OK(xr_lir_ownership_fact_at(plan,n,&fact));
        if (fact.function!=2) continue;
        if (fact.kind==XR_LIR_OWNERSHIP_PARAMETER_BORROW) {
            CHECK(fact.type==XR_LIR_STRING_REF && !fact.block && !fact.instruction && !fact.argument &&
                fact.transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_BORROW); parameter=true;
        }
        if (fact.kind==XR_LIR_OWNERSHIP_STRING_OPERATION) {
            CHECK(fact.type==XR_LIR_STRING_REF && fact.string_operation==XR_LIR_STRING_OP_APPEND_REF &&
                fact.value && fact.target_value && fact.block==1 && fact.instruction==2 &&
                fact.transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_BORROW); append=true;
        }
    }
    CHECK(parameter && append);
    uint64_t before=digest(plan); xr_lir_module_destroy(module); CHECK(digest(plan)==before);
    xr_lir_ownership_plan_destroy(plan);
}

#include "lir_array_forwarding_cases.h"
#include "lir_array_set_region_cases.h"
int main(void) {
    array_forwarding_preparation();
    array_set_region_preparations();
    XrLirType elements[] = {XR_LIR_I64, XR_LIR_STRING, XR_LIR_U8, XR_LIR_U64};
    for (uint32_t i = 0; i < 4; ++i) arrays(elements[i]);
    copies(); other_shapes(); rebuilt_fact_identity(); malformed(); boxed_lifetimes(); boxed_selected_edges();
    constructed_snapshots(); constructed_requests(); pod_facts(); unmodeled_constructed(); string_bindings();
    puts("LIR ownership snapshots passed");
    return 0;
}
