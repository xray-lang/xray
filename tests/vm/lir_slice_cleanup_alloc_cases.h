/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef LIR_SLICE_CLEANUP_ALLOC_CASES_H
#define LIR_SLICE_CLEANUP_ALLOC_CASES_H
#include "../lir/lir_slice_cleanup_fixture.h"

static void slice_cleanup_billing(XrLirType element,SliceCleanupCase shape) {
    SliceCleanupFixture c=slice_cleanup_fixture(element,shape,true);
    OK(xr_lir_module_finish(c.module,NULL));XrRbcProgram *program=NULL;XrRbcCompileDiagnostic diagnostic={0};
    CHECK(xr_rbc_compile(c.module,c.root,&program,&diagnostic)==XR_RBC_COMPILE_OK && program);
    xr_lir_module_destroy(c.module);size_t baseline=live_allocations;
    XrtBillingOwner *control=NULL;CHECK(xrt_billing_owner_new(NULL,SIZE_MAX,&control)==XRT_MANAGED_OK);
    XrtBillingStats empty=xrt_billing_owner_stats(control);size_t failures=0;
    for (size_t allowance=0;;++allowance) {
        XrRbcExecution *execution=NULL;CHECK(xr_rbc_program_prepare(program,NULL,0,&execution)==XR_RBC_OK);
        XrtBillingOwner *billing=NULL;
        CHECK(xrt_native_values_prepare(&execution->values,&billing)==XRT_MANAGED_OK && xrt_billing_owner_retain(billing));
        xrt_billing_owner_fail_after(billing,allowance);XrRbcOutcome *outcome=xr_rbc_execution_run(execution);
        CHECK(outcome);bool success=xr_rbc_outcome_status(outcome)==XR_RBC_OK;
        if (success) {
            uint64_t bits=0;CHECK(xr_rbc_outcome_bits(outcome,&bits) && bits==(uint64_t)c.expected);
        } else {
            XrXirValueStatus failure=XR_XIR_VALUE_OK;uint32_t source=UINT32_MAX;const XrtLocationOwner *locations=NULL;
            CHECK(xr_rbc_outcome_status(outcome)==XR_RBC_VALUE_FAILURE &&
                xr_rbc_outcome_value_failure(outcome,&failure,&source,&locations) && failure==XR_XIR_VALUE_OOM);
        }
        CHECK(!execution->stack.values && !execution->state && xrt_value_root_empty(&execution->values));
        xrt_billing_owner_fail_after(billing,SIZE_MAX);xr_rbc_execution_free(&execution);xr_rbc_outcome_free(&outcome);
        XrtBillingStats retired=xrt_billing_owner_stats(billing);
        CHECK(retired.bytes==empty.bytes && retired.blocks==empty.blocks);
        xrt_billing_owner_drop(billing);CHECK(live_allocations==baseline);
        if (success) break;
        ++failures;CHECK(allowance<SIZE_MAX);
    }
    CHECK(failures);
    XrRbcExecution *success=NULL,*failure=NULL;
    CHECK(xr_rbc_program_prepare(program,NULL,0,&success)==XR_RBC_OK &&
        xr_rbc_program_prepare(program,NULL,0,&failure)==XR_RBC_OK);
    XrtBillingOwner *billing=NULL;
    CHECK(xrt_native_values_prepare(&failure->values,&billing)==XRT_MANAGED_OK && xrt_billing_owner_retain(billing));
    xr_rbc_program_destroy(program);
    XrRbcOutcome *outcome=xr_rbc_execution_run(success);uint64_t bits=0;
    CHECK(outcome && xr_rbc_outcome_status(outcome)==XR_RBC_OK &&
        xr_rbc_outcome_bits(outcome,&bits) && bits==(uint64_t)c.expected);
    xr_rbc_execution_free(&success);xr_rbc_outcome_free(&outcome);
    xrt_billing_owner_fail_after(billing,0);outcome=xr_rbc_execution_run(failure);
    XrXirValueStatus status=XR_XIR_VALUE_OK;uint32_t source=UINT32_MAX;const XrtLocationOwner *locations=NULL;
    CHECK(outcome && xr_rbc_outcome_status(outcome)==XR_RBC_VALUE_FAILURE &&
        xr_rbc_outcome_value_failure(outcome,&status,&source,&locations) && status==XR_XIR_VALUE_OOM);
    CHECK(!failure->stack.values && !failure->state && xrt_value_root_empty(&failure->values));
    xrt_billing_owner_fail_after(billing,SIZE_MAX);xr_rbc_execution_free(&failure);xr_rbc_outcome_free(&outcome);
    XrtBillingStats retired=xrt_billing_owner_stats(billing);
    CHECK(retired.bytes==empty.bytes && retired.blocks==empty.blocks);
    xrt_billing_owner_drop(billing);xrt_billing_owner_drop(control);CHECK(!live_allocations);
}
static void slice_cleanup_allocation_failures(void) {
    const XrLirType elements[]={XR_LIR_I64,XR_LIR_U8};
    for (size_t family=0;family<2;++family) {
        SliceCleanupFixture c=slice_cleanup_fixture(elements[family],SLICE_CLEANUP_CALL_BEFORE,true);
        OK(xr_lir_module_finish(c.module,NULL));
        callable_allocation_failures(c.module,c.root,(uint64_t)c.expected,"Slice cleanup with real return landing");
        slice_cleanup_billing(elements[family],SLICE_CLEANUP_CALL_BEFORE);
        slice_cleanup_billing(elements[family],SLICE_CLEANUP_CALL_AFTER);
    }
}
#endif
