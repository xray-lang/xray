/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_string_instance_probe.c - Same-source Instance definitions with retirement observations
 *
 * KEY CONCEPT:
 *   The test links the actual implementation and observes only its final free.
 */
#include "xir_string_instance_probe.h"
#include "xir/xxir_value_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef CHECK
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#endif
static void string_instance_retiring(void *pointer);
#ifndef XIR_STRING_INSTANCE_IMPLEMENTATION_PRESENT
static void string_instance_metadata_free(XrXirDomain *domain, void *pointer, uint64_t bytes);
#define xr_xir_domain_metadata_deallocate(domain,pointer,bytes) string_instance_metadata_free(domain,pointer,bytes)
#include "xir/xxir_instance.c"
#undef xr_xir_domain_metadata_deallocate
static void string_instance_metadata_free(XrXirDomain *domain, void *pointer, uint64_t bytes) {
    string_instance_retiring(pointer);
    xr_xir_domain_metadata_deallocate(domain,pointer,bytes);
}
#endif
static XrXirInstance *string_instance_armed;
static XirStringRetirement string_instance_record;
static uint32_t string_instance_retirements;
static void string_instance_retiring(void *pointer) {
    if (!string_instance_armed || pointer!=string_instance_armed) return;
    CHECK(!string_instance_armed->call && !string_instance_armed->executor);
    memcpy(string_instance_record.accounting,string_instance_armed->accounting,
        sizeof(string_instance_record.accounting));
    ++string_instance_retirements;
}
XR_FUNC XrXirDomain *xr_string_instance_domain(XrXirInstance *instance) {
    CHECK(instance && xr_xir_domain_current(instance->domain));
    string_instance_record=(XirStringRetirement){.domain=instance->domain,
        .baseline=xr_xir_domain_stats(instance->domain)};
    CHECK(string_instance_record.baseline.live_bytes &&
        string_instance_record.baseline.allocations==1 && !string_instance_record.baseline.frees);
    return instance->domain;
}
XR_FUNC XrXirCallStatus xr_string_instance_completion(XrXirInstance *instance) {
    CHECK(instance && instance->executor);
    return xr_xir_task_executor_completion_status(instance->executor);
}
XR_FUNC void xr_string_instance_output_busy(XrXirInstance *instance, bool cancel) {
    CHECK(instance && instance->call && instance->driving);
    XrXirValue value={0};
    CHECK(xr_xir_call_poll_bounded(instance->call,UINT64_MAX).status==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_call_take_result(instance->call,&value)==XR_XIR_CALL_BUSY && !value.type);
    CHECK(xr_xir_call_free(instance->call)==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_BUSY && !value.type);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_BUSY);
    if (cancel) CHECK(xr_xir_call_request_cancel(instance->call)==XR_XIR_CALL_CANCEL_REQUESTED);
}
XR_FUNC XirStringRetirement xr_string_instance_retire(XrXirInstance *instance,
    XrXirCallStatus expected, uint64_t poll_limit) {
    CHECK(instance && !string_instance_armed);
    XrXirDomain *domain=instance->domain;
    CHECK(xr_xir_domain_retain(domain));
    CHECK(string_instance_record.domain==domain && string_instance_record.baseline.live_bytes);
    memset(string_instance_record.accounting,0,sizeof(string_instance_record.accounting));
    string_instance_armed=instance;string_instance_retirements=0;
    XrXirCallStatus status=xr_xir_instance_free(instance);
    string_instance_armed=NULL;
    CHECK(status==expected && string_instance_retirements==1);
    for (uint32_t i=0;i<2;++i) {
        const XrXirCallAccounting *a=&string_instance_record.accounting[i];
        CHECK(!a->live_bytes && !a->depth && a->allocations==a->frees);
        CHECK(a->peak_bytes<=65536 && a->polls<=poll_limit && a->peak_depth<=10);
    }
    XrXirDomainBudgetStats budget=xr_xir_domain_budget_stats(domain);
    CHECK(!budget.metadata_live && !budget.call_live &&
        budget.metadata_allocations==budget.metadata_frees && budget.call_allocations==budget.call_frees);
    return string_instance_record;
}
