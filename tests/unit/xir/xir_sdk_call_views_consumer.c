/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_sdk_call_views_consumer.c - Actual Call root loans and late view owners
 *
 * KEY CONCEPT:
 *   The SDK driver grants scope only to registered physical views in its real
 *   callback; frame retirement revokes them before root storage can be rebound.
 */
#include "xir/xxir_call_view_internal.h"
#include "xir/xxir_call_internal.h"
#include "xir/xxir_array.h"
#include "xir/xxir_value_internal.h"
#include "xir/xxir_compile_domain.h"
#include "xir/xxir_type_arena.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"%d: %s\n",__LINE__,#x);exit(1);}} while(0)
typedef struct SdkCallViewsState {XrXirValue parent,shared;uint32_t pc;} SdkCallViewsState;
typedef struct SdkCallViewsOwner {XrXirValue escaped;XrXirViewAccess access;uint32_t releases;} SdkCallViewsOwner;
static XrXirAction sdk_call_views_resume(XrXirCallView *view) {
    SdkCallViewsOwner *owner=(SdkCallViewsOwner *)view->environment;SdkCallViewsState *state=view->state;
    XrXirFaultDetail fault={0};XrXirValue read={0},number={XR_XIR_I64,0,42};
    if (!state->pc++) {
        XrXirCallView copied=*view;XrXirViewAccess rejected={0};
        CHECK(xr_xir_call_view_access(&copied,&rejected)==XR_XIR_VALUE_BAD_ARGUMENT);
        CHECK(xr_xir_call_view_access(view,&owner->access)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_call_view_new(view,(XrXirType)258,0,0,2,&state->parent)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_view_set(&state->parent,0,&number,&owner->access,&fault)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_call_view_reborrow(view,(XrXirType)259,&state->parent,0,2,&state->shared)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_value_copy(&state->shared,&owner->escaped)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_call_view_drop(view,&state->parent)==XR_XIR_VALUE_OK);
        return (XrXirAction){.kind=XR_XIR_ACTION_SUSPEND};
    }
    CHECK(xr_xir_view_get(&owner->escaped,0,&owner->access,&read,&fault)==XR_XIR_VALUE_OK && read.payload==42);
    xr_xir_value_drop(&read);return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value=number};
}
static void sdk_call_views_release(XrXirCallView *view,XrXirCallStatus reason) {
    SdkCallViewsOwner *owner=(SdkCallViewsOwner *)view->environment;SdkCallViewsState *state=view->state;
    CHECK(reason==XR_XIR_CALL_RETURNED);++owner->releases;
    xr_xir_value_drop(&state->shared);xr_xir_value_drop(&state->parent);
}
static XrXirCellParameterRole sdk_call_views_role(void *context,uint32_t entry,uint32_t parameter) {
    (void)context;return !entry && !parameter ? XR_XIR_CELL_ROLE_SCOPED_REF : XR_XIR_CELL_ROLE_UNKNOWN;
}
int main(void) {
    const XrCompileResourceLimits caps={1048576,1048576,4000000};XrXirCompileContext c={0};
    CHECK(xr_compile_resources_new(&caps,&c.resources)==XR_COMPILE_RESOURCE_OK);c.limits=xr_xir_compile_default_limits();
    const XrXirTypeNode nodes[]={
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},{.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)256},
        {.kind=XR_XIR_TYPE_MUT_SLICE,.element=XR_XIR_I64},{.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_I64}};
    const XrXirTypes types={nodes,4,NULL,NULL};XrXirTypeArena *arena=NULL;XrXirDomain *domain=NULL;
    CHECK(xr_xir_compile_type_arena_new(&c,&types,&arena)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_domain_new(c.resources,65536,&domain)==XR_XIR_VALUE_OK);
    const XrXirDomainBudgetControls limits={65536,65536,1000000,65536,65536};
    CHECK(xr_xir_domain_budget_bind(domain,&limits)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={arena,domain,NULL,NULL,1000000,65536};XrXirValue array={0},cell={0},result={0};
    const XrXirValue fields[]={{XR_XIR_I64,0,11},{XR_XIR_I64,0,22}};
    CHECK(xr_xir_array_new((XrXirType)256,fields,2,&admission,&array)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_new(domain,arena,(XrXirType)257,&array,&admission,&cell)==XR_XIR_VALUE_OK);
    SdkCallViewsOwner owner={0};XrXirCallAccounting accounting={0};XrXirCallConfig config={0};
    const XrXirType parameter=(XrXirType)257;
    XrXirCallEntry entry={.abi_version=XR_XIR_CALL_ABI_VERSION,.parameters=&parameter,.parameter_count=1,
        .result=XR_XIR_I64,.state_bytes=sizeof(SdkCallViewsState),.resume=sdk_call_views_resume,
        .release=sdk_call_views_release,.environment=&owner};
    CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.entries=&entry;config.entry_count=1;config.accounting=&accounting;config.admission=admission;config.byte_limit=65536;
    XrXirCallBudget budget={.byte_limit=65536,.requested_limit=65536,.resume_limit=16,.work_domain=domain};
    XrXirCallRequest request={0,&cell,1,sdk_call_views_role,NULL};XrXirCall *call=NULL;
    entry.result=(XrXirType)258;
    CHECK(xr_xir_call_new_budgeted(&config,&request,&budget,&call)==XR_XIR_CALL_BAD_ARGUMENT && !call);
    entry.result=XR_XIR_I64;request.cell_role=NULL;
    CHECK(xr_xir_call_new_budgeted(&config,&request,&budget,&call)==XR_XIR_CALL_BAD_ARGUMENT && !call);
    request.cell_role=sdk_call_views_role;
    CHECK(xr_xir_call_new_budgeted(&config,&request,&budget,&call)==XR_XIR_CALL_READY);
    XrXirCallResult suspended=xr_xir_call_poll_bounded(call,16);int64_t length=0;
    CHECK(suspended.status==XR_XIR_CALL_SUSPENDED);
    CHECK(xr_xir_view_len(&owner.escaped,&owner.access,&length)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_call_resume(call,suspended.wake)==XR_XIR_CALL_READY);
    CHECK(xr_xir_call_poll_bounded(call,16).status==XR_XIR_CALL_RETURNED && owner.releases==1);
    CHECK(xr_xir_call_take_result(call,&result)==XR_XIR_CALL_RETURNED && result.payload==42);
    xr_xir_value_drop(&result);
    CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY && !accounting.live_bytes && accounting.allocations==accounting.frees);
    CHECK(xr_xir_cell_unborrowed(&cell) && xr_xir_value_valid(&owner.escaped));
    xr_xir_value_drop(&cell);xr_xir_value_drop(&array);xr_xir_domain_drop(domain);
    xr_xir_compile_type_arena_drop(arena);xr_compile_resources_release(c.resources);
    CHECK(xr_xir_value_valid(&owner.escaped));xr_xir_value_drop(&owner.escaped);
    puts("call-views-sdk-ok");return 0;
}
