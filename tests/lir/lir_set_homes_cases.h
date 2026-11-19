/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_set_homes_cases.h - Shared Set layout and exact binding-owner projections
 */
#include "lir_set_fixture.h"
#include "lir/xlir_verify.h"
#include "runtime/native/xnative_set_backing.h"
#include "lir_set_header_edge_fixture.h"
#include "lir_set_block_cell_fixture.h"
static void set_stage_transactions(const XrLirValueHomes *homes, XrtBillingOwner *billing) {
    CHECK(homes->transfer_count && homes->transfer_count<=2);
    XrtBackingTypes types={0}; MANAGED(xrt_backing_types_init(billing,&types));
    XrtManagedRoot *root=NULL; XrtManagedFrame *incoming=NULL,*frame=NULL;
    MANAGED(xrt_managed_root_new(billing,&root)); MANAGED(xrt_managed_frame_enter(root,1,&incoming));
    MANAGED(xrt_managed_frame_enter_layout(root,&homes->frame,&frame));
    const int64_t key=7; MANAGED(xrt_set_backing_create(incoming,0,&types,&key,1));
    XrtManagedHeader *formal=NULL; MANAGED(xrt_managed_slot_borrow(incoming,0,&formal));
    int32_t references=atomic_load(&formal->references);
    XrtPlaceEdge edges[2]={0}; XrtManagedHeader *previous[2]={0};
    for (size_t p=0;p<homes->transfer_count;++p) {
        XrLirHomeTransfer transfer=homes->transfers[p];
        const XrLirHomeWorkspace *stage=&homes->workspaces[transfer.borrowed_owner_workspace];
        const XrLirValueHome *target=&homes->values[transfer.target];
        int64_t old_key=90+(int64_t)p;
        MANAGED(xrt_set_backing_create(frame,target->first_owner,&types,&old_key,1));
        MANAGED(xrt_managed_slot_borrow(frame,target->first_owner,&previous[p]));
        edges[p]=(XrtPlaceEdge){
            .destination={.frame=frame,.slot=target->first_owner,.nulls=XRT_PLACE_NULLABLE},
            .source={.frame=frame,.slot=stage->first_owner,.nulls=XRT_PLACE_NONNULL},
            .temporary={.frame=frame,.slot=target->temporary_first_owner,.nulls=XRT_PLACE_NULLABLE},
            .transfer=XRT_PLACE_TRANSFER_MOVE};
    }
    XrtBillingStats baseline=xrt_billing_owner_stats(billing);
    xrt_billing_owner_fail_after(billing,0);
    for (size_t limit=0;limit<homes->transfer_count;++limit) {
        atomic_store(&formal->references,INT32_MAX-(int32_t)limit); size_t prepared=0;
        for (;prepared<homes->transfer_count;++prepared) {
            XrtManagedHeader *copy=formal;
            if (!xrt_managed_retain(copy)) break;
            MANAGED(xrt_managed_slot_adopt(frame,edges[prepared].source.slot,&copy)); CHECK(!copy);
        }
        CHECK(prepared==limit);
        while (prepared) MANAGED(xrt_managed_slot_drop(frame,edges[--prepared].source.slot));
        CHECK(atomic_load(&formal->references)==INT32_MAX-(int32_t)limit);
        atomic_store(&formal->references,references);
        for (size_t p=0;p<homes->transfer_count;++p) {
            XrtManagedHeader *owner=NULL,*stage=NULL,*temporary=NULL;
            MANAGED(xrt_managed_slot_borrow(frame,edges[p].destination.slot,&owner)); CHECK(owner==previous[p]);
            MANAGED(xrt_managed_slot_borrow(frame,edges[p].source.slot,&stage)); CHECK(!stage);
            MANAGED(xrt_managed_slot_borrow(frame,edges[p].temporary.slot,&temporary)); CHECK(!temporary);
        }
        XrtBillingStats current=xrt_billing_owner_stats(billing);
        CHECK(current.blocks==baseline.blocks && current.bytes==baseline.bytes);
    }
    for (unsigned invalid=1;;invalid=0) {
        for (size_t p=0;p<homes->transfer_count;++p) {
            XrtManagedHeader *copy=formal; CHECK(xrt_managed_retain(copy));
            MANAGED(xrt_managed_slot_adopt(frame,edges[p].source.slot,&copy)); CHECK(!copy);
        }
        XrtPlace temporary=edges[0].temporary;
        if (invalid) edges[0].temporary=edges[0].destination;
        XrtManagedStatus status=xrt_place_edge_transfer(edges,homes->transfer_count,NULL);
        edges[0].temporary=temporary;
        CHECK(status==(invalid ? XRT_MANAGED_ARGUMENT : XRT_MANAGED_OK));
        for (size_t p=homes->transfer_count;p;--p) {
            XrtManagedHeader *owner=NULL,*stage=NULL,*end=NULL;
            MANAGED(xrt_managed_slot_borrow(frame,edges[p-1].source.slot,&stage));
            CHECK(stage==(invalid ? formal : NULL));
            if (invalid) MANAGED(xrt_managed_slot_drop(frame,edges[p-1].source.slot));
            MANAGED(xrt_managed_slot_borrow(frame,edges[p-1].destination.slot,&owner));
            CHECK(owner==(invalid ? previous[p-1] : formal));
            MANAGED(xrt_managed_slot_borrow(frame,edges[p-1].temporary.slot,&end)); CHECK(!end);
        }
        if (!invalid) break;
        CHECK(atomic_load(&formal->references)==references);
        XrtBillingStats current=xrt_billing_owner_stats(billing);
        CHECK(current.blocks==baseline.blocks && current.bytes==baseline.bytes);
    }
    XrtManagedHeader *unchanged=NULL; MANAGED(xrt_managed_slot_borrow(incoming,0,&unchanged)); CHECK(unchanged==formal);
    CHECK(atomic_load(&formal->references)==references+(int32_t)homes->transfer_count);
    xrt_billing_owner_fail_after(billing,SIZE_MAX);
    MANAGED(xrt_managed_root_destroy(&root)); xrt_backing_types_drop(&types);
}
static void set_block_cells(void) {
    for (unsigned invalid=0;invalid<4;++invalid) {
        SetLirFixture c=set_block_cell_fixture(invalid); XrLirDiagnostic diagnostic={0};
        XrLirStatus status=xr_lir_module_finish(c.module,&diagnostic);
        if (status!=(invalid ? XR_LIR_VERIFY_ERROR : XR_LIR_OK))
            fprintf(stderr,"Set block Cell mode %u status %u: %s\n",invalid,(unsigned)status,diagnostic.message);
        CHECK(status==(invalid ? XR_LIR_VERIFY_ERROR : XR_LIR_OK));
        if (invalid==1) CHECK(strstr(diagnostic.message,"reconstruction requires an empty lifetime"));
        if (invalid>=2) CHECK(strstr(diagnostic.message,"use follows release"));
        if (!invalid) OK(xr_lir_verify(c.module,NULL));
        xr_lir_module_destroy(c.module);
    }
}
static void set_borrowed_edges(void) {
    for (unsigned mode=0;mode<3;++mode) {
        SetLirFixture c=set_header_edge_fixture(mode); XrLirDiagnostic diagnostic={0};
        XrLirStatus status=xr_lir_module_finish(c.module,&diagnostic);
        if (status!=(mode==2 ? XR_LIR_VERIFY_ERROR : XR_LIR_OK))
            fprintf(stderr,"Set Header edge mode %u status %u: %s\n",mode,(unsigned)status,diagnostic.message);
        CHECK(status==(mode==2 ? XR_LIR_VERIFY_ERROR : XR_LIR_OK));
        if (mode==2) {
            CHECK(strstr(diagnostic.message,"duplicates one owned reference"));
            xr_lir_module_destroy(c.module); continue;
        }
        OK(xr_lir_verify(c.module,NULL));
        XrtBillingOwner *billing=NULL; MANAGED(xrt_billing_owner_new(NULL,SIZE_MAX,&billing));
        XrLirValueLayoutKey roots[]={{XR_LIR_I64,0,0},{XR_LIR_SET_I64,0,0}};
        XrLirValueLayouts *layouts=NULL; HOME(xr_lir_value_layouts_create(c.module,roots,2,billing,&layouts));
        XrLirOwnershipPlan *plan=NULL;
        OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=c.module},&plan,NULL));
        XrLirValueHomesRequest request={c.module,c.read,layouts,plan}; XrLirValueHomes *homes=NULL;
        size_t baseline=live,baseline_bytes=home_live_bytes(); attempts=0;
        HOME(xr_lir_value_homes_create(&request,&homes)); size_t sites=attempts;
        const XrLirValueHome *formal=&homes->values[1];
        CHECK(formal->parameter==1 && formal->owner_count==1 && formal->size==sizeof(XrtManagedHeader *) &&
            formal->offset==SIZE_MAX && formal->first_owner==SIZE_MAX && formal->temporary_offset==SIZE_MAX &&
            formal->temporary_first_owner==SIZE_MAX);
        CHECK(homes->edge_count==1 && homes->transfer_count==mode+1 && homes->owned_frame_needed);
        for (size_t t=0;t<homes->transfer_count;++t) {
            const XrLirHomeTransfer *transfer=&homes->transfers[t];
            CHECK(transfer->source==1 && transfer->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_COPY &&
                transfer->hold_transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_NONE &&
                transfer->borrowed_owner_workspace<homes->workspace_count);
            const XrLirHomeWorkspace *stage=&homes->workspaces[transfer->borrowed_owner_workspace];
            const XrLirValueHome *target=&homes->values[transfer->target];
            CHECK(stage->kind==XR_LIR_HOME_WORKSPACE_HEADER_BORROW_STAGE && stage->value==1 &&
                stage->type==formal->type && stage->block==homes->edges[0].block && stage->instruction &&
                stage->first_owner<homes->frame.slot_count && homes->frame.owned_offsets[stage->first_owner]==stage->offset &&
                stage->temporary_offset==SIZE_MAX && stage->temporary_first_owner==SIZE_MAX &&
                stage->first_owner!=target->first_owner && stage->first_owner!=target->temporary_first_owner);
            for (size_t p=0;p<t;++p) {
                const XrLirHomeWorkspace *previous=&homes->workspaces[homes->transfers[p].borrowed_owner_workspace];
                CHECK(previous->first_owner!=stage->first_owner && previous->offset!=stage->offset);
            }
        }
        xr_lir_value_homes_destroy(homes); homes=NULL;
        CHECK(live==baseline && home_live_bytes()==baseline_bytes);
        for (size_t n=1;n<=sites;++n) {
            attempts=0; fail_at=n;
            CHECK(xr_lir_value_homes_create(&request,&homes)==XR_LIR_VALUE_LAYOUT_OOM && !homes &&
                live==baseline && home_live_bytes()==baseline_bytes);
        }
        fail_at=SIZE_MAX; HOME(xr_lir_value_homes_create(&request,&homes));
        set_stage_transactions(homes,billing);
        size_t stages=homes->transfer_count,stage_offset=homes->workspaces[homes->transfers[0].borrowed_owner_workspace].offset;
        xr_lir_ownership_plan_destroy(plan); xr_lir_value_layouts_destroy(layouts); xr_lir_module_destroy(c.module);
        CHECK(homes->transfer_count==stages && homes->values[1].offset==SIZE_MAX &&
            homes->workspaces[homes->transfers[0].borrowed_owner_workspace].offset==stage_offset);
        xr_lir_value_homes_destroy(homes); CHECK(live==baseline && home_live_bytes()==baseline_bytes);
        CHECK(xrt_billing_owner_stats(billing).blocks==1); xrt_billing_owner_drop(billing);
    }
}
static void set_owner_rejections(void) {
    for (unsigned borrowed=0;borrowed<2;++borrowed) {
        SetLirFixture c={0}; OK(xr_lir_module_create(NULL,&c.module));
        XrLirType parameter=XR_LIR_SET_I64; XrLirBlock block={0};
        XrLirFunctionSpec spec={.name="invalid_set",.name_length=11,.parameters=borrowed?&parameter:NULL,
            .parameter_count=borrowed?1u:0u,.result=borrowed?XR_LIR_SET_I64:XR_LIR_I64};
        c.entry=set_fixture_function(&c,&spec,&block); XrLirValue value={0};
        if (borrowed) OK(xr_lir_function_parameter(c.module,c.entry,0,&value));
        else {
            value=set_fixture_value(block,(XrLirInstruction){.op=XR_LIR_SET_NEW,.type=XR_LIR_SET_I64});
            set_fixture_effect(block,(XrLirInstruction){.op=XR_LIR_SET_DROP,.type=XR_LIR_UNIT,.operands=&value,.operand_count=1});
            value=set_fixture_value(block,(XrLirInstruction){.op=XR_LIR_SET_LEN,.type=XR_LIR_I64,.operands=&value,.operand_count=1});
        }
        set_fixture_return(block,value); XrLirDiagnostic diagnostic={0};
        CHECK(xr_lir_module_finish(c.module,&diagnostic)==XR_LIR_VERIFY_ERROR);
        CHECK(!strcmp(diagnostic.message,borrowed ? "Managed transfer requires an owned reference" :
            "Managed use follows release or precedes ownership"));
        xr_lir_module_destroy(c.module);
    }
}
static void set_homes(void) {
    set_block_cells();
    set_borrowed_edges();
    set_owner_rejections();
    for (unsigned state=0;state<2;++state) {
        SetLirFixture c=set_lir_fixture(state!=0); OK(xr_lir_module_finish(c.module,NULL));
        XrtBillingOwner *billing=NULL; MANAGED(xrt_billing_owner_new(NULL,SIZE_MAX,&billing));
        XrLirValueLayoutKey roots[]={{XR_LIR_UNIT,0,0},{XR_LIR_I64,0,0},{XR_LIR_SET_I64,0,0}};
        XrLirValueLayouts *layouts=NULL; HOME(xr_lir_value_layouts_create(c.module,roots,3,billing,&layouts));
        const XrtValuePlan *set=NULL; uint32_t set_id=0; XrtValuePlanSpec spec;
        HOME(xr_lir_value_layouts_lookup(layouts,roots[2],&set,&set_id)); MANAGED(xrt_value_plan_view(set,&spec));
        CHECK(spec.kind==XRT_VALUE_SET_I64 && spec.size==8 && spec.alignment==8 && spec.owned_count==1 && !spec.owned_offsets[0]);
        XrLirOwnershipPlan *ownership=NULL;
        OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=c.module},&ownership,NULL));
        XrLirFunction functions[]={c.make,c.read,c.write,c.entry};
        for (size_t f=0;f<4;++f) {
            XrLirValueHomesRequest request={c.module,functions[f],layouts,ownership}; XrLirValueHomes *homes=NULL;
            attempts=0; HOME(xr_lir_value_homes_create(&request,&homes)); size_t allocations=attempts;
            if (functions[f].id==c.read.id) {
                CHECK(!homes->owned_frame_needed && !homes->frame.slot_count && !homes->workspace_count);
                const XrLirValueHome *formal=&homes->values[1];
                CHECK(formal->parameter==1 && formal->type==set_id && formal->offset==SIZE_MAX && formal->temporary_offset==SIZE_MAX);
            } else if (functions[f].id==c.write.id) {
                CHECK(homes->bindings && homes->bindings[1].kind==XR_LIR_HOME_BINDING_PARAMETER &&
                    homes->bindings[1].parameter==1 && homes->bindings[1].type==set_id &&
                    homes->bindings[1].offset==SIZE_MAX && homes->bindings[1].first_owner==SIZE_MAX);
                CHECK(homes->workspace_count==1 && homes->workspaces[0].type==set_id &&
                    homes->workspaces[0].kind==XR_LIR_HOME_WORKSPACE_VALUE &&
                    homes->workspaces[0].first_owner<homes->frame.slot_count);
                bool actual=false;
                for (size_t e=0;e<homes->event_count;++e) {
                    const XrLirHomeEvent *event=&homes->events[e];
                    if (event->kind!=XR_LIR_OWNERSHIP_SET_OPERATION) continue;
                    CHECK(event->operation==XR_LIR_SET_ADD && event->source==1 && homes->bindings[event->source].type==set_id);
                    actual=true;
                }
                CHECK(actual);
            } else if (functions[f].id==c.entry.id) {
                CHECK(homes->bindings && homes->bindings[c.binding.id].type==set_id &&
                    homes->bindings[c.binding.id].source==c.reference.id);
                CHECK(homes->bindings[c.binding.id].kind==(state ? XR_LIR_HOME_BINDING_STATE_BORROW : XR_LIR_HOME_BINDING_CELL_BORROW));
                if (state) CHECK(homes->bindings[c.reference.id].kind==XR_LIR_HOME_BINDING_STATE_BORROW &&
                    homes->bindings[c.reference.id].source==c.reference.id && homes->bindings[c.reference.id].offset==SIZE_MAX);
            }
            xr_lir_value_homes_destroy(homes); homes=NULL; CHECK(!live);
            for (size_t allowed=1;allowed<=allocations;++allowed) {
                attempts=0; fail_at=allowed;
                CHECK(xr_lir_value_homes_create(&request,&homes)==XR_LIR_VALUE_LAYOUT_OOM && !homes && !live);
            }
            fail_at=SIZE_MAX;
        }
        xr_lir_ownership_plan_destroy(ownership); xr_lir_value_layouts_destroy(layouts); xr_lir_module_destroy(c.module);
        CHECK(xrt_billing_owner_stats(billing).blocks==1); xrt_billing_owner_drop(billing);
    }
}
