/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_ownership_faults.c - Verifier and snapshot publication failure sweep
 */
#include "base/xmalloc.h"
#include "lir/xlir_ownership.h"
#include "lir/xlir_value_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define OK(c) CHECK((c) == XR_LIR_OK)

static size_t attempt, fail_at = SIZE_MAX, live, live_bytes;
typedef union OwnershipAllocation {
    long double alignment;
    void *pointer;
    size_t bytes;
} OwnershipAllocation;

static void *ownership_allocate(size_t bytes) {
    if (attempt++ == fail_at || bytes > SIZE_MAX - sizeof(OwnershipAllocation)) return NULL;
    OwnershipAllocation *allocation = xr_malloc(sizeof(*allocation) + bytes);
    if (!allocation) return NULL;
    allocation->bytes = bytes;
    ++live; live_bytes += bytes;
    return allocation + 1;
}
static void *ownership_callocate(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) return NULL;
    void *memory = ownership_allocate(count * size);
    if (memory) memset(memory, 0, count * size);
    return memory;
}
static void ownership_free(void *memory) {
    if (!memory) return;
    OwnershipAllocation *allocation = (OwnershipAllocation *)memory - 1;
    CHECK(live && live_bytes >= allocation->bytes);
    --live; live_bytes -= allocation->bytes;
    xr_free(allocation);
}
#undef xr_malloc
#undef xr_calloc
#undef xr_free
#define xr_malloc(bytes) ownership_allocate(bytes)
#define xr_calloc(count, size) ownership_callocate(count, size)
#define xr_free(memory) ownership_free(memory)
#include "lir/xlir_verify.c"
#include "lir_array_fixture.h"
#include "lir_ownership_fixture.h"
#include "lir_record_fixture.h"
#include "lir_slice_ownership_fixture.h"
#include "lir_slice_callable_fixture.h"
#include "lir_slice_cleanup_fixture.h"
#include "lir_array_forwarding_fixture.h"
#include "lir_array_set_region_fixture.h"

static void sweep(XrLirModule *input, const XrLirOwnershipTypeFact *types, uint32_t type_count) {
    XrLirOwnershipPlan *output = NULL;
    XrLirOwnershipRequest request={input,types,type_count};
    size_t input_count = live, input_bytes = live_bytes;
    CHECK(input_count && input_bytes);
    attempt = 0;
    OK(xr_lir_verify(input, NULL));
    size_t verification = attempt;
    CHECK(live == input_count && live_bytes == input_bytes);
    attempt = 0;
    OK(xr_lir_ownership_plan_create(&request, &output, NULL));
    size_t count = attempt;
    CHECK(count > verification && live > input_count && live_bytes > input_bytes);
    xr_lir_ownership_plan_destroy(output); output = NULL;
    CHECK(live == input_count && live_bytes == input_bytes);
    for (size_t point = 0; point < count; ++point) {
        attempt = 0; fail_at = point;
        XrLirDiagnostic diagnostic;
        CHECK(xr_lir_ownership_plan_create(&request, &output, &diagnostic) == XR_LIR_OUT_OF_MEMORY);
        CHECK(!output && diagnostic.status == XR_LIR_OUT_OF_MEMORY && attempt > point &&
            live == input_count && live_bytes == input_bytes);
        fail_at = SIZE_MAX;
        OK(xr_lir_verify(input, NULL));
        CHECK(live == input_count && live_bytes == input_bytes);
    }
    OK(xr_lir_ownership_plan_create(&request, &output, NULL));
    size_t occupied_count = live, occupied_bytes = live_bytes;
    CHECK(occupied_count > input_count && occupied_bytes > input_bytes);
    XrLirOwnershipPlan *saved = output;
    uint32_t regions = xr_lir_ownership_array_region_count(output);
    uint32_t region_steps = xr_lir_ownership_array_region_step_count(output);
    attempt = 0; fail_at = 0;
    CHECK(xr_lir_ownership_plan_create(&request, &output, NULL) == XR_LIR_INVALID_ARGUMENT &&
        output == saved && !attempt && live == occupied_count && live_bytes == occupied_bytes);
    fail_at = SIZE_MAX;
    xr_lir_module_destroy(input);
    if (regions) {
        XrLirArraySetRegion region; XrLirArraySetRegionStep step;
        OK(xr_lir_ownership_array_region_at(output, regions - 1, &region));
        OK(xr_lir_ownership_array_region_step_at(output, region_steps - 1, &step));
        CHECK(region.function && region.step_count && step.kind == XR_LIR_ARRAY_SET_REGION_EXIT_MOVE);
    }
    XrLirOwnershipFact fact;
    OK(xr_lir_ownership_fact_at(output, 0, &fact));
    CHECK(fact.kind && live == occupied_count - input_count &&
        live_bytes == occupied_bytes - input_bytes);
    xr_lir_ownership_plan_destroy(output);
    CHECK(!live && !live_bytes);
    printf("ownership snapshot failure points: %zu (verifier %zu)\n", count, verification);
}

static void rejected_after_collection(void) {
    XrLirModule *m = NULL;
    OK(ownership_fixture(XR_LIR_STRING, false, 2, &m));
    size_t input_count = live, input_bytes = live_bytes;
    CHECK(input_count && input_bytes);
    XrLirInstructionView *ret = &m->functions[0].blocks[0].instructions[3].view;
    uint32_t *operand = (uint32_t *)ret->operands;
    uint32_t saved = *operand;
    *operand = m->functions[0].parameters[0];
    XrLirOwnershipPlan *plan = NULL;
    XrLirDiagnostic diagnostic;
    CHECK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=m}, &plan, &diagnostic) == XR_LIR_VERIFY_ERROR &&
        !plan && live == input_count && live_bytes == input_bytes);
    *operand = saved;
    OK(xr_lir_verify(m, NULL));
    CHECK(live == input_count && live_bytes == input_bytes);
    xr_lir_module_destroy(m);
    CHECK(!live && !live_bytes);
}
static void array_region_tampering(void) {
    ArraySetRegionFixture c = array_set_region_fixture(3, ARRAY_SET_REGION_LOCAL);
    XrLirOwnershipPlan *plan = NULL;
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=c.module}, &plan, NULL));
    CHECK(plan->array_regions.regions.count == 1 && plan->array_regions.steps.count);
    size_t baseline = live, bytes = live_bytes;
    XrLirOwnershipFact *move = NULL;
    for (uint32_t n = 0; n < plan->fact_count; ++n)
        if (plan->facts[n].kind == XR_LIR_OWNERSHIP_SELECTED_EDGE_TRANSFER && plan->facts[n].type == XR_LIR_ARRAY) {
            move = &plan->facts[n]; break;
        }
    CHECK(move && move->transfer_mode == XR_LIR_OWNERSHIP_TRANSFER_MOVE);
    move->transfer_mode = XR_LIR_OWNERSHIP_TRANSFER_COPY;
    CHECK(xr_lir_ownership_array_regions_verify(c.module, plan) == XR_LIR_VERIFY_ERROR &&
        live == baseline && live_bytes == bytes);
    move->transfer_mode = XR_LIR_OWNERSHIP_TRANSFER_MOVE;
    XrLirArraySetRegionStep *steps = plan->array_regions.steps.data;
    XrLirArraySetRegionStepKind saved = steps[0].kind; steps[0].kind = XR_LIR_ARRAY_SET_REGION_EXIT_MOVE;
    CHECK(xr_lir_ownership_array_regions_verify(c.module, plan) == XR_LIR_VERIFY_ERROR &&
        live == baseline && live_bytes == bytes);
    steps[0].kind = saved; OK(xr_lir_ownership_array_regions_verify(c.module, plan));
    CHECK(live == baseline && live_bytes == bytes);
    xr_lir_ownership_plan_destroy(plan); xr_lir_module_destroy(c.module); CHECK(!live && !live_bytes);
}
static void array_region_input_equal(const XrLirModule *input, const XrLirModule *reference) {
    CHECK(input->function_count == reference->function_count);
    for (uint32_t f = 0; f < input->function_count; ++f) {
        const XrLirFunctionData *a = &input->functions[f], *b = &reference->functions[f];
        CHECK(a->entry == b->entry && a->value_count == b->value_count && a->block_count == b->block_count);
        for (uint32_t v = 0; v < a->value_count; ++v)
            CHECK(a->values[v].type == b->values[v].type && a->values[v].block == b->values[v].block &&
                a->values[v].instruction == b->values[v].instruction && a->values[v].parameter == b->values[v].parameter &&
                a->values[v].array_element == b->values[v].array_element);
        for (uint32_t n = 0; n < a->block_count; ++n) {
            const XrLirBlockData *x = &a->blocks[n], *y = &b->blocks[n];
            CHECK(x->parameter_count == y->parameter_count && x->instruction_count == y->instruction_count);
            for (uint32_t p = 0; p < x->parameter_count; ++p) CHECK(x->parameters[p] == y->parameters[p]);
            for (uint32_t i = 0; i < x->instruction_count; ++i) {
                const XrLirInstructionView *u = &x->instructions[i].view, *v = &y->instructions[i].view;
                CHECK(u->op == v->op && u->type == v->type && u->predicate == v->predicate && u->result == v->result &&
                    u->callee == v->callee && u->intrinsic == v->intrinsic && u->bits == v->bits &&
                    u->operand_count == v->operand_count && u->edge_count == v->edge_count);
                for (uint32_t o = 0; o < u->operand_count; ++o) CHECK(u->operands[o] == v->operands[o]);
                for (uint32_t e = 0; e < u->edge_count; ++e) {
                    CHECK(u->edges[e].target == v->edges[e].target &&
                        u->edges[e].argument_count == v->edges[e].argument_count);
                    for (uint32_t o = 0; o < u->edges[e].argument_count; ++o)
                        CHECK(u->edges[e].arguments[o] == v->edges[e].arguments[o]);
                }
            }
        }
    }
}
static void array_region_verification_faults(void) {
    ArraySetRegionFixture input = array_set_region_build(NULL, 3, ARRAY_SET_REGION_LOCAL);
    input = array_set_region_build(input.module, 6, ARRAY_SET_REGION_LOCAL);
    ArraySetRegionFixture reference = array_set_region_build(NULL, 3, ARRAY_SET_REGION_LOCAL);
    reference = array_set_region_build(reference.module, 6, ARRAY_SET_REGION_LOCAL);
    OK(xr_lir_module_finish(input.module, NULL)); OK(xr_lir_module_finish(reference.module, NULL));
    XrLirOwnershipPlan *plan = NULL, *saved = NULL;
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=input.module}, &plan, NULL));
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=reference.module}, &saved, NULL));
    CHECK(plan->array_regions.regions.count == 2 && lir_array_region_plans_equal(plan, saved));
    size_t baseline = live, bytes = live_bytes;
    attempt = 0; OK(xr_lir_ownership_array_regions_verify(input.module, plan));
    size_t count = attempt; CHECK(count && live == baseline && live_bytes == bytes);
    for (size_t point = 0; point < count; ++point) {
        attempt = 0; fail_at = point;
        CHECK(xr_lir_ownership_array_regions_verify(input.module, plan) == XR_LIR_OUT_OF_MEMORY && attempt > point &&
            live == baseline && live_bytes == bytes);
        fail_at = SIZE_MAX;
        CHECK(lir_array_region_plans_equal(plan, saved)); array_region_input_equal(input.module, reference.module);
        OK(xr_lir_ownership_array_regions_verify(input.module, plan));
        CHECK(live == baseline && live_bytes == bytes);
    }
    xr_lir_ownership_plan_destroy(plan); xr_lir_ownership_plan_destroy(saved);
    xr_lir_module_destroy(input.module); xr_lir_module_destroy(reference.module);
    CHECK(!live && !live_bytes); printf("array region verification failure points: %zu\n", count);
}

int main(void) {
    attempt = 0;
    CHECK(!lir_plan_allocate(SIZE_MAX, 2) && !attempt && !live && !live_bytes);
    XrLirType elements[] = {XR_LIR_I64, XR_LIR_STRING, XR_LIR_U8, XR_LIR_U64};
    for (uint32_t i = 0; i < 4; ++i) {
        XrLirModule *input = NULL;
        OK(array_fixture_create(elements[i], &input));
        sweep(input,NULL,0);
    }
    for (uint32_t i = 0; i < 3; ++i) {
        XrLirModule *input = NULL;
        OK(ownership_fixture(i == 1 ? XR_LIR_MAP_STRING_I64 : XR_LIR_STRING, i == 2, 2, &input));
        sweep(input,NULL,0);
    }
    for (uint32_t i = 0; i < 2; ++i) {
        XrLirModule *input = NULL; OK(slice_ownership_fixture(i ? XR_LIR_U8 : XR_LIR_I64, &input));
        sweep(input,NULL,0);
    }
    ArrayForwardFixture forwarding=array_forward_fixture(XR_LIR_I64,ARRAY_FORWARD_LOCAL);
    sweep(forwarding.module,NULL,0);
    ArraySetRegionFixture region=array_set_region_build(NULL,3,ARRAY_SET_REGION_LOCAL);
    region=array_set_region_build(region.module,6,ARRAY_SET_REGION_LOCAL);
    OK(xr_lir_module_finish(region.module,NULL));
    sweep(region.module,NULL,0);
    array_region_tampering();
    array_region_verification_faults();
    XrLirModule *grown = NULL;
    OK(ownership_fixture(XR_LIR_STRING, false, 24, &grown));
    sweep(grown,NULL,0);
    XrLirFunction record_root={0}; XrLirModule *record=record_fixture(&record_root);
    XrtBillingOwner *billing=NULL; XrLirValueLayouts *layouts=NULL;
    CHECK(xrt_billing_owner_new(NULL,SIZE_MAX,&billing)==XRT_MANAGED_OK);
    XrLirValueLayoutKey key={record->functions[1].result,0,0};
    CHECK(xr_lir_value_layouts_create(record,&key,1,billing,&layouts)==XR_LIR_VALUE_LAYOUT_OK);
    const XrLirOwnershipTypeFact *types=NULL; uint32_t type_count=0;
    CHECK(xr_lir_value_layouts_ownership(layouts,&types,&type_count)==XR_LIR_VALUE_LAYOUT_OK&&type_count==2);
    sweep(record,types,type_count); xr_lir_value_layouts_destroy(layouts); xrt_billing_owner_drop(billing);
    rejected_after_collection();
    XrLirModule *callable=slice_callable_fixture(SLICE_CALLABLE_DIRECT);
    OK(xr_lir_module_finish(callable,NULL));sweep(callable,NULL,0);
    SliceCleanupFixture cleanup=slice_cleanup_fixture(XR_LIR_I64,SLICE_CLEANUP_BRANCH,false);
    OK(xr_lir_module_finish(cleanup.module,NULL));sweep(cleanup.module,NULL,0);
    puts("LIR ownership snapshot allocation failures passed");
    return 0;
}
