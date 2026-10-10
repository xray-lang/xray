/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_nominal_transport_cases.h - Independent nominal call and release expectations
 */
#ifndef XIR_NOMINAL_TRANSPORT_CASES_H
#define XIR_NOMINAL_TRANSPORT_CASES_H
#include "xir/xxir_struct.h"
#include "xir/xxir_types.h"
#include "xir/xxir_type_arena.h"
static void nominal_transport_escaped(XrXirValue *escaped) {
    if (escaped->type == XR_XIR_UNIT) return;
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(65536,&domain) == XR_XIR_VALUE_OK);
    XrXirValueAdmission admission = {xr_xir_value_arena(escaped),domain,NULL,NULL,10000,65536};
    XrXirValue field = {0};
    CHECK(xr_xir_struct_get(escaped,1,&admission,&field) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(escaped);
    const char *bytes; size_t count;
    CHECK(xr_xir_string_view(&field,&bytes,&count) && count == 9 && !memcmp(bytes,"transport",9));
    xr_xir_value_drop(&field); xr_xir_domain_drop(domain);
}
static XrXirInstance *nominal_transport_instance(XrXirProgram *program) {
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config)) == XR_XIR_CALL_READY);
    config.metadata_limit = config.value_limit = config.call_limit = 65536;
    config.requested_value_limit = config.requested_call_limit = 65536;
    config.work_limit = 100000; config.poll_limit = 1000; config.depth_limit = 10;
    XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_new(program,&config,&instance) == XR_XIR_CALL_READY && instance);
    return instance;
}
static XrXirValue nominal_transport_cases(XrXirInstance *instance, const XrXirTypes *types,
                                         unsigned mode, bool branch) {
    XrXirDomain *domain = NULL; XrXirTypeArena *arena = NULL;
    CHECK(xr_xir_domain_new(65536,&domain) == XR_XIR_VALUE_OK);
    XrXirCompileContext budget=consumer_context_limits((XrCompileResourceLimits){65536,1048576,100001});budget.limits.parameters=1000;
    CHECK(xr_xir_compile_type_arena_new(&budget, types, &arena) == XR_XIR_VALUE_OK);
    uint64_t baseline = xr_xir_domain_stats(domain).live_bytes;
    XrXirValueAdmission admission = {arena,domain,NULL,NULL,100000,65536};
    XrXirValue fields[2] = {{XR_XIR_I64,0,23},{0}}, arguments[2] = {{0},{XR_XIR_BOOL,0,branch}};
    CHECK(xr_xir_string_new(domain,"transport",9,&fields[1]) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_struct_new((XrXirType)256,fields,2,&admission,&arguments[0]) == XR_XIR_VALUE_OK);
    /* The independently owned arena remains a real bounded negative input.
     * Matching nominal metadata cannot replace the Instance's owning arena. */
    uint64_t rejected_epoch = instance->epoch;
    CHECK(xr_xir_instance_start(instance,2,arguments,2) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(instance->epoch == rejected_epoch && !instance->call &&
        xr_xir_task_executor_root_idle(instance->executor));
    xr_xir_value_drop(&arguments[0]);
    admission.arena = instance->program->arena;
    CHECK(admission.arena && types == instance->program->types &&
        xr_xir_compile_type_arena_types(admission.arena) == types);
    CHECK(xr_xir_struct_new((XrXirType)256,fields,2,&admission,&arguments[0]) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&fields[1]);
    CHECK(xr_xir_instance_start(instance,2,arguments,2) == XR_XIR_CALL_READY);
    xr_xir_value_drop(&arguments[0]);
    XrXirInstanceResult wait = xr_xir_instance_poll_bounded(instance, UINT64_MAX);
    CHECK(wait.outcome.status == XR_XIR_CALL_SUSPENDED);
    XrXirValue escaped = {0};
    if (mode == 1) {
        CHECK(xr_xir_instance_cancel_current(instance) == XR_XIR_CALL_CANCEL_REQUESTED);
        CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status == XR_XIR_CALL_CANCELLED);
    }
    else if (!mode) {
        CHECK(xr_xir_instance_resume(instance,wait.epoch,wait.outcome.wake) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instance,&escaped) == XR_XIR_CALL_RETURNED);
        CHECK(escaped.type == (XrXirType)256);
        XrXirValue field = {0};
        CHECK(xr_xir_struct_get(&escaped,0,&admission,&field) == XR_XIR_VALUE_OK);
        CHECK(field.type == XR_XIR_I64 && field.payload == 23); xr_xir_value_drop(&field);
        CHECK(xr_xir_struct_get(&escaped,1,&admission,&field) == XR_XIR_VALUE_OK);
        const char *bytes; size_t count;
        CHECK(xr_xir_string_view(&field,&bytes,&count) && count == 9 && !memcmp(bytes,"transport",9));
        xr_xir_value_drop(&field);
    }
    CHECK(xr_xir_instance_stop(instance) == XR_XIR_CALL_READY);
    CHECK(xr_xir_task_executor_root_idle(instance->executor));
    CHECK(instance->budget.peak_bytes <= 65536 && instance->budget.requested_bytes <= 65536 &&
        instance->budget.resumes <= 1000 && !instance->budget.exhausted);
    XrXirCallAccounting accounting[2]; memcpy(accounting,instance->accounting,sizeof(accounting));
    XrXirDomain *owned_domain = instance->domain; CHECK(xr_xir_domain_retain(owned_domain));
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    for (unsigned i = 0; i < 2; ++i)
        CHECK(!accounting[i].depth && accounting[i].peak_bytes <= 65536 && accounting[i].peak_depth <= 10);
    XrXirDomainBudgetStats released = xr_xir_domain_budget_stats(owned_domain);
    CHECK(!released.call_live && released.call_allocations == released.call_frees &&
        !released.metadata_live && released.metadata_allocations == released.metadata_frees);
    CHECK(released.call_peak <= 65536 && released.work <= 100000 &&
        released.requested_call_bytes <= 65536 && released.requested_bytes <= 65536);
    xr_xir_domain_drop(owned_domain);
    if (mode) CHECK(xr_xir_domain_stats(domain).live_bytes == baseline);
    xr_xir_compile_type_arena_drop(arena); xr_xir_domain_drop(domain);
    return escaped;
}
#endif // XIR_NOMINAL_TRANSPORT_CASES_H
