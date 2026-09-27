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
static XrXirValue nominal_transport_cases(const XrXirCallEntry *entries, const XrXirTypes *types,
                                         unsigned mode, bool branch) {
    XrXirDomain *domain = NULL; XrXirTypeArena *arena = NULL;
    CHECK(xr_xir_domain_new(65536,&domain) == XR_XIR_VALUE_OK);
    XrXirBudget budget = {0}; budget.parameters = 1000; budget.metadata_bytes = 65536; budget.work = 100000;
    CHECK(xr_xir_type_arena_new(domain,types,&budget,&arena) == XR_XIR_VALUE_OK);
    uint64_t baseline = xr_xir_domain_stats(domain).live_bytes;
    XrXirValueAdmission admission = {arena,domain,NULL,NULL,100000,65536};
    XrXirValue fields[2] = {{XR_XIR_I64,0,23},{0}}, arguments[2] = {{0},{XR_XIR_BOOL,0,branch}};
    CHECK(xr_xir_string_new(domain,"transport",9,&fields[1]) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_struct_new((XrXirType)256,fields,2,&admission,&arguments[0]) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&fields[1]);
    XrXirCallAccounting accounting = {0};
    XrXirCallConfig config = {entries,4,NULL,65536,1000,10,&accounting,{0},admission};
    XrXirCall *call = NULL;
    CHECK(xr_xir_call_new(&config,2,arguments,2,&call) == XR_XIR_CALL_READY);
    xr_xir_value_drop(&arguments[0]);
    XrXirCallResult wait = xr_xir_call_poll(call);
    CHECK(wait.status == XR_XIR_CALL_SUSPENDED);
    XrXirValue escaped = {0};
    if (mode == 1) CHECK(xr_xir_call_cancel(call) == XR_XIR_CALL_CANCELLED);
    else if (!mode) {
        CHECK(xr_xir_call_resume(call,wait.wake) == XR_XIR_CALL_READY);
        CHECK(xr_xir_call_poll(call).status == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_call_take_result(call,&escaped) == XR_XIR_CALL_RETURNED);
        CHECK(escaped.type == (XrXirType)256);
        XrXirValue field = {0};
        CHECK(xr_xir_struct_get(&escaped,0,&admission,&field) == XR_XIR_VALUE_OK);
        CHECK(field.type == XR_XIR_I64 && field.payload == 23); xr_xir_value_drop(&field);
        CHECK(xr_xir_struct_get(&escaped,1,&admission,&field) == XR_XIR_VALUE_OK);
        const char *bytes; size_t count;
        CHECK(xr_xir_string_view(&field,&bytes,&count) && count == 9 && !memcmp(bytes,"transport",9));
        xr_xir_value_drop(&field);
    }
    CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
    CHECK(accounting.live_bytes == 0 && accounting.allocations == accounting.frees && !accounting.depth);
    if (mode) CHECK(xr_xir_domain_stats(domain).live_bytes == baseline);
    xr_xir_type_arena_drop(arena); xr_xir_domain_drop(domain);
    return escaped;
}
#endif // XIR_NOMINAL_TRANSPORT_CASES_H
