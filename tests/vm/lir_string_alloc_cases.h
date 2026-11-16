/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_string_alloc_cases.h - String binding references survive faults and creator retirement
 */
#include "../lir/lir_string_fixture.h"
static void string_reference_allocation_failures(void) {
    XrLirModule *module=NULL;OK(string_ref_fixture(&module));
    XrLirFunction root={0};OK(xr_lir_function_at(module,0,&root));
    callable_allocation_failures(module,root,2,"String binding references");

    module=NULL;OK(string_ref_fixture(&module));
    root=(XrLirFunction){0};OK(xr_lir_function_at(module,0,&root));
    XrRbcProgram *program=NULL;
    CHECK(xr_rbc_compile(module,root,&program,NULL)==XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(module);
    size_t baseline=live_allocations;
    /* Native billing allocations are outside the compiler's malloc hooks.
     * One live control owner defines the exact post-execution charge. */
    XrtBillingOwner *control=NULL;
    CHECK(xrt_billing_owner_new(NULL,SIZE_MAX,&control)==XRT_MANAGED_OK);
    XrtBillingStats control_stats=xrt_billing_owner_stats(control);
    CHECK(control_stats.blocks==1);
    size_t failures=0;
    for (size_t allowance=0;;++allowance) {
        XrRbcExecution *execution=NULL;
        CHECK(xr_rbc_program_prepare(program,NULL,0,&execution)==XR_RBC_OK);
        XrtBillingOwner *billing=NULL;
        CHECK(xrt_native_values_prepare(&execution->values,&billing)==XRT_MANAGED_OK &&
            xrt_billing_owner_retain(billing));
        xrt_billing_owner_fail_after(billing,allowance);
        XrRbcOutcome *outcome=xr_rbc_execution_run(execution);
        CHECK(outcome && !execution->stack.values && !execution->state && xrt_value_root_empty(&execution->values));
        XrRbcStatus status=xr_rbc_outcome_status(outcome);
        uint64_t bits=0;
        if (status==XR_RBC_OK) CHECK(xr_rbc_outcome_bits(outcome,&bits) && bits==2);
        else {
            XrXirValueStatus failure;uint32_t source;const XrtLocationOwner *locations;
            CHECK(status==XR_RBC_VALUE_FAILURE &&
                xr_rbc_outcome_value_failure(outcome,&failure,&source,&locations) && failure==XR_XIR_VALUE_OOM);
            ++failures;
        }
        xrt_billing_owner_fail_after(billing,SIZE_MAX);
        xr_rbc_execution_free(&execution);xr_rbc_outcome_free(&outcome);
        XrtBillingStats retired=xrt_billing_owner_stats(billing);
        CHECK(retired.blocks==control_stats.blocks && retired.bytes==control_stats.bytes);
        xrt_billing_owner_drop(billing);
        CHECK(live_allocations==baseline);
        if (status==XR_RBC_OK) break;
        CHECK(allowance<SIZE_MAX);
    }
    CHECK(failures);
    xrt_billing_owner_drop(control);
    xr_rbc_program_destroy(program);CHECK(!live_allocations);
    printf("String binding native allocation failures %zu drained to the billing control baseline\n",failures);
}
