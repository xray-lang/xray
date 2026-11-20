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
static void array_region_binding_check(const XrLirArrayRegionBindings *b,const XrLirValueHomes *homes,
    const XrLirModule *module,XrLirFunction function) {
    CHECK(b->region_count==homes->array_region_count);
    for (size_t n=0;n<b->region_count;++n) {
        const XrLirArrayRegionBinding *r=&b->regions[n]; CHECK(r->slot_count && r->move_count==r->region.step_count);
        CHECK(ars_range_same(r->region.range, homes->array_regions[n].range));
        CHECK(ars_owner_home_same(r->region.owner_home,homes->array_regions[n].owner_home));
        if (r->region.owner_home.kind) {
            CHECK(r->region.owner_home.first_active_step==2 && r->region.owner_home.exit_step==r->move_count);
            const XrLirArrayRegionMove *exit=&b->moves[r->first_move+r->region.owner_home.exit_step-1];
            CHECK(r->owner_home.kind==r->region.owner_home.kind &&
                r->owner_home.stable_slot==r->canonical_slot && r->owner_home.exit_slot==exit->target_slot &&
                r->owner_home.stable_slot!=r->owner_home.exit_slot && exit->step.kind==XR_LIR_ARRAY_SET_REGION_EXIT_MOVE);
        } else CHECK(!r->owner_home.kind && !r->owner_home.stable_slot && !r->owner_home.exit_slot);
        CHECK(r->canonical_slot>=r->first_slot && r->canonical_slot-r->first_slot<r->slot_count);
        XrtArrayI64RegionSpec runtime={b->slots+r->first_slot,r->slot_count,r->canonical_slot-r->first_slot};
        CHECK(runtime.slots[runtime.canonical].slot==homes->values[r->region.canonical_value].first_owner);
        for (size_t p=0;p<r->slot_count;++p) {
            XrtArrayI64RegionSlot slot=runtime.slots[p];
            CHECK(slot.slot<homes->frame.slot_count && homes->frame.owned_offsets[slot.slot]==slot.offset);
            for (size_t q=0;q<p;++q) CHECK(runtime.slots[q].slot!=slot.slot && runtime.slots[q].offset!=slot.offset);
        }
        for (size_t p=0;p<r->move_count;++p) {
            const XrLirArrayRegionMove *move=&b->moves[r->first_move+p];
            CHECK(move->source_slot>=r->first_slot && move->source_slot-r->first_slot<r->slot_count);
            CHECK(move->target_slot>=r->first_slot && move->target_slot-r->first_slot<r->slot_count);
            const XrLirArrayRegionBlock *block=&b->block_index[move->step.block];
            if (move->step.edge) {
                size_t edge=block->edges[move->step.edge-1]; CHECK(edge && edge<=b->edge_count);
                CHECK(b->edges[edge-1].region==n+1 && b->edges[edge-1].move==r->first_move+p+1);
            } else {
                CHECK(block->region==n+1);
                CHECK(b->operations[block->first_operation+move->step.instruction-1].move==r->first_move+p+1);
                CHECK(b->operations[block->first_operation+move->step.drop-1].drop==r->first_move+p+1);
                bool found=false;
                for (size_t e=0;e<homes->event_count;++e) {
                    const XrLirHomeEvent *event=&homes->events[e];
                    if (event->block==move->step.block && event->instruction==move->step.instruction &&
                        event->forwarding_mode==XR_LIR_OWNERSHIP_TRANSFER_MOVE) {
                        CHECK(event->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_NONE && event->forwarding_drop==move->step.drop);
                        found=true;
                    }
                }
                CHECK(found);
            }
        }
        CHECK(r->block_count==(size_t)r->region.body_block_count+1);
        for (size_t p=0;p<r->block_count;++p) {
            uint32_t block=b->blocks[r->first_block+p]; CHECK(block && block<b->block_index_count);
            const XrLirArrayRegionBlock *index=&b->block_index[block];
            XrLirBlockView body={0}; OK(xr_lir_block_view(module,(XrLirBlock){module,function.id,block},&body));
            CHECK(index->region==n+1 && index->instruction_count==body.instruction_count &&
                index->first_operation<=b->operation_count && index->instruction_count<=b->operation_count-index->first_operation);
        }
        for (size_t p=0;p<r->access_count;++p) {
            const XrLirArrayRegionAccess *a=&b->accesses[r->first_access+p];
            XrLirInstructionView instruction={0};
            OK(xr_lir_instruction_view(module,(XrLirBlock){module,function.id,a->block},a->instruction-1,&instruction));
            CHECK(a->receiver==instruction.operands[0] && a->result==instruction.result);
            CHECK(a->receiver_slot>=r->first_slot && a->receiver_slot-r->first_slot<r->slot_count);
            CHECK(b->operations[b->block_index[a->block].first_operation+a->instruction-1].access==r->first_access+p+1);
            if (a->operation==XR_LIR_ARRAY_OP_SET) {
                CHECK(a->index==instruction.operands[1] && a->item==instruction.operands[2]);
                CHECK(a->workspace_slot>=r->first_slot && a->workspace_slot-r->first_slot<r->slot_count);
            } else {
                CHECK(a->workspace_slot==SIZE_MAX && !a->item);
                CHECK(a->operation==XR_LIR_ARRAY_OP_GET ? a->index==instruction.operands[1] : !a->index);
            }
        }
    }
}
static void array_region_binding_unsupported(const XrLirValueHomes *homes,const XrLirValueLayouts *set) {
    size_t count=(size_t)homes->value_count+1;
    XrLirValueHome *values=xr_malloc(count*sizeof(*values)); CHECK(values);
    for (uint32_t disabled=0;disabled<homes->array_region_count;++disabled) {
        memcpy(values,homes->values,count*sizeof(*values));
        values[homes->array_regions[disabled].canonical_value].parameter=1;
        XrLirValueHomes view=*homes; view.values=values;
        XrLirArrayRegionBindings *bindings=NULL; HOME(xr_lir_array_region_bindings_create(&view,set,&bindings));
        CHECK(bindings->region_count==homes->array_region_count && !bindings->regions[disabled].slot_count);
        CHECK(ars_owner_home_same(bindings->regions[disabled].region.owner_home,homes->array_regions[disabled].owner_home) &&
            !bindings->regions[disabled].owner_home.kind && !bindings->regions[disabled].owner_home.stable_slot &&
            !bindings->regions[disabled].owner_home.exit_slot);
        for (size_t n=0;n<bindings->region_count;++n) if (n!=disabled) CHECK(bindings->regions[n].slot_count);
        for (size_t n=0;n<bindings->block_index_count;++n) CHECK(bindings->block_index[n].region!=disabled+1);
        for (size_t n=0;n<bindings->edge_count;++n) CHECK(bindings->edges[n].region!=disabled+1);
        xr_lir_array_region_bindings_destroy(bindings);
    }
    xr_free(values);
    {
        size_t size=homes->array_region_count*sizeof(*homes->array_regions);
        XrLirArraySetRegion *regions=xr_malloc(size); CHECK(regions); memcpy(regions,homes->array_regions,size);
        for (uint32_t n=0;n<homes->array_region_count;++n) regions[n].owner_home=(XrLirArraySetRegionOwnerHome){0};
        XrLirValueHomes ordinary=*homes; ordinary.array_regions=regions;
        XrLirArrayRegionBindings *projection=NULL; HOME(xr_lir_array_region_bindings_create(&ordinary,set,&projection));
        for (size_t n=0;n<projection->region_count;++n) CHECK(projection->regions[n].slot_count &&
            !projection->regions[n].owner_home.kind && !projection->regions[n].owner_home.stable_slot &&
            !projection->regions[n].owner_home.exit_slot);
        xr_lir_array_region_bindings_destroy(projection); projection=NULL;
        regions[0].owner_home.first_active_step=1;
        CHECK(xr_lir_array_region_bindings_create(&ordinary,set,&projection)==XR_LIR_VALUE_LAYOUT_ARGUMENT && !projection);
        xr_free(regions);
    }
    XrLirValueHomes view=*homes; view.frame.payload_alignment=1;
    XrLirArrayRegionBindings *bindings=NULL; HOME(xr_lir_array_region_bindings_create(&view,set,&bindings));
    CHECK(!bindings->slot_count && !bindings->move_count && !bindings->access_count && !bindings->block_count && !bindings->operation_count);
    xr_lir_array_region_bindings_destroy(bindings); bindings=NULL;
    view=*homes; view.array_region_count=view.array_region_step_count=0;
    HOME(xr_lir_array_region_bindings_create(&view,set,&bindings));
    CHECK(bindings && !bindings->region_count && !bindings->slot_count && !bindings->edge_count);
    xr_lir_array_region_bindings_destroy(bindings);
    bindings=NULL; view=*homes; view.frame.slot_count=SIZE_MAX;
    CHECK(xr_lir_array_region_bindings_create(&view,set,&bindings)==XR_LIR_VALUE_LAYOUT_OVERFLOW && !bindings);
}
static void array_region_binding_shared_check(const XrLirArrayRegionBindings *bindings,
    const XrLirValueHomes *homes,const XrLirValueLayouts *set,const XrLirModule *module,XrLirFunction function,bool reverse) {
    CHECK(homes->array_region_count==2 && bindings->region_count==2);
    const XrLirArraySetRegion *a=&homes->array_regions[reverse ? 1 : 0],*b=&homes->array_regions[reverse ? 0 : 1];
    CHECK(reverse ? b->header<a->header : a->header<b->header);
    CHECK(a->header==b->entry_block && a->exit_edge==b->entry_edge && a->exit_block==b->header &&
        a->exit_value==b->canonical_value && b->entry_value==a->canonical_value);
    const XrLirArrayRegionBinding *skipped=&bindings->regions[1];
    CHECK(bindings->regions[0].slot_count && !skipped->slot_count && !skipped->move_count &&
        !skipped->access_count && !skipped->block_count && skipped->canonical_slot==SIZE_MAX);
    XrLirValueHomes first=*homes; first.array_region_count=1;
    XrLirArrayRegionBindings *alone=NULL; HOME(xr_lir_array_region_bindings_create(&first,set,&alone));
    array_region_binding_check(alone,&first,module,function);
    XrLirArrayRegionBindings kept=*bindings; kept.region_count=1;
    array_region_binding_check(&kept,&first,module,function);
    CHECK(bindings->slot_count==alone->slot_count && bindings->move_count==alone->move_count &&
        bindings->access_count==alone->access_count && bindings->block_count==alone->block_count &&
        bindings->operation_count==alone->operation_count && bindings->edge_count==alone->edge_count &&
        bindings->block_index_count==alone->block_index_count);
    for (size_t n=0;n<alone->slot_count;++n) CHECK(bindings->slots[n].slot==alone->slots[n].slot &&
        bindings->slots[n].offset==alone->slots[n].offset);
    for (size_t n=0;n<alone->edge_count;++n) CHECK(bindings->edges[n].region==alone->edges[n].region &&
        bindings->edges[n].kind==alone->edges[n].kind && bindings->edges[n].move==alone->edges[n].move);
    for (size_t n=0;n<alone->operation_count;++n) CHECK(bindings->operations[n].access==alone->operations[n].access &&
        bindings->operations[n].move==alone->operations[n].move && bindings->operations[n].drop==alone->operations[n].drop);
    for (size_t n=0;n<bindings->block_index_count;++n) {
        const XrLirArrayRegionBlock *actual=&bindings->block_index[n],*expected=&alone->block_index[n];
        CHECK(actual->region==expected->region && actual->first_operation==expected->first_operation &&
            actual->instruction_count==expected->instruction_count && actual->edges[0]==expected->edges[0] &&
            actual->edges[1]==expected->edges[1]);
    }
    size_t shared=bindings->block_index[a->header].edges[a->exit_edge-1]; CHECK(shared);
    CHECK(bindings->edges[shared-1].region==1 && bindings->edges[shared-1].kind==
        (reverse ? XR_LIR_ARRAY_REGION_EDGE_ENTRY : XR_LIR_ARRAY_REGION_EDGE_EXIT));
    xr_lir_array_region_bindings_destroy(alone);
}
static void array_region_binding_mixed(bool reverse,unsigned read_mask) {
    ArraySetRegionFixture c=array_region_binding_loops(true,reverse,true,read_mask,false);
    XrtBillingOwner *billing=NULL; MANAGED(xrt_billing_owner_new(NULL,SIZE_MAX,&billing));
    XrLirValueLayoutKey roots[]={{XR_LIR_UNIT,0,0},{XR_LIR_I1,0,0},{XR_LIR_I64,0,0},{XR_LIR_ARRAY,XR_LIR_I64,0}};
    XrLirValueLayouts *set=NULL; HOME(xr_lir_value_layouts_create(c.module,roots,4,billing,&set));
    XrLirOwnershipPlan *plan=NULL; OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=c.module},&plan,NULL));
    XrLirValueHomes *homes=NULL;
    HOME(xr_lir_value_homes_create(&(XrLirValueHomesRequest){c.module,c.function,set,plan},&homes));
    size_t winner=read_mask==1 ? (reverse ? 0u : 1u) : (reverse ? 1u : 0u), skipped=1-winner;
    CHECK(homes->array_region_count==2 && homes->array_regions[winner].primary_kind==XR_LIR_ARRAY_REGION_PRIMARY_SET &&
        homes->array_regions[skipped].primary_kind==XR_LIR_ARRAY_REGION_PRIMARY_GET);
    size_t baseline=live,bytes=home_live_bytes(); attempts=0;
    XrLirArrayRegionBindings *bindings=NULL; HOME(xr_lir_array_region_bindings_create(homes,set,&bindings));
    size_t sites=attempts;
    const XrLirArrayRegionBinding *selected=&bindings->regions[winner],*ordinary=&bindings->regions[skipped];
    CHECK(bindings->region_count==2 && selected->slot_count && selected->move_count && selected->access_count &&
        !ordinary->slot_count && !ordinary->move_count && !ordinary->access_count && !ordinary->block_count &&
        ordinary->canonical_slot==SIZE_MAX);
    for (size_t n=0;n<2;++n) CHECK(bindings->regions[n].region.first_step==homes->array_regions[n].first_step &&
        bindings->regions[n].region.primary_kind==homes->array_regions[n].primary_kind &&
        bindings->regions[n].region.primary_block==homes->array_regions[n].primary_block &&
        bindings->regions[n].region.primary_instruction==homes->array_regions[n].primary_instruction);
    const XrLirArraySetRegion *first=&homes->array_regions[reverse ? 1 : 0],*second=&homes->array_regions[reverse ? 0 : 1];
    CHECK(first->header==second->entry_block && first->exit_edge==second->entry_edge && first->exit_block==second->header);
    size_t edge=bindings->block_index[first->header].edges[first->exit_edge-1]; CHECK(edge);
    CHECK(bindings->edges[edge-1].region==winner+1 && bindings->edges[edge-1].kind==
        ((reverse ? 1u : 0u)==winner ? XR_LIR_ARRAY_REGION_EDGE_EXIT : XR_LIR_ARRAY_REGION_EDGE_ENTRY));
    XrLirValueHomes single=*homes; single.array_regions=&homes->array_regions[winner]; single.array_region_count=1;
    XrLirArrayRegionBindings *alone=NULL; HOME(xr_lir_array_region_bindings_create(&single,set,&alone));
    array_region_binding_check(alone,&single,c.module,c.function);
    CHECK(bindings->slot_count==alone->slot_count && bindings->move_count==alone->move_count &&
        bindings->access_count==alone->access_count && bindings->block_count==alone->block_count &&
        bindings->operation_count==alone->operation_count && bindings->edge_count==alone->edge_count &&
        bindings->block_index_count==alone->block_index_count);
    for (size_t n=0;n<bindings->slot_count;++n) CHECK(bindings->slots[n].slot==alone->slots[n].slot &&
        bindings->slots[n].offset==alone->slots[n].offset);
    for (size_t n=0;n<bindings->edge_count;++n) CHECK(bindings->edges[n].kind==alone->edges[n].kind &&
        bindings->edges[n].move==alone->edges[n].move && bindings->edges[n].region==(alone->edges[n].region ? winner+1 : 0));
    for (size_t n=0;n<bindings->operation_count;++n) CHECK(bindings->operations[n].access==alone->operations[n].access &&
        bindings->operations[n].move==alone->operations[n].move && bindings->operations[n].drop==alone->operations[n].drop);
    for (size_t n=0;n<bindings->block_index_count;++n) CHECK(bindings->block_index[n].region==
        (alone->block_index[n].region ? winner+1 : 0) &&
        bindings->block_index[n].first_operation==alone->block_index[n].first_operation &&
        bindings->block_index[n].instruction_count==alone->block_index[n].instruction_count &&
        bindings->block_index[n].edges[0]==alone->block_index[n].edges[0] &&
        bindings->block_index[n].edges[1]==alone->block_index[n].edges[1]);
    xr_lir_array_region_bindings_destroy(alone); xr_lir_array_region_bindings_destroy(bindings); bindings=NULL;
    CHECK(live==baseline && home_live_bytes()==bytes);
    if (!reverse && read_mask==1) for (size_t point=1;point<=sites;++point) {
        attempts=0; fail_at=point;
        CHECK(xr_lir_array_region_bindings_create(homes,set,&bindings)==XR_LIR_VALUE_LAYOUT_OOM && !bindings &&
            attempts>=point && live==baseline && home_live_bytes()==bytes);
    }
    fail_at=SIZE_MAX; HOME(xr_lir_array_region_bindings_create(homes,set,&bindings));
    xr_lir_value_homes_destroy(homes); xr_lir_ownership_plan_destroy(plan); xr_lir_value_layouts_destroy(set);
    xr_lir_module_destroy(c.module);
    CHECK(bindings->regions[winner].slot_count && bindings->regions[winner].region.primary_kind==XR_LIR_ARRAY_REGION_PRIMARY_SET &&
        !bindings->regions[skipped].slot_count && bindings->edges[edge-1].region==winner+1);
    xr_lir_array_region_bindings_destroy(bindings); CHECK(!live && !home_live_bytes());
    CHECK(xrt_billing_owner_stats(billing).blocks==1); xrt_billing_owner_drop(billing);
}
static void array_push_binding_slot(const XrLirArrayRegionBindings *bindings,const XrLirArrayRegionBinding *row,size_t slot) {
    bool found=false;
    for (size_t n=0;n<row->slot_count;++n) found|=bindings->slots[row->first_slot+n].slot==slot;
    CHECK(found);
}
static void array_push_binding_projection(void) {
    const enum ArrayPushWindowMode modes[]={PUSH_WINDOW_CURRENT,PUSH_WINDOW_PARAMETER_END,PUSH_WINDOW_MIRRORED_END,PUSH_WINDOW_ENTRY_EXIT_RETAINS,
        PUSH_WINDOW_EXIT_BRIDGE};
    for (size_t test=0;test<sizeof(modes)/sizeof(*modes);++test) {
        enum ArrayPushWindowMode mode=modes[test];
        ArraySetRegionFixture c=array_push_window_build((enum ArrayPushWindowMode)mode);
        XrtBillingOwner *billing=NULL; MANAGED(xrt_billing_owner_new(NULL,SIZE_MAX,&billing));
        XrLirValueLayoutKey roots[]={{XR_LIR_UNIT,0,0},{XR_LIR_I1,0,0},{XR_LIR_I64,0,0},{XR_LIR_ARRAY,XR_LIR_I64,0}};
        XrLirValueLayouts *set=NULL; HOME(xr_lir_value_layouts_create(c.module,roots,4,billing,&set));
        XrLirOwnershipPlan *plan=NULL; OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=c.module},&plan,NULL));
        XrLirValueHomes *homes=NULL;
        HOME(xr_lir_value_homes_create(&(XrLirValueHomesRequest){c.module,c.function,set,plan},&homes));
        CHECK(!homes->array_region_count && homes->array_push_window_count==1);
        XrLirArraySetRegion published={0}; OK(xr_lir_ownership_array_push_window_at(plan,0,&published));
        CHECK(ars_push_same(published.push_capacity,homes->array_push_windows[0].push_capacity));
        XrLirArrayRegionBindings *bindings=NULL; attempts=0;
        size_t baseline=live,bytes=home_live_bytes();
        HOME(xr_lir_array_push_window_bindings_create(homes,set,&bindings)); size_t sites=attempts;
        CHECK(bindings->region_count==1 && bindings->regions[0].slot_count && bindings->regions[0].access_count==1);
        const XrLirArrayRegionBinding *row=&bindings->regions[0];
        XrLirArrayPushCapacity retained=row->region.push_capacity;
        CHECK(ars_push_same(retained,published.push_capacity));
        for (size_t n=0;n<row->move_count;++n) {
            XrLirArraySetRegionStep step=bindings->moves[row->first_move+n].step; uint32_t values[]={step.source,step.target};
            for (size_t a=0;a<2;++a) {
                uint32_t v=values[a]; const XrLirHomeBinding *binding=homes->bindings ? &homes->bindings[v] : NULL;
                if (binding && binding->type) array_push_binding_slot(bindings,row,binding->first_owner);
                else {
                    array_push_binding_slot(bindings,row,homes->values[v].first_owner);
                    if (homes->values[v].temporary_first_owner!=SIZE_MAX)
                        array_push_binding_slot(bindings,row,homes->values[v].temporary_first_owner);
                }
            }
        }
        const XrLirArrayRegionAccess *push=&bindings->accesses[row->first_access]; bool workspace=false;
        for (size_t n=0;n<homes->workspace_count;++n) if (homes->workspaces[n].block==push->block &&
            homes->workspaces[n].instruction==push->instruction) {
            CHECK(bindings->slots[push->workspace_slot].slot==homes->workspaces[n].first_owner); workspace=true;
        }
        CHECK(workspace && push->operation==XR_LIR_ARRAY_OP_PUSH && push->item==retained.item);
        bool entry=false,latch=false;
        for (size_t n=0;n<homes->edge_count;++n) if (bindings->edges[n].region) {
            const XrLirArrayRegionMove *move=&bindings->moves[bindings->edges[n].move-1];
            CHECK(move->step.block==homes->edges[n].block && move->step.edge==homes->edges[n].edge);
            entry|=bindings->edges[n].kind==XR_LIR_ARRAY_REGION_EDGE_ENTRY;
            latch|=bindings->edges[n].kind==XR_LIR_ARRAY_REGION_EDGE_LATCH;
        }
        CHECK(entry && latch);
        xr_lir_array_region_bindings_destroy(bindings); bindings=NULL; CHECK(live==baseline && home_live_bytes()==bytes);
        if (!mode) for (size_t point=1;point<=sites;++point) {
            attempts=0; fail_at=point;
            CHECK(xr_lir_array_push_window_bindings_create(homes,set,&bindings)==XR_LIR_VALUE_LAYOUT_OOM && !bindings &&
                attempts>=point && live==baseline && home_live_bytes()==bytes);
        }
        fail_at=SIZE_MAX; HOME(xr_lir_array_push_window_bindings_create(homes,set,&bindings));
        xr_lir_value_homes_destroy(homes); xr_lir_ownership_plan_destroy(plan); xr_lir_value_layouts_destroy(set);
        xr_lir_module_destroy(c.module);
        CHECK(bindings->regions[0].slot_count && ars_push_same(bindings->regions[0].region.push_capacity,retained));
        xr_lir_array_region_bindings_destroy(bindings); CHECK(!live && !home_live_bytes());
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

static void array_region_binding_local_fallback(void) {
    /* Borrow the second Array before its loop. Only its entry owner fails the
     * closed-region proof. The stationary update starts after that call. */
    ArraySetRegionFixture c=array_region_binding_loops(false,false,true,0,true);
    XrLirOwnershipPlan *plan=NULL;
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=c.module},&plan,NULL));
    OK(xr_lir_ownership_array_regions_verify(c.module,plan));
    XrLirOwnershipFunction function;
    OK(xr_lir_ownership_function_at(plan,c.function.id,&function));
    CHECK(function.region_count==1 && function.fixed_update_count==1 && !function.push_window_count);
    XrLirArraySetRegion legacy,fixed;
    OK(xr_lir_ownership_array_region_at(plan,function.region_offset,&legacy));
    OK(xr_lir_ownership_fixed_array_update_at(plan,function.fixed_update_offset,&fixed));
    CHECK(legacy.entry_block==c.entry.id && legacy.header!=c.header.id &&
        legacy.primary_kind==XR_LIR_ARRAY_REGION_PRIMARY_SET &&
        legacy.range.kind==XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_SAME_LEN);
    CHECK(fixed.header==c.header.id && fixed.entry_block==legacy.exit_block && fixed.exit_block==c.exit.id &&
        fixed.primary_kind==XR_LIR_ARRAY_REGION_PRIMARY_SET &&
        fixed.range.kind==XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_SAME_LEN);

    XrtBillingOwner *billing=NULL; MANAGED(xrt_billing_owner_new(NULL,SIZE_MAX,&billing));
    XrLirValueLayoutKey roots[]={{XR_LIR_UNIT,0,0},{XR_LIR_I1,0,0},{XR_LIR_I64,0,0},{XR_LIR_ARRAY,XR_LIR_I64,0}};
    XrLirValueLayouts *set=NULL; HOME(xr_lir_value_layouts_create(c.module,roots,4,billing,&set));
    XrLirValueHomes *homes=NULL;
    HOME(xr_lir_value_homes_create(&(XrLirValueHomesRequest){c.module,c.function,set,plan},&homes));
    CHECK(homes->array_region_count==1 && homes->fixed_array_update_count==1);
    XrLirArrayRegionBindings *bindings=NULL;
    HOME(xr_lir_array_region_bindings_create(homes,set,&bindings));
    array_region_binding_check(bindings,homes,c.module,c.function);
    CHECK(bindings->regions[0].region.header==legacy.header);
    const XrLirFixedArrayUpdateHome *home=&homes->fixed_array_updates[0];
    CHECK(home->region.header==fixed.header && home->body_edge==1 && home->region.exit_edge==2);
    XrtArrayI64RegionSlot slots[XR_LIR_FIXED_ARRAY_GUARD_SLOT_COUNT];
    XrtArrayI64RegionSpec guard=xr_lir_fixed_array_guard_spec(homes,home,slots);
    CHECK(guard.slots==slots && guard.slot_count==2 && guard.canonical==0);
    CHECK(slots[0].slot==homes->values[fixed.canonical_value].first_owner &&
        slots[0].offset==homes->values[fixed.canonical_value].offset);
    CHECK(slots[1].slot==home->workspace_slot && slots[1].slot!=slots[0].slot &&
        slots[1].offset==homes->frame.owned_offsets[home->workspace_slot]);

    xr_lir_array_region_bindings_destroy(bindings); xr_lir_value_homes_destroy(homes);
    xr_lir_value_layouts_destroy(set); xr_lir_ownership_plan_destroy(plan); xr_lir_module_destroy(c.module);
    CHECK(!live && !home_live_bytes() && xrt_billing_owner_stats(billing).blocks==1);
    xrt_billing_owner_drop(billing);
}

static void array_region_binding_projection(unsigned shape) {
    if (!shape) {
        array_push_binding_projection();
        fixed_array_home_allocation_faults();
        array_region_binding_local_fallback();
    }
    if (!shape) for (unsigned mask=1;mask<=2;++mask) {
        array_region_binding_mixed(false,mask); array_region_binding_mixed(true,mask);
    }
    bool large=shape==1,shared=shape==2 || shape==3,reverse=shape==3,single_range=shape==4 || shape==6,range=shape>=4;
    ArraySetRegionFixture c=single_range ? ars_range_build(NULL,3,shape==6 ? ARRAY_REGION_RANGE_READ_ONLY : ARRAY_REGION_RANGE_UNIT) :
        large ? array_set_region_fixture(257,ARRAY_SET_REGION_LOCAL) : array_region_binding_two_loops(shared,reverse,range);
    if (single_range) OK(xr_lir_module_finish(c.module,NULL));
    XrtBillingOwner *billing=NULL; MANAGED(xrt_billing_owner_new(NULL,SIZE_MAX,&billing));
    XrLirValueLayoutKey roots[]={{XR_LIR_UNIT,0,0},{XR_LIR_I1,0,0},{XR_LIR_I64,0,0},{XR_LIR_ARRAY,XR_LIR_I64,0}};
    XrLirValueLayouts *set=NULL; HOME(xr_lir_value_layouts_create(c.module,roots,4,billing,&set));
    XrLirOwnershipPlan *plan=NULL; OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=c.module},&plan,NULL));
    XrLirValueHomes *homes=NULL;
    HOME(xr_lir_value_homes_create(&(XrLirValueHomesRequest){c.module,c.function,set,plan},&homes));
    CHECK(homes->array_region_count==(large || single_range ? 1u : 2u));
    XrLirOwnershipFunction owner; OK(xr_lir_ownership_function_at(plan,c.function.id,&owner));
    for (uint32_t n=0;n<homes->array_region_count;++n) {
        XrLirArraySetRegion published; OK(xr_lir_ownership_array_region_at(plan,owner.region_offset+n,&published));
        CHECK(ars_range_same(homes->array_regions[n].range,published.range));
        if (range) CHECK(published.range.kind==XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_SAME_LEN);
    }
    size_t baseline=live,bytes=home_live_bytes(); attempts=0;
    XrLirArrayRegionBindings *bindings=NULL; HOME(xr_lir_array_region_bindings_create(homes,set,&bindings));
    size_t sites=attempts; CHECK(sites && live>baseline);
    if (shared) array_region_binding_shared_check(bindings,homes,set,c.module,c.function,reverse);
    else array_region_binding_check(bindings,homes,c.module,c.function);
    if (!large && !shared && !single_range) CHECK(bindings->regions[1].first_slot && bindings->regions[1].first_move &&
        bindings->regions[1].first_access && bindings->regions[1].canonical_slot>=bindings->regions[1].first_slot);
    if (shape==5) CHECK(bindings->regions[1].region.first_step &&
        bindings->regions[0].region.range.iv!=bindings->regions[1].region.range.iv &&
        bindings->regions[0].region.range.length!=bindings->regions[1].region.range.length);
    XrLirArrayRegionBindings *preserved=bindings;
    CHECK(xr_lir_array_region_bindings_create(homes,set,&bindings)==XR_LIR_VALUE_LAYOUT_ARGUMENT && bindings==preserved);
    xr_lir_array_region_bindings_destroy(bindings); bindings=NULL;
    CHECK(live==baseline && home_live_bytes()==bytes);
    if (shape==6) {
        XrLirArraySetRegion region=homes->array_regions[0]; XrLirValueHomes view=*homes; view.array_regions=&region;
        uint32_t *fields[]={&region.primary_kind,&region.primary_block,&region.primary_instruction};
        for (size_t n=0;n<3;++n) {
            uint32_t saved=*fields[n]; *fields[n]^=1;
            CHECK(xr_lir_array_region_bindings_create(&view,set,&bindings)==XR_LIR_VALUE_LAYOUT_ARGUMENT && !bindings);
            *fields[n]=saved; CHECK(live==baseline && home_live_bytes()==bytes);
        }
        const uint32_t invalid_primary_kinds[]={XR_LIR_ARRAY_REGION_PRIMARY_NONE,XR_LIR_ARRAY_REGION_PRIMARY_SET,UINT32_MAX};
        for (size_t n=0;n<3;++n) {
            region.primary_kind=invalid_primary_kinds[n];
            CHECK(xr_lir_array_region_bindings_create(&view,set,&bindings)==XR_LIR_VALUE_LAYOUT_ARGUMENT && !bindings);
            CHECK(live==baseline && home_live_bytes()==bytes);
        }
    }
    for (size_t point=1;point<=sites;++point) {
        attempts=0; fail_at=point;
        CHECK(xr_lir_array_region_bindings_create(homes,set,&bindings)==XR_LIR_VALUE_LAYOUT_OOM &&
            !bindings && attempts>=point && live==baseline && home_live_bytes()==bytes);
    }
    fail_at=SIZE_MAX;
    if (!large && !shared) array_region_binding_unsupported(homes,set);
    HOME(xr_lir_array_region_bindings_create(homes,set,&bindings));
    size_t regions=bindings->region_count,moves=bindings->move_count,accesses=bindings->access_count;
    XrLirArraySetRegion first=bindings->regions[0].region;
    uint64_t range_digest=ars_range_digest(first.range);
    XrLirArraySetRegionRange retained_ranges[2]; CHECK(regions<=2);
    XrLirArraySetRegionOwnerHome retained_homes[2]; XrLirArrayRegionOwnerHomeBinding retained_projections[2];
    for (size_t n=0;n<regions;++n) {
        retained_ranges[n]=bindings->regions[n].region.range; retained_homes[n]=bindings->regions[n].region.owner_home;
        retained_projections[n]=bindings->regions[n].owner_home;
    }
    xr_lir_value_homes_destroy(homes); xr_lir_ownership_plan_destroy(plan);
    xr_lir_value_layouts_destroy(set); xr_lir_module_destroy(c.module);
    CHECK(bindings->region_count==regions && bindings->move_count==moves && bindings->access_count==accesses &&
        bindings->regions[0].region.canonical_value==first.canonical_value &&
        ars_range_same(bindings->regions[0].region.range,first.range) &&
        ars_range_digest(bindings->regions[0].region.range)==range_digest);
    for (size_t n=0;n<regions;++n) {
        const XrLirArrayRegionBinding *r=&bindings->regions[n];
        CHECK(ars_range_same(r->region.range,retained_ranges[n]) &&
            ars_range_digest(r->region.range)==ars_range_digest(retained_ranges[n]));
        CHECK(ars_owner_home_same(r->region.owner_home,retained_homes[n]) &&
            ars_owner_home_digest(r->region.owner_home)==ars_owner_home_digest(retained_homes[n]) &&
            r->owner_home.kind==retained_projections[n].kind && r->owner_home.stable_slot==retained_projections[n].stable_slot &&
            r->owner_home.exit_slot==retained_projections[n].exit_slot);
        if (!n) CHECK(r->region.primary_kind==first.primary_kind && r->region.primary_block==first.primary_block &&
            r->region.primary_instruction==first.primary_instruction);
        if (shared && n==1) CHECK(!r->slot_count);
        else CHECK(r->slot_count && r->canonical_slot<bindings->slot_count && r->first_move+r->move_count<=moves &&
            r->first_access+r->access_count<=accesses);
    }
    xr_lir_array_region_bindings_destroy(bindings); CHECK(!live && !home_live_bytes());
    CHECK(xrt_billing_owner_stats(billing).blocks==1); xrt_billing_owner_drop(billing);
    printf("Array region binding: %zu regions, %zu moves, %zu accesses, %zu allocation failures preserve output\n",
        regions,moves,accesses,sites);
}
#endif // LIR_ARRAY_REGION_BINDING_CASES_H
