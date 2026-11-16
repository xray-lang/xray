/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_slice_alloc_cases.h - Genuine Slice landings and staged-edge fault recovery
 */
#include "../lir/lir_slice_boundary_root_fixture.h"
typedef struct SliceFaultObservation {
    uint32_t fanout;
    size_t returns,retains,fail_return,fail_retain;
    bool pending_return,pending_edge,checked;
    XrtInlineValueHome landing;
    XrtArraySliceBundle old_landing,old_edge[2];
    RbcValueActivation *edge_activation;
    XrtManagedHeader *backing;
    int32_t references;
    XrtBillingStats before_edge;
} SliceFaultObservation;
static SliceFaultObservation slice_fault;
static bool slice_zero_bundle(const void *bytes) {
    const unsigned char zero[sizeof(XrtArraySliceBundle)]={0};
    return memcmp(bytes,zero,sizeof(zero))==0;
}
static XrtManagedStatus slice_observe_inline(const XrtInlineValueHome *home,const XrtInlineValueSpec *spec,const void *source) {
    if (!slice_running_execution || !source || spec->size!=sizeof(XrtArraySliceBundle) ||
        spec->owned_count!=1 || spec->owned_offsets[0]!=offsetof(XrtArraySliceBundle,hold))
        return xrt_inline_value_replace(home,spec,source);
    ++slice_fault.returns;
    if (slice_fault.returns!=slice_fault.fail_return) return xrt_inline_value_replace(home,spec,source);
    /* Seed the actual caller landing through the same valid COPY kernel. This
     * covers a reused nonempty landing rather than only a vacant target. */
    XrtBillingOwner *billing=slice_running_execution->stack.values->billing;
    XrtBillingStats before=xrt_billing_owner_stats(billing);
    CHECK(xrt_inline_value_replace(home,spec,source)==XRT_MANAGED_OK);
    slice_fault.landing=*home; memcpy(&slice_fault.old_landing,home->value,sizeof(slice_fault.old_landing));
    slice_fault.backing=((const XrtArraySliceBundle *)source)->hold;
    CHECK(slice_fault.backing && slice_fault.old_landing.hold==slice_fault.backing);
    slice_fault.references=atomic_load(&slice_fault.backing->references);
    atomic_store(&slice_fault.backing->references,-INT32_MAX);
    XrtManagedStatus status=xrt_inline_value_replace(home,spec,source);
    atomic_store(&slice_fault.backing->references,slice_fault.references);
    CHECK(status==XRT_MANAGED_REFCOUNT && memcmp(home->value,&slice_fault.old_landing,sizeof(slice_fault.old_landing))==0 &&
        slice_zero_bundle(home->temporary));
    XrtBillingStats after=xrt_billing_owner_stats(billing);
    CHECK(before.allocations==after.allocations && before.blocks==after.blocks && before.bytes==after.bytes);
    slice_fault.pending_return=true; return status;
}
static void slice_seed_edge(RbcValueActivation *activation) {
    const XrLirValueHomes *homes=activation->code->homes;
    CHECK(homes->edge_count==1 && homes->edges[0].count==2);
    const XrLirHomeEdge *edge=&homes->edges[0];
    XrtInlineValueSpec spec=xrt_array_slice_bundle_spec();
    for (size_t n=0;n<edge->count;++n) {
        XrLirHomeTransfer transfer=homes->transfers[edge->first+n];
        RbcValuePayload payload;
        CHECK(rbc_value_payload(slice_running_execution->module,&slice_running_execution->stack,
            rbc_value_origin(activation,transfer.source),&payload)==XRT_MANAGED_OK &&
            payload.size==sizeof(XrtReadonlyArraySlice) && !payload.lanes);
        XrtArraySliceBundle source={0}; memcpy(&source.value,payload.address,sizeof(source.value));
        source.hold=(XrtManagedHeader *)source.value.backing;
        XrtInlineValueHome target={0}; CHECK(rbc_value_slice_storage(activation,transfer.target_workspace,&target)==XRT_MANAGED_OK);
        /* Existing real destinations model the next loop visit. Neither their
         * storage nor their owning positions are fabricated by this test. */
        CHECK(xrt_inline_value_replace(&target,&spec,&source)==XRT_MANAGED_OK);
        memcpy(&slice_fault.old_edge[n],target.value,sizeof(source));
    }
    slice_fault.edge_activation=activation;
}
static bool slice_observe_retain(XrtManagedHeader *header) {
    RbcValueActivation *activation=slice_running_execution && slice_running_execution->stack.values ?
        slice_running_execution->stack.values->top : NULL;
    if (!activation || activation->code->homes->source_function!=slice_fault.fanout) return xrt_managed_retain(header);
    ++slice_fault.retains;
    if (!slice_fault.fail_retain) return xrt_managed_retain(header);
    if (slice_fault.retains==1) {
        slice_fault.before_edge=xrt_billing_owner_stats(slice_running_execution->stack.values->billing);
        slice_seed_edge(activation); slice_fault.backing=header;
        slice_fault.references=atomic_load(&header->references);
    }
    if (slice_fault.retains!=slice_fault.fail_retain) return xrt_managed_retain(header);
    CHECK(header==slice_fault.backing);
    int32_t references=atomic_load(&header->references); atomic_store(&header->references,-INT32_MAX);
    bool retained=xrt_managed_retain(header); atomic_store(&header->references,references);
    CHECK(!retained); slice_fault.pending_edge=true; return retained;
}
static _Noreturn void slice_observe_failure(XrtManagedStatus status,uint32_t source) {
    if (slice_running_execution && slice_fault.pending_return) {
        CHECK(status==XRT_MANAGED_REFCOUNT &&
            memcmp(slice_fault.landing.value,&slice_fault.old_landing,sizeof(slice_fault.old_landing))==0 &&
            slice_zero_bundle(slice_fault.landing.temporary) &&
            atomic_load(&slice_fault.backing->references)==slice_fault.references);
        slice_fault.checked=true;
    }
    if (slice_running_execution && slice_fault.pending_edge) {
        CHECK(status==XRT_MANAGED_REFCOUNT && slice_fault.edge_activation);
        RbcValueActivation *activation=slice_fault.edge_activation;
        const XrLirValueHomes *homes=activation->code->homes; const XrLirHomeEdge *edge=&homes->edges[0];
        for (size_t n=0;n<edge->count;++n) {
            XrLirHomeTransfer transfer=homes->transfers[edge->first+n];
            const XrLirHomeWorkspace *stage=rbc_value_slice_stage(activation->code,transfer);
            XrtManagedHeader *owner=NULL; CHECK(stage &&
                xrt_managed_slot_borrow(activation->frame,stage->first_owner,&owner)==XRT_MANAGED_OK && !owner);
            XrtInlineValueHome target={0}; CHECK(rbc_value_slice_storage(activation,transfer.target_workspace,&target)==XRT_MANAGED_OK &&
                memcmp(target.value,&slice_fault.old_edge[n],sizeof(slice_fault.old_edge[n]))==0 &&
                slice_zero_bundle(target.temporary));
        }
        XrtBillingStats after=xrt_billing_owner_stats(slice_running_execution->stack.values->billing);
        CHECK(after.allocations==slice_fault.before_edge.allocations && after.blocks==slice_fault.before_edge.blocks &&
            after.bytes==slice_fault.before_edge.bytes &&
            atomic_load(&slice_fault.backing->references)==slice_fault.references); slice_fault.checked=true;
    }
    xrt_native_managed_failure(status,source);
}
static void slice_alloc_outcome(XrRbcOutcome *outcome,bool success,XrXirValueStatus expected) {
    CHECK(outcome); XrRbcStatus status=xr_rbc_outcome_status(outcome);
    if (success) { uint64_t bits=0; CHECK(status==XR_RBC_OK && xr_rbc_outcome_bits(outcome,&bits) && bits==68); }
    else if (status==XR_RBC_VALUE_FAILURE) {
        XrXirValueStatus failure; uint32_t source; const XrtLocationOwner *locations;
        CHECK(xr_rbc_outcome_value_failure(outcome,&failure,&source,&locations) && failure==expected);
    } else CHECK(expected==XR_XIR_VALUE_OOM && status==XR_RBC_OUT_OF_MEMORY);
}
static void slice_billing_faults(XrRbcProgram *program,uint32_t fanout,size_t baseline) {
    XrtBillingOwner *control=NULL; CHECK(xrt_billing_owner_new(NULL,SIZE_MAX,&control)==XRT_MANAGED_OK);
    XrtBillingStats empty=xrt_billing_owner_stats(control); CHECK(empty.blocks==1);
    size_t failures=0;
    for (size_t allowance=0;;++allowance) {
        XrRbcExecution *execution=NULL; CHECK(xr_rbc_program_prepare(program,NULL,0,&execution)==XR_RBC_OK);
        XrtBillingOwner *billing=NULL;
        CHECK(xrt_native_values_prepare(&execution->values,&billing)==XRT_MANAGED_OK && xrt_billing_owner_retain(billing));
        xrt_billing_owner_fail_after(billing,allowance);
        slice_fault=(SliceFaultObservation){.fanout=fanout}; slice_running_execution=execution;
        XrRbcOutcome *outcome=xr_rbc_execution_run(execution); slice_running_execution=NULL;
        bool success=xr_rbc_outcome_status(outcome)==XR_RBC_OK;
        slice_alloc_outcome(outcome,success,XR_XIR_VALUE_OOM);
        CHECK(!execution->stack.values && !execution->state && xrt_value_root_empty(&execution->values));
        xrt_billing_owner_fail_after(billing,SIZE_MAX);
        xr_rbc_execution_free(&execution); xr_rbc_outcome_free(&outcome);
        XrtBillingStats retired=xrt_billing_owner_stats(billing);
        CHECK(retired.blocks==empty.blocks && retired.bytes==empty.bytes);
        xrt_billing_owner_drop(billing); CHECK(live_allocations==baseline);
        if (success) break;
        ++failures; CHECK(allowance<SIZE_MAX);
    }
    CHECK(failures); xrt_billing_owner_drop(control);
    printf("Slice actual native billing failures %zu refund to the live owner baseline, then owner retired\n",failures);
}
static void slice_boundary_allocation_failures(void) {
    const XrLirType elements[]={XR_LIR_I64,XR_LIR_U8};
    for (size_t family=0;family<2;++family) {
        SliceBoundaryFixture witness=slice_boundary_fixture(elements[family],false);
        OK(xr_lir_module_finish(witness.module,NULL)); xr_lir_module_destroy(witness.module);
        XrLirFunction root={0}; SliceBoundaryFixture c=slice_boundary_root_fixture(elements[family],&root);
        uint32_t fanout=c.fanout.id; OK(xr_lir_module_finish(c.module,NULL));
        XrRbcProgram *program=NULL; CHECK(xr_rbc_compile(c.module,root,&program,NULL)==XR_RBC_COMPILE_OK);
        xr_lir_module_destroy(c.module); size_t baseline=live_allocations;
        XrRbcExecution *execution=NULL; attempts=0;
        CHECK(xr_rbc_program_prepare(program,NULL,0,&execution)==XR_RBC_OK); size_t prepare_calls=attempts;
        attempts=0; slice_fault=(SliceFaultObservation){.fanout=fanout}; slice_running_execution=execution;
        XrRbcOutcome *outcome=xr_rbc_execution_run(execution); slice_running_execution=NULL;
        size_t run_calls=attempts,returns=slice_fault.returns,retains=slice_fault.retains;
        slice_alloc_outcome(outcome,true,XR_XIR_VALUE_OOM);
        xr_rbc_execution_free(&execution); xr_rbc_outcome_free(&outcome);
        CHECK(prepare_calls && run_calls && returns && retains==2 && live_allocations==baseline);
        for (size_t point=1;point<=prepare_calls;++point) {
            attempts=0; fail_at=point;
            CHECK(xr_rbc_program_prepare(program,NULL,0,&execution)==XR_RBC_OUT_OF_MEMORY && !execution);
            fail_at=SIZE_MAX; CHECK(attempts>=point && live_allocations==baseline);
        }
        for (size_t point=1;point<=run_calls;++point) {
            CHECK(xr_rbc_program_prepare(program,NULL,0,&execution)==XR_RBC_OK);
            attempts=0; fail_at=point; slice_fault=(SliceFaultObservation){.fanout=fanout}; slice_running_execution=execution;
            outcome=xr_rbc_execution_run(execution); slice_running_execution=NULL; fail_at=SIZE_MAX;
            slice_alloc_outcome(outcome,false,XR_XIR_VALUE_OOM);
            xr_rbc_execution_free(&execution); xr_rbc_outcome_free(&outcome);
            CHECK(attempts>=point && live_allocations==baseline);
        }
        slice_billing_faults(program,fanout,baseline);
        /* Landing replacement and selected-edge staging allocate nothing.
         * Inject their reachable retain limit, not an imaginary malloc OOM. */
        for (size_t point=1;point<=returns+retains;++point) {
            CHECK(xr_rbc_program_prepare(program,NULL,0,&execution)==XR_RBC_OK);
            XrtBillingOwner *billing=NULL,*control=NULL;
            CHECK(xrt_native_values_prepare(&execution->values,&billing)==XRT_MANAGED_OK && xrt_billing_owner_retain(billing) &&
                xrt_billing_owner_new(NULL,SIZE_MAX,&control)==XRT_MANAGED_OK);
            XrtBillingStats empty=xrt_billing_owner_stats(control);
            slice_fault=(SliceFaultObservation){.fanout=fanout,
                .fail_return=point<=returns ? point : 0,.fail_retain=point>returns ? point-returns : 0};
            slice_running_execution=execution; outcome=xr_rbc_execution_run(execution); slice_running_execution=NULL;
            slice_alloc_outcome(outcome,false,XR_XIR_VALUE_REFCOUNT_LIMIT); CHECK(slice_fault.checked);
            CHECK(!execution->stack.values && !execution->state && xrt_value_root_empty(&execution->values));
            xr_rbc_execution_free(&execution); xr_rbc_outcome_free(&outcome);
            XrtBillingStats retired=xrt_billing_owner_stats(billing);
            CHECK(retired.blocks==empty.blocks && retired.bytes==empty.bytes);
            xrt_billing_owner_drop(billing); xrt_billing_owner_drop(control); CHECK(live_allocations==baseline);
        }
        XrRbcExecution *retired_failure=NULL;
        CHECK(xr_rbc_program_prepare(program,NULL,0,&execution)==XR_RBC_OK &&
            xr_rbc_program_prepare(program,NULL,0,&retired_failure)==XR_RBC_OK);
        XrtBillingOwner *billing=NULL,*control=NULL;
        CHECK(xrt_native_values_prepare(&retired_failure->values,&billing)==XRT_MANAGED_OK &&
            xrt_billing_owner_retain(billing) && xrt_billing_owner_new(NULL,SIZE_MAX,&control)==XRT_MANAGED_OK);
        XrtBillingStats empty=xrt_billing_owner_stats(control); xr_rbc_program_destroy(program);
        slice_fault=(SliceFaultObservation){.fanout=fanout}; slice_running_execution=execution;
        outcome=xr_rbc_execution_run(execution); slice_running_execution=NULL;
        xr_rbc_execution_free(&execution); slice_alloc_outcome(outcome,true,XR_XIR_VALUE_OOM); xr_rbc_outcome_free(&outcome);
        slice_fault=(SliceFaultObservation){.fanout=fanout,.fail_retain=2}; slice_running_execution=retired_failure;
        outcome=xr_rbc_execution_run(retired_failure); slice_running_execution=NULL;
        slice_alloc_outcome(outcome,false,XR_XIR_VALUE_REFCOUNT_LIMIT); CHECK(slice_fault.checked);
        CHECK(!retired_failure->stack.values && !retired_failure->state && xrt_value_root_empty(&retired_failure->values));
        xr_rbc_execution_free(&retired_failure); xr_rbc_outcome_free(&outcome);
        XrtBillingStats retired=xrt_billing_owner_stats(billing);
        CHECK(retired.blocks==empty.blocks && retired.bytes==empty.bytes);
        xrt_billing_owner_drop(billing); xrt_billing_owner_drop(control);
        CHECK(!live_allocations); attempts=0;
        printf("Slice family%zu prepare/run OOM %zu/%zu, landing/edge retain faults %zu/%zu, retired creators PASS\n",
            family,prepare_calls,run_calls,returns,retains);
    }
}
