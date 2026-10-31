/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_pointer_instance_probe.c - Observe the actual Instance page owner
 *
 * KEY CONCEPT:
 *   The public consumer keeps an observation lease on the real Domain. Page
 *   allocations use its actual ledger; execution and close are not replaced.
 */
#define xr_xir_instance_new pointer_observed_instance_new
#define xr_xir_domain_new_budgeted pointer_observed_domain_new_budgeted
#define xr_xir_instance_generation_new pointer_observed_generation_new
#include "../../../src/xir/xxir_instance.c"
#undef xr_xir_instance_new
#undef xr_xir_domain_new_budgeted
#undef xr_xir_instance_generation_new
XR_FUNC XrXirValueStatus xr_xir_domain_new_budgeted(uint64_t,
    const XrXirDomainBudgetControls *,XrXirDomain **);
static XrXirDomain *pointer_observed_domain;
static uint64_t pointer_observed_frame;
XR_FUNC XrXirValueStatus pointer_observed_domain_new_budgeted(uint64_t limit,
    const XrXirDomainBudgetControls *controls,XrXirDomain **output) {
    XrXirValueStatus status=xr_xir_domain_new_budgeted(limit,controls,output);
    if (status==XR_XIR_VALUE_OK) pointer_observed_frame=xr_xir_domain_stats(*output).live_bytes;
    return status;
}
XR_FUNC XrXirCallStatus xr_xir_instance_new(XrXirProgram *program,
    const XrXirInstanceConfig *config,XrXirInstance **output) {
    XrXirCallStatus status=pointer_observed_instance_new(program,config,output);
    if (status==XR_XIR_CALL_READY) {
        XR_CHECK(!pointer_observed_domain,"one observed Instance owns this test request");
        pointer_observed_domain=(*output)->domain;
        XR_CHECK(xr_xir_domain_retain(pointer_observed_domain),"the observation lease is real");
    }
    return status;
}
XR_FUNC XrXirCallStatus xr_xir_instance_generation_new(const XrXirInstanceConfig *config,XrXirInstance **output) {
    XrXirCallStatus status=pointer_observed_generation_new(config,output);
    if (status==XR_XIR_CALL_READY) {
        XR_CHECK(!pointer_observed_domain,"one observed generation owner is active");
        pointer_observed_domain=xir_instance_generation_domain(*output);
        XR_CHECK(xr_xir_domain_retain(pointer_observed_domain),"generation observation has its own lease");
    }
    return status;
}
XR_FUNC XrXirDomain *pointer_instance_observed_domain(void) {return pointer_observed_domain;}
XR_FUNC uint64_t pointer_instance_observed_frame(void) {return pointer_observed_frame;}
XR_FUNC void pointer_instance_observer_drop(void) {
    xr_xir_domain_drop(pointer_observed_domain);pointer_observed_domain=NULL;
}
