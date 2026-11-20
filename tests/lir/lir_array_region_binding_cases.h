/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_array_region_binding_cases.h - Numeric bindings outlive their creators
 */
#ifndef LIR_ARRAY_REGION_BINDING_CASES_H
#define LIR_ARRAY_REGION_BINDING_CASES_H
#include "lir_array_region_loop_fixture.h"
#include "lir_array_push_window_fixture.h"
static void array_push_binding_projection(void) {
    const enum ArrayPushWindowMode modes[]={PUSH_WINDOW_CURRENT,PUSH_WINDOW_PARAMETER_END,PUSH_WINDOW_MIRRORED_END,
        PUSH_WINDOW_ENTRY_EXIT_RETAINS,PUSH_WINDOW_EXIT_BRIDGE};
    for (size_t test=0;test<sizeof(modes)/sizeof(*modes);++test) {
        ArraySetRegionFixture c=array_push_window_build(modes[test]);
        XrtBillingOwner *billing=NULL; MANAGED(xrt_billing_owner_new(NULL,SIZE_MAX,&billing));
        XrLirValueLayoutKey roots[]={{XR_LIR_UNIT,0,0},{XR_LIR_I1,0,0},{XR_LIR_I64,0,0},{XR_LIR_ARRAY,XR_LIR_I64,0}};
        XrLirValueLayouts *layouts=NULL; HOME(xr_lir_value_layouts_create(c.module,roots,4,billing,&layouts));
        XrLirOwnershipPlan *plan=NULL; OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=c.module},&plan,NULL));
        XrLirOwnershipFunction function; OK(xr_lir_ownership_function_at(plan,c.function.id,&function));
        CHECK(function.fixed_update_count==1);
        XrLirArraySetRegion published={0};
        OK(xr_lir_ownership_fixed_array_update_at(plan,function.fixed_update_offset,&published));
        XrLirValueHomesRequest request={c.module,c.function,layouts,plan};
        size_t baseline=live,bytes=home_live_bytes();
        XrLirValueHomes *homes=NULL;
        attempts=0; HOME(xr_lir_value_homes_create(&request,&homes));
        size_t sites=attempts;
        CHECK(sites && homes->fixed_array_update_count==1);
        const XrLirFixedArrayUpdateHome *row=&homes->fixed_array_updates[0];
        CHECK(row->region.push_counted && published.push_counted && row->region.range.iv==published.range.iv &&
            row->region.range.length==published.range.length && row->region.step_count==published.step_count &&
            row->owner_slot!=row->workspace_slot && row->workspace_slot!=SIZE_MAX);
        CHECK(homes->values[row->region.canonical_value].first_owner==row->owner_slot);
        CHECK(row->body_edge==1);
        bool entry=false,latch=false,workspace=false;
        for (uint32_t n=0;n<row->region.step_count;++n) {
            XrLirArraySetRegionStep step=homes->fixed_array_update_steps[row->region.first_step+n];
            switch (step.kind) {
            case XR_LIR_ARRAY_SET_REGION_ENTRY_MOVE:
                CHECK(homes->values[step.target].first_owner==row->owner_slot &&
                    step.block==row->region.entry_block && step.edge==row->region.entry_edge);
                entry=true; break;
            case XR_LIR_ARRAY_SET_REGION_EXIT_MOVE:
                CHECK(homes->values[step.source].first_owner==row->owner_slot);
                break;
            case XR_LIR_ARRAY_SET_REGION_EDGE_MOVE:
                CHECK(homes->values[step.source].first_owner==row->owner_slot &&
                    homes->values[step.target].first_owner==row->owner_slot);
                latch|=step.block==row->region.latch;
                break;
            case XR_LIR_ARRAY_SET_REGION_NEW_CANDIDATE:
                CHECK(homes->values[step.source].first_owner==row->owner_slot &&
                    homes->bindings[step.target].first_owner==row->owner_slot);
                break;
            case XR_LIR_ARRAY_SET_REGION_READ_CANDIDATE:
                CHECK(homes->bindings[step.source].first_owner==row->owner_slot &&
                    homes->values[step.target].first_owner==row->owner_slot);
                break;
            default: CHECK(false);
            }
        }
        for (size_t n=0;n<homes->workspace_count;++n)
            if (homes->workspaces[n].block==row->region.primary_block &&
                homes->workspaces[n].instruction==row->region.primary_instruction) {
                CHECK(homes->workspaces[n].first_owner==row->workspace_slot); workspace=true;
            }
        CHECK(entry && latch && workspace);
        xr_lir_value_homes_destroy(homes); homes=NULL;
        CHECK(live==baseline && home_live_bytes()==bytes);
        if (!test) for (size_t point=1;point<=sites;++point) {
            attempts=0; fail_at=point;
            CHECK(xr_lir_value_homes_create(&request,&homes)==XR_LIR_VALUE_LAYOUT_OOM && !homes &&
                attempts>=point && live==baseline && home_live_bytes()==bytes);
        }
        fail_at=SIZE_MAX; HOME(xr_lir_value_homes_create(&request,&homes));
        xr_lir_ownership_plan_destroy(plan); xr_lir_value_layouts_destroy(layouts); xr_lir_module_destroy(c.module);
        row=&homes->fixed_array_updates[0];
        XrtArrayI64RegionSlot slots[XR_LIR_FIXED_ARRAY_GUARD_SLOT_COUNT];
        XrtArrayI64RegionSpec spec=xr_lir_fixed_array_guard_spec(homes,row,slots);
        CHECK(spec.slot_count==2 && spec.canonical<2 && slots[spec.canonical].slot==row->owner_slot &&
            slots[1-spec.canonical].slot==row->workspace_slot && row->region.range.iv==published.range.iv &&
            row->region.range.length==published.range.length);
        xr_lir_value_homes_destroy(homes); CHECK(!live && !home_live_bytes());
        CHECK(xrt_billing_owner_stats(billing).blocks==1); xrt_billing_owner_drop(billing);
    }
}
static void fixed_array_home_allocation_faults(void) {
    ArraySetRegionFixture c = ars_range_build_seeded(NULL, 3, ARRAY_REGION_RANGE_UNIT, true);
    OK(xr_lir_module_finish(c.module, NULL));
    XrtBillingOwner *billing = NULL;
    MANAGED(xrt_billing_owner_new(NULL, SIZE_MAX, &billing));
    XrLirValueLayoutKey roots[] = {{XR_LIR_UNIT,0,0},{XR_LIR_I1,0,0},{XR_LIR_I64,0,0},{XR_LIR_ARRAY,XR_LIR_I64,0}};
    XrLirValueLayouts *layouts = NULL;
    HOME(xr_lir_value_layouts_create(c.module, roots, 4, billing, &layouts));
    XrLirOwnershipPlan *plan = NULL;
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=c.module}, &plan, NULL));
    XrLirValueHomesRequest request = {c.module,c.function,layouts,plan};
    XrLirValueHomes *homes = NULL;
    size_t baseline = live, bytes = home_live_bytes();
    attempts = 0;
    HOME(xr_lir_value_homes_create(&request, &homes));
    size_t sites = attempts;
    CHECK(sites && homes->fixed_array_update_count == 1 && homes->fixed_array_update_step_count);
    xr_lir_value_homes_destroy(homes); homes = NULL;
    for (size_t point = 1; point <= sites; ++point) {
        attempts = 0; fail_at = point;
        CHECK(xr_lir_value_homes_create(&request, &homes) == XR_LIR_VALUE_LAYOUT_OOM && !homes &&
            attempts >= point && live == baseline && home_live_bytes() == bytes);
    }
    fail_at = SIZE_MAX;
    HOME(xr_lir_value_homes_create(&request, &homes));
    CHECK(homes->fixed_array_update_count == 1);
    xr_lir_value_homes_destroy(homes); xr_lir_ownership_plan_destroy(plan);
    xr_lir_value_layouts_destroy(layouts); xr_lir_module_destroy(c.module);
    CHECK(!live && !home_live_bytes() && xrt_billing_owner_stats(billing).blocks == 1);
    xrt_billing_owner_drop(billing);
}

static void array_region_binding_projection(unsigned shape) {
    if (!shape) {
        array_push_binding_projection();
        fixed_array_home_allocation_faults();
    }
    bool large=shape==1, shared=shape==2 || shape==3 || shape>=5, reverse=shape==3 || shape==7;
    bool range=shape>=5, single_range=shape==4;
    unsigned read_mask=shape>=6 ? shape-5 : 0;
    ArraySetRegionFixture c=single_range ? ars_range_build(NULL,3,ARRAY_REGION_RANGE_BOUND) :
        large ? array_set_region_fixture(257,ARRAY_SET_REGION_LOCAL) :
        read_mask ? array_region_binding_loops(shared,reverse,range,read_mask,false) :
        array_region_binding_two_loops(shared,reverse,range);
    if (single_range) OK(xr_lir_module_finish(c.module,NULL));
    XrtBillingOwner *billing=NULL; MANAGED(xrt_billing_owner_new(NULL,SIZE_MAX,&billing));
    XrLirValueLayoutKey roots[]={{XR_LIR_UNIT,0,0},{XR_LIR_I1,0,0},{XR_LIR_I64,0,0},{XR_LIR_ARRAY,XR_LIR_I64,0}};
    XrLirValueLayouts *layouts=NULL; HOME(xr_lir_value_layouts_create(c.module,roots,4,billing,&layouts));
    XrLirOwnershipPlan *plan=NULL;
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=c.module},&plan,NULL));
    XrLirValueHomes *homes=NULL;
    HOME(xr_lir_value_homes_create(&(XrLirValueHomesRequest){c.module,c.function,layouts,plan},&homes));
    uint32_t count=large || single_range ? 1 : 2;
    XrLirOwnershipFunction owner; OK(xr_lir_ownership_function_at(plan,c.function.id,&owner));
    CHECK(owner.fixed_update_count==count && homes->fixed_array_update_count==count);
    for (uint32_t n=0;n<count;++n) {
        const XrLirFixedArrayUpdateHome *row=&homes->fixed_array_updates[n];
        XrLirArraySetRegion published;
        OK(xr_lir_ownership_fixed_array_update_at(plan,owner.fixed_update_offset+n,&published));
        CHECK(ars_range_same(row->region.range,published.range) && row->region.header==published.header);
        CHECK(row->region.range.kind==(uint32_t)(range ? XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_SAME_LEN :
            XR_LIR_ARRAY_SET_RANGE_NONE));
        XrtArrayI64RegionSlot slots[XR_LIR_FIXED_ARRAY_GUARD_SLOT_COUNT];
        XrtArrayI64RegionSpec guard=xr_lir_fixed_array_guard_spec(homes,row,slots);
        CHECK(guard.canonical==0 && slots[0].slot==row->owner_slot &&
            slots[0].offset==homes->values[row->region.canonical_value].offset);
        if (row->region.primary_kind==XR_LIR_ARRAY_REGION_PRIMARY_GET)
            CHECK(guard.slot_count==1 && row->workspace_slot==SIZE_MAX);
        else CHECK(guard.slot_count==2 && slots[1].slot==row->workspace_slot && row->owner_slot!=row->workspace_slot);
        XrLirValueHome *values=(XrLirValueHome *)homes->values;
        XrLirValueHome *canonical=&values[row->region.canonical_value];
        uint32_t parameter=canonical->parameter;
        canonical->parameter=1;
        LirHomeBuilder builder={.homes=homes,.layouts=layouts,.compiler={.input=c.module,.function=c.function}};
        XrLirFixedArrayUpdateHome projected={.body_edge=733};
        CHECK(!lir_fixed_array_project(&builder,plan,&published,NULL,&projected) && projected.body_edge==733);
        canonical->parameter=parameter;
    }
    if (shared) {
        const XrLirArraySetRegion *first=&homes->fixed_array_updates[reverse ? 1 : 0].region;
        const XrLirArraySetRegion *next=&homes->fixed_array_updates[reverse ? 0 : 1].region;
        CHECK(first->header==next->entry_block && first->exit_edge==next->entry_edge &&
            first->exit_block==next->header && first->exit_value==next->canonical_value);
        const XrLirArraySetRegionStep *steps=homes->fixed_array_update_steps;
        const XrLirArraySetRegionStep *exit=&steps[first->first_step+first->step_count-1];
        const XrLirArraySetRegionStep *entry=&steps[next->first_step];
        CHECK(exit->kind==XR_LIR_ARRAY_SET_REGION_EXIT_MOVE && entry->kind==XR_LIR_ARRAY_SET_REGION_ENTRY_MOVE &&
            exit->source==entry->source && exit->target==entry->target);
    }
    xr_lir_ownership_plan_destroy(plan); xr_lir_value_layouts_destroy(layouts); xr_lir_module_destroy(c.module);
    CHECK(homes->fixed_array_update_count==count);
    xr_lir_value_homes_destroy(homes);
    CHECK(!live && !home_live_bytes() && xrt_billing_owner_stats(billing).blocks==1);
    xrt_billing_owner_drop(billing);
}
