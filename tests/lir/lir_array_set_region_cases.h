/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_array_set_region_cases.h - Structural proof does not coalesce storage
 */
#include "lir_array_set_region_fixture.h"
#include "lir/xlir_value_homes.h"

static uint64_t ars_steps_digest(const XrLirArraySetRegionStep *steps, uint32_t count) {
    uint64_t hash = 0;
    for (uint32_t n = 0; n < count; ++n) {
        const XrLirArraySetRegionStep *s = &steps[n];
        uint32_t fields[] = {(uint32_t)s->kind,s->block,s->instruction,s->edge,s->source,s->target,s->drop};
        for (size_t v = 0; v < sizeof(fields) / sizeof(*fields); ++v) hash = hash * UINT64_C(16777619) + fields[v];
    }
    return hash;
}
static void ars_home_geometry(const XrLirValueHomes *homes) {
    for (uint32_t n = 0; n < homes->array_region_step_count; ++n) {
        XrLirArraySetRegionStep step = homes->array_region_steps[n];
        size_t source, target;
        if (step.kind == XR_LIR_ARRAY_SET_REGION_NEW_CANDIDATE) {
            source = homes->values[step.source].first_owner; target = homes->bindings[step.target].first_owner;
            CHECK(homes->bindings[step.target].kind == XR_LIR_HOME_BINDING_LOCAL);
        } else if (step.kind == XR_LIR_ARRAY_SET_REGION_READ_CANDIDATE) {
            source = homes->bindings[step.source].first_owner; target = homes->values[step.target].first_owner;
        } else {
            source = homes->values[step.source].first_owner; target = homes->values[step.target].first_owner;
        }
        CHECK(source < homes->frame.slot_count && target < homes->frame.slot_count && source != target &&
            homes->frame.owned_offsets[source] != homes->frame.owned_offsets[target]);
    }
}
static void ars_retirement(ArraySetRegionFixture c, XrLirOwnershipPlan *plan) {
    XrtBillingOwner *billing = NULL; CHECK(xrt_billing_owner_new(NULL, SIZE_MAX, &billing) == XRT_MANAGED_OK);
    XrLirValueLayoutKey roots[] = {{XR_LIR_UNIT,0,0},{XR_LIR_I1,0,0},{XR_LIR_I64,0,0},{XR_LIR_ARRAY,XR_LIR_I64,0}};
    XrLirValueLayouts *layouts = NULL;
    CHECK(xr_lir_value_layouts_create(c.module, roots, 4, billing, &layouts) == XR_LIR_VALUE_LAYOUT_OK);
    XrLirValueHomes *homes = NULL;
    CHECK(xr_lir_value_homes_create(&(XrLirValueHomesRequest){c.module,c.function,layouts,plan}, &homes) == XR_LIR_VALUE_LAYOUT_OK);
    CHECK(homes && homes->array_region_count == 1 && homes->array_regions[0].canonical_value == c.header_array.id);
    ars_home_geometry(homes);
    uint64_t before = ars_steps_digest(homes->array_region_steps, homes->array_region_step_count);
    uint32_t count = homes->array_region_step_count;
    xr_lir_module_destroy(c.module); xr_lir_ownership_plan_destroy(plan); xr_lir_value_layouts_destroy(layouts);
    CHECK(homes->array_regions[0].body_block_count == c.count && homes->array_region_step_count == count &&
        ars_steps_digest(homes->array_region_steps, count) == before);
    ars_home_geometry(homes); xr_lir_value_homes_destroy(homes);
    CHECK(xrt_billing_owner_stats(billing).blocks == 1); xrt_billing_owner_drop(billing);
}
static void ars_current_body(ArraySetRegionFixture c, const XrLirOwnershipPlan *plan) {
    XrLirFunctionData *f = &c.module->functions[c.function.id - 1];
    XrLirBlockData *latch = &f->blocks[c.body[c.count - 1].id - 1];
    XrLirInstructionView *increment = &latch->instructions[latch->instruction_count - 2].view;
    CHECK(increment->op == XR_LIR_ADD); increment->op = XR_LIR_DIV;
    OK(xr_lir_verify(c.module, NULL));
    CHECK(xr_lir_ownership_array_regions_verify(c.module, plan, c.function.id) == XR_LIR_VERIFY_ERROR);
    increment->op = XR_LIR_ADD;
    OK(xr_lir_ownership_array_regions_verify(c.module, plan, c.function.id));
}
static void ars_preparation(uint32_t blocks, enum ArraySetRegionMode mode) {
    ArraySetRegionFixture c = array_set_region_fixture(blocks, mode);
    XrLirOwnershipPlan *plan = NULL; XrLirOwnershipRequest request = {.module=c.module};
    OK(xr_lir_ownership_plan_create(&request, &plan, NULL)); compare_internal(&request, plan);
    bool expected = mode == ARRAY_SET_REGION_LOCAL || mode == ARRAY_SET_REGION_PADDED;
    CHECK(xr_lir_ownership_array_region_count(plan) == (expected ? 1u : 0u));
    OK(xr_lir_ownership_array_regions_verify(c.module, plan, c.function.id));
    XrLirOwnershipFunction row; OK(xr_lir_ownership_function_at(plan, c.function.id, &row));
    CHECK(row.region_count == (expected ? 1u : 0u));
    if (!expected) { xr_lir_module_destroy(c.module); xr_lir_ownership_plan_destroy(plan); return; }
    XrLirArraySetRegion region; OK(xr_lir_ownership_array_region_at(plan, row.region_offset, &region));
    CHECK(region.function == c.function.id && region.header == c.header.id && region.entry_block == c.entry.id &&
        region.latch == c.body[blocks - 1].id && region.exit_block == c.exit.id && region.body_block_count == blocks &&
        region.canonical_value == c.header_array.id && region.set_block == c.body[0].id && region.set_instruction);
    uint32_t kinds[6] = {0};
    for (uint32_t n = 0; n < region.step_count; ++n) {
        XrLirArraySetRegionStep step; OK(xr_lir_ownership_array_region_step_at(plan, region.first_step + n, &step));
        CHECK(step.kind > 0 && step.kind < 6 && step.source && step.target); ++kinds[step.kind];
        bool candidate = step.kind == XR_LIR_ARRAY_SET_REGION_NEW_CANDIDATE || step.kind == XR_LIR_ARRAY_SET_REGION_READ_CANDIDATE;
        CHECK(candidate ? step.drop == step.instruction + 1 && !step.edge : !step.drop && step.edge);
    }
    CHECK(kinds[XR_LIR_ARRAY_SET_REGION_ENTRY_MOVE] == 1 && kinds[XR_LIR_ARRAY_SET_REGION_EXIT_MOVE] == 1 &&
        kinds[XR_LIR_ARRAY_SET_REGION_EDGE_MOVE] == blocks + 1 &&
        kinds[XR_LIR_ARRAY_SET_REGION_NEW_CANDIDATE] == 1 && kinds[XR_LIR_ARRAY_SET_REGION_READ_CANDIDATE] == 1);
    XrLirArraySetRegion sentinel = {.function=733};
    CHECK(xr_lir_ownership_array_region_at(plan, xr_lir_ownership_array_region_count(plan), &sentinel) == XR_LIR_INVALID_ARGUMENT &&
        sentinel.function == 733);
    ars_current_body(c, plan); ars_retirement(c, plan);
}
static void array_set_region_preparations(void) {
    const uint32_t blocks[] = {1,3,6};
    for (size_t n = 0; n < sizeof(blocks) / sizeof(*blocks); ++n) ars_preparation(blocks[n], ARRAY_SET_REGION_LOCAL);
    ars_preparation(4, ARRAY_SET_REGION_PADDED);
    for (unsigned mode = ARRAY_SET_REGION_CALL; mode <= ARRAY_SET_REGION_TRAP; ++mode)
        ars_preparation(3, (enum ArraySetRegionMode)mode);
    ArraySetRegionFixture first = array_set_region_build(NULL, 3, ARRAY_SET_REGION_LOCAL);
    ArraySetRegionFixture second = array_set_region_build(first.module, 6, ARRAY_SET_REGION_LOCAL);
    OK(xr_lir_module_finish(first.module, NULL));
    XrLirOwnershipPlan *plan = NULL;
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=first.module}, &plan, NULL));
    CHECK(xr_lir_ownership_array_region_count(plan) == 2);
    OK(xr_lir_ownership_array_regions_verify(first.module, plan, first.function.id));
    OK(xr_lir_ownership_array_regions_verify(second.module, plan, second.function.id));
    XrLirOwnershipFunction row; OK(xr_lir_ownership_function_at(plan, second.function.id, &row));
    XrLirArraySetRegion region; OK(xr_lir_ownership_array_region_at(plan, row.region_offset, &region));
    CHECK(row.region_offset == 1 && row.region_count == 1 && region.first_step > 0);
    ars_retirement(second, plan);
}
