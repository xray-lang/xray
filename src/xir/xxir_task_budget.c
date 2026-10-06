/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_task_budget.c - Failure-before-allocation executor storage charging
 *
 * KEY CONCEPT:
 *   Storage requests consume one cumulative owner before touching an allocator.
 */
#include "xxir_call_internal.h"
#include "xxir_value_internal.h"
#include "../base/xmalloc.h"

XR_FUNC void *xr_xir_call_budget_allocate(XrXirCallBudget *budget, uint64_t bytes, XrXirCallStatus *status) {
    if (!budget || !status || !budget->work_domain || !bytes || bytes > SIZE_MAX ||
        budget->live_bytes > budget->byte_limit || bytes > budget->byte_limit - budget->live_bytes ||
        budget->requested_bytes > budget->requested_limit || bytes > budget->requested_limit - budget->requested_bytes ||
        budget->allocations == UINT64_MAX) {
        if (status) *status = XR_XIR_CALL_LIMIT;
        if (budget) budget->exhausted = true;
        return NULL;
    }
    XrXirValueStatus value_status = XR_XIR_VALUE_OK;
    void *memory = xr_xir_domain_call_allocate(budget->work_domain, bytes, &value_status);
    if (memory || value_status == XR_XIR_VALUE_OOM) budget->requested_bytes += bytes;
    if (!memory) {
        *status = value_status == XR_XIR_VALUE_OOM ? XR_XIR_CALL_OOM : XR_XIR_CALL_LIMIT;
        if (value_status != XR_XIR_VALUE_OOM) budget->exhausted = true;
        return NULL;
    }
    budget->live_bytes += bytes;
    if (budget->live_bytes > budget->peak_bytes) budget->peak_bytes = budget->live_bytes;
    ++budget->allocations;
    return memory;
}
XR_FUNC void xr_xir_call_budget_deallocate(XrXirCallBudget *budget, void *memory, uint64_t bytes) {
    XR_CHECK(budget && memory && bytes && budget->live_bytes >= bytes &&
        budget->allocations > budget->frees, "executor storage requires its original allocation owner");
    budget->live_bytes -= bytes;
    ++budget->frees;
    xr_xir_domain_call_deallocate(budget->work_domain, memory, bytes);
}
