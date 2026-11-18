/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_array_set_region_cases.h - Conditional facts preserve ordinary storage
 */
#include "lir_array_set_region_fixture.h"
#include "lir_array_region_range_fixture.h"
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
    XrLirArraySetRegionRange range = homes->array_regions[0].range;
    uint64_t range_digest = ars_range_digest(range);
    XrLirArraySetRegionOwnerHome owner_home = homes->array_regions[0].owner_home;
    uint64_t owner_home_digest = ars_owner_home_digest(owner_home);
    uint32_t count = homes->array_region_step_count;
    xr_lir_module_destroy(c.module); xr_lir_ownership_plan_destroy(plan); xr_lir_value_layouts_destroy(layouts);
    CHECK(homes->array_regions[0].body_block_count == c.count && homes->array_region_step_count == count &&
        ars_steps_digest(homes->array_region_steps, count) == before);
    CHECK(ars_range_same(homes->array_regions[0].range, range) &&
        ars_range_digest(homes->array_regions[0].range) == range_digest);
    CHECK(ars_owner_home_same(homes->array_regions[0].owner_home,owner_home) &&
        ars_owner_home_digest(homes->array_regions[0].owner_home) == owner_home_digest);
    ars_home_geometry(homes); xr_lir_value_homes_destroy(homes);
    CHECK(xrt_billing_owner_stats(billing).blocks == 1); xrt_billing_owner_drop(billing);
}
static void ars_range_current(ArraySetRegionFixture c, const XrLirOwnershipPlan *plan,
    XrLirArraySetRegionRange range) {
    XrLirFunctionData *f = &c.module->functions[c.function.id - 1];
    XrLirInstructionView *compare = &f->blocks[c.header.id - 1].instructions[range.condition_instruction - 1].view;
    XrLirPredicate predicate = compare->predicate; compare->predicate = XR_LIR_LE;
    OK(xr_lir_verify(c.module, NULL));
    CHECK(xr_lir_ownership_array_regions_verify(c.module, plan) == XR_LIR_VERIFY_ERROR);
    compare->predicate = predicate;
    const XrLirValueData *one = &f->values[range.one - 1];
    XrLirInstructionView *constant = &f->blocks[one->block - 1].instructions[one->instruction].view;
    CHECK(constant->bits == 1); constant->bits = 2;
    CHECK(xr_lir_ownership_array_regions_verify(c.module, plan) == XR_LIR_VERIFY_ERROR);
    constant->bits = 1;
    XrLirBlockData *body = &f->blocks[c.body[0].id - 1], *latch = &f->blocks[c.body[c.count - 1].id - 1];
    XrLirInstructionView *header = &f->blocks[c.header.id - 1].instructions[f->blocks[c.header.id - 1].instruction_count - 1].view;
    uint32_t *indices[4]; size_t count = 0;
    for (uint32_t i = 0; i < body->instruction_count; ++i) {
        XrLirInstructionView *op = &body->instructions[i].view;
        const XrLirArrayIntrinsic *shape = op->op == XR_LIR_CALL_INTRINSIC ?
            xr_lir_internal_array_intrinsic(c.module, op->intrinsic) : NULL;
        if (shape && (shape->operation == XR_LIR_ARRAY_OP_GET || shape->operation == XR_LIR_ARRAY_OP_SET)) {
            CHECK(count < 4); indices[count++] = (uint32_t *)&op->operands[1];
        }
    }
    CHECK(count == 3);
    uint32_t *fields[] = {indices[0],indices[1],indices[2],
        (uint32_t *)&latch->instructions[latch->instruction_count - 1].view.edges[0].arguments[range.iv_argument],
        (uint32_t *)&header->edges[0].arguments[1]};
    for (size_t n = 0; n < sizeof(fields) / sizeof(*fields); ++n) {
        uint32_t original = *fields[n]; *fields[n] = f->blocks[c.header.id - 1].parameters[2];
        OK(xr_lir_verify(c.module, NULL));
        CHECK(xr_lir_ownership_array_regions_verify(c.module, plan) == XR_LIR_VERIFY_ERROR);
        XrLirOwnershipPlan *none = NULL;
        OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=c.module}, &none, NULL));
        XrLirArraySetRegion changed; OK(xr_lir_ownership_array_region_at(none, 0, &changed));
        CHECK(ars_range_same(changed.range, (XrLirArraySetRegionRange){0}));
        CHECK(ars_owner_home_same(changed.owner_home,(XrLirArraySetRegionOwnerHome){0}));
        *fields[n] = original;
        CHECK(xr_lir_ownership_array_regions_verify(c.module, none) == XR_LIR_VERIFY_ERROR);
        xr_lir_ownership_plan_destroy(none);
        OK(xr_lir_ownership_array_regions_verify(c.module, plan));
    }
}
static void ars_range_preparations(void) {
    for (unsigned mode = ARRAY_REGION_RANGE_UNIT; mode <= ARRAY_REGION_RANGE_NEXT_COPIES; ++mode) {
        ArraySetRegionFixture c = ars_range_build(NULL, 3, (enum ArrayRegionRangeMode)mode);
        OK(xr_lir_module_finish(c.module, NULL));
        XrLirOwnershipPlan *plan = NULL; XrLirOwnershipRequest request = {.module=c.module};
        OK(xr_lir_ownership_plan_create(&request, &plan, NULL)); compare_internal(&request, plan);
        CHECK(xr_lir_ownership_array_region_count(plan) == 1);
        XrLirArraySetRegion region; OK(xr_lir_ownership_array_region_at(plan, 0, &region));
        OK(xr_lir_ownership_array_regions_verify(c.module, plan));
        if (mode == ARRAY_REGION_RANGE_UNIT || mode == ARRAY_REGION_RANGE_NEXT_COPIES) {
            XrLirArraySetRegionRange range = region.range;
            const XrLirFunctionData *f = &c.module->functions[c.function.id - 1];
            CHECK(range.kind == XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_SAME_LEN && range.iv_argument == 3 &&
                range.iv == f->blocks[c.header.id - 1].parameters[3] && range.entry_iv == f->parameters[0] &&
                range.length && range.length_instruction && range.condition && range.condition_instruction &&
                range.increment && range.increment_block == c.body[mode == ARRAY_REGION_RANGE_UNIT ? 2 : 0].id &&
                range.increment_instruction && range.one && range.latch_iv);
            CHECK(region.owner_home.kind == XR_LIR_ARRAY_SET_OWNER_HOME_CANONICAL_WINDOW &&
                region.owner_home.first_active_step == 2 && region.owner_home.exit_step == region.step_count);
            if (mode == ARRAY_REGION_RANGE_UNIT) {
                CHECK(range.latch_iv == range.increment); ars_range_current(c, plan, range);
            } else CHECK(range.latch_iv != range.increment);
        } else {
            CHECK(ars_range_same(region.range, (XrLirArraySetRegionRange){0}));
            CHECK(ars_owner_home_same(region.owner_home,(XrLirArraySetRegionOwnerHome){0}));
        }
        ars_retirement(c, plan);
    }
    ArraySetRegionFixture c = ars_range_build(NULL, ARRAY_SET_REGION_BODY_CAPACITY, ARRAY_REGION_RANGE_NEXT_COPIES);
    OK(xr_lir_module_finish(c.module, NULL));
    XrLirOwnershipPlan *plan = NULL;
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=c.module}, &plan, NULL));
    XrLirArraySetRegion region; OK(xr_lir_ownership_array_region_at(plan, 0, &region));
    CHECK(region.body_block_count == ARRAY_SET_REGION_BODY_CAPACITY && region.range.kind ==
        XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_SAME_LEN);
    OK(xr_lir_ownership_array_regions_verify(c.module, plan)); ars_retirement(c, plan);
    c = ars_range_build(NULL,3,ARRAY_REGION_RANGE_FAILING_RHS);
    OK(xr_lir_module_finish(c.module,NULL)); plan = NULL;
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=c.module},&plan,NULL));
    CHECK(!xr_lir_ownership_array_region_count(plan));
    OK(xr_lir_ownership_array_regions_verify(c.module,plan));
    xr_lir_ownership_plan_destroy(plan); xr_lir_module_destroy(c.module);
}
static void ars_current_body(ArraySetRegionFixture c, const XrLirOwnershipPlan *plan) {
    XrLirFunctionData *f = &c.module->functions[c.function.id - 1];
    XrLirBlockData *latch = &f->blocks[c.body[c.count - 1].id - 1];
    XrLirInstructionView *increment = &latch->instructions[latch->instruction_count - 2].view;
    CHECK(increment->op == XR_LIR_ADD); increment->op = XR_LIR_DIV;
    OK(xr_lir_verify(c.module, NULL));
    CHECK(xr_lir_ownership_array_regions_verify(c.module, plan) == XR_LIR_VERIFY_ERROR);
    increment->op = XR_LIR_ADD;
    OK(xr_lir_ownership_array_regions_verify(c.module, plan));
}
static void ars_current_retained_death(ArraySetRegionFixture c, const XrLirOwnershipPlan *plan) {
    XrLirBlockData *entry = &c.module->functions[c.function.id - 1].blocks[c.entry.id - 1];
    uint32_t *death = NULL;
    for (uint32_t i = 0; i + 1 < entry->instruction_count; ++i)
        if (entry->instructions[i].view.op == XR_LIR_ARRAY_DROP)
            death = (uint32_t *)entry->instructions[i].view.operands;
    CHECK(death);
    uint32_t *survivor = (uint32_t *)entry->instructions[entry->instruction_count - 1].view.edges[0].arguments;
    uint32_t old_death = *death, old_survivor = *survivor;
    CHECK(old_death != old_survivor);
    *death = old_survivor; *survivor = old_death;
    OK(xr_lir_verify(c.module, NULL));
    CHECK(xr_lir_ownership_array_regions_verify(c.module, plan) == XR_LIR_VERIFY_ERROR);
    *death = old_death; *survivor = old_survivor;
    OK(xr_lir_ownership_array_regions_verify(c.module, plan));
}
static void ars_readonly_preparations(void) {
    for (unsigned mode=ARRAY_REGION_RANGE_READ_ONLY;mode<=ARRAY_REGION_RANGE_READ_STEP;++mode) {
        ArraySetRegionFixture c=ars_range_build(NULL,3,(enum ArrayRegionRangeMode)mode);
        OK(xr_lir_module_finish(c.module,NULL));
        XrLirOwnershipPlan *plan=NULL; XrLirOwnershipRequest request={.module=c.module};
        OK(xr_lir_ownership_plan_create(&request,&plan,NULL)); compare_internal(&request,plan);
        CHECK(xr_lir_ownership_array_region_count(plan)==(mode==ARRAY_REGION_RANGE_READ_ONLY ? 1u : 0u));
        if (mode!=ARRAY_REGION_RANGE_READ_ONLY) {
            xr_lir_ownership_plan_destroy(plan); xr_lir_module_destroy(c.module); continue;
        }
        XrLirArraySetRegion *region=plan->array_regions.regions.data;
        CHECK(region->primary_kind==XR_LIR_ARRAY_REGION_PRIMARY_GET && region->primary_block==c.body[0].id &&
            region->range.kind==XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_SAME_LEN &&
            region->owner_home.kind==XR_LIR_ARRAY_SET_OWNER_HOME_READ_ONLY_WINDOW);
        uint32_t *fields[]={&region->primary_kind,&region->primary_block,&region->primary_instruction};
        for (size_t n=0;n<3;++n) {
            uint32_t saved=*fields[n]; *fields[n]^=1;
            CHECK(xr_lir_ownership_array_regions_verify(c.module,plan)==XR_LIR_VERIFY_ERROR); *fields[n]=saved;
        }
        XrLirFunctionData *function=&c.module->functions[c.function.id-1];
        XrLirInstructionView *get=&function->blocks[region->primary_block-1].instructions[region->primary_instruction-1].view;
        uint32_t *index=(uint32_t *)&get->operands[1], saved=*index; *index=region->range.one;
        CHECK(xr_lir_ownership_array_regions_verify(c.module,plan)==XR_LIR_VERIFY_ERROR); *index=saved;
        OK(xr_lir_ownership_array_regions_verify(c.module,plan)); ars_retirement(c,plan);
    }
}
static void ars_preparation(uint32_t blocks, enum ArraySetRegionMode mode) {
    ArraySetRegionFixture c = array_set_region_fixture(blocks, mode);
    XrLirOwnershipPlan *plan = NULL; XrLirOwnershipRequest request = {.module=c.module};
    OK(xr_lir_ownership_plan_create(&request, &plan, NULL)); compare_internal(&request, plan);
    bool expected = mode == ARRAY_SET_REGION_LOCAL || mode == ARRAY_SET_REGION_PADDED || mode == ARRAY_SET_REGION_DIAMONDS ||
        mode == ARRAY_SET_REGION_RETAIN_SOURCE || mode == ARRAY_SET_REGION_RETAIN_TARGET ||
        mode == ARRAY_SET_REGION_RETAIN_MANY || mode == ARRAY_SET_REGION_RETAIN_DOMINATING;
    CHECK(xr_lir_ownership_array_region_count(plan) == (expected ? 1u : 0u));
    OK(xr_lir_ownership_array_regions_verify(c.module, plan));
    XrLirOwnershipFunction row; OK(xr_lir_ownership_function_at(plan, c.function.id, &row));
    CHECK(row.region_count == (expected ? 1u : 0u));
    if (!expected) { xr_lir_module_destroy(c.module); xr_lir_ownership_plan_destroy(plan); return; }
    XrLirArraySetRegion region; OK(xr_lir_ownership_array_region_at(plan, row.region_offset, &region));
    CHECK(region.function == c.function.id && region.header == c.header.id && region.entry_block == c.region_entry.id &&
        region.latch == c.body[blocks - 1].id && region.exit_block == c.exit.id && region.body_block_count == blocks &&
        region.canonical_value == c.header_array.id && region.primary_kind == XR_LIR_ARRAY_REGION_PRIMARY_SET &&
        region.primary_block == c.body[0].id && region.primary_instruction);
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
    ars_current_body(c, plan);
    if (mode == ARRAY_SET_REGION_RETAIN_SOURCE || mode == ARRAY_SET_REGION_RETAIN_TARGET)
        ars_current_retained_death(c, plan);
    ars_retirement(c, plan);
}
static void ars_current_inventory(void) {
    ArraySetRegionFixture absent = array_set_region_fixture(3, ARRAY_SET_REGION_NO_SET);
    ArraySetRegionFixture present = array_set_region_fixture(3, ARRAY_SET_REGION_LOCAL);
    XrLirOwnershipPlan *old_absent = NULL, *old_present = NULL;
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=absent.module}, &old_absent, NULL));
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=present.module}, &old_present, NULL));
    CHECK(!xr_lir_ownership_array_region_count(old_absent) && xr_lir_ownership_array_region_count(old_present) == 1);
    CHECK(xr_lir_ownership_array_regions_verify(present.module, old_absent) == XR_LIR_VERIFY_ERROR);
    CHECK(xr_lir_ownership_array_regions_verify(absent.module, old_present) == XR_LIR_VERIFY_ERROR);
    OK(xr_lir_ownership_array_regions_verify(absent.module, old_absent));
    OK(xr_lir_ownership_array_regions_verify(present.module, old_present));
    xr_lir_ownership_plan_destroy(old_absent); xr_lir_ownership_plan_destroy(old_present);
    xr_lir_module_destroy(absent.module); xr_lir_module_destroy(present.module);
}
static void ars_current_retained(void) {
    ArraySetRegionFixture snapshot = array_set_region_fixture(3, ARRAY_SET_REGION_SNAPSHOT);
    XrLirOwnershipPlan *old_snapshot = NULL;
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=snapshot.module}, &old_snapshot, NULL));
    CHECK(!xr_lir_ownership_array_region_count(old_snapshot));
    const enum ArraySetRegionMode modes[] = {ARRAY_SET_REGION_RETAIN_SOURCE,ARRAY_SET_REGION_RETAIN_TARGET};
    for (uint32_t n = 0; n < 2; ++n) {
        ArraySetRegionFixture retired = array_set_region_fixture(3, modes[n]);
        XrLirOwnershipPlan *old_retired = NULL;
        OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=retired.module}, &old_retired, NULL));
        CHECK(xr_lir_ownership_array_region_count(old_retired) == 1);
        CHECK(xr_lir_ownership_array_regions_verify(retired.module, old_snapshot) == XR_LIR_VERIFY_ERROR);
        CHECK(xr_lir_ownership_array_regions_verify(snapshot.module, old_retired) == XR_LIR_VERIFY_ERROR);
        OK(xr_lir_ownership_array_regions_verify(retired.module, old_retired));
        xr_lir_ownership_plan_destroy(old_retired); xr_lir_module_destroy(retired.module);
    }
    OK(xr_lir_ownership_array_regions_verify(snapshot.module, old_snapshot));
    xr_lir_ownership_plan_destroy(old_snapshot); xr_lir_module_destroy(snapshot.module);
}
static void array_set_region_preparations(void) {
    ars_range_preparations();
    ars_readonly_preparations();
    const uint32_t blocks[] = {1,3,6,ARRAY_SET_REGION_BODY_CAPACITY};
    for (size_t n = 0; n < sizeof(blocks) / sizeof(*blocks); ++n) ars_preparation(blocks[n], ARRAY_SET_REGION_LOCAL);
    ars_preparation(4, ARRAY_SET_REGION_PADDED);
    for (unsigned mode = ARRAY_SET_REGION_CALL; mode <= ARRAY_SET_REGION_TRAP; ++mode)
        ars_preparation(3, (enum ArraySetRegionMode)mode);
    ars_preparation(ARRAY_SET_REGION_BODY_CAPACITY, ARRAY_SET_REGION_EXTRA_ENTRY);
    ars_preparation(3, ARRAY_SET_REGION_OUTSIDE_NEW_SNAPSHOT);
    ars_preparation(3, ARRAY_SET_REGION_OUTSIDE_READ_SNAPSHOT);
    ars_preparation(3, ARRAY_SET_REGION_NO_SET);
    ars_preparation(3, ARRAY_SET_REGION_REF_ORIGIN);
    ars_preparation(3, ARRAY_SET_REGION_ARRAY_FORMAL);
    ars_preparation(3, ARRAY_SET_REGION_DIAMONDS);
    for (unsigned mode = ARRAY_SET_REGION_RETAIN_SOURCE; mode <= ARRAY_SET_REGION_RETAIN_BODY_TARGET; ++mode)
        ars_preparation(3, (enum ArraySetRegionMode)mode);
    ars_current_inventory();
    ars_current_retained();
    ArraySetRegionFixture first = array_set_region_build(NULL, 3, ARRAY_SET_REGION_LOCAL);
    ArraySetRegionFixture second = array_set_region_build(first.module, 6, ARRAY_SET_REGION_LOCAL);
    OK(xr_lir_module_finish(first.module, NULL));
    XrLirOwnershipPlan *plan = NULL;
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=first.module}, &plan, NULL));
    CHECK(xr_lir_ownership_array_region_count(plan) == 2);
    OK(xr_lir_ownership_array_regions_verify(first.module, plan));
    ars_current_body(second, plan);
    XrLirOwnershipFunction row; OK(xr_lir_ownership_function_at(plan, second.function.id, &row));
    XrLirArraySetRegion region; OK(xr_lir_ownership_array_region_at(plan, row.region_offset, &region));
    CHECK(row.region_offset == 1 && row.region_count == 1 && region.first_step > 0);
    ars_retirement(second, plan);
}
