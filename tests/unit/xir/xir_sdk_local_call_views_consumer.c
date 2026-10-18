/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_sdk_local_call_views_consumer.c - Local Cell scopes in an ordinary SDK Call
 */
#include "xir/xxir_call_view_internal.h"
#include "xir/xxir_call_internal.h"
#include "xir/xxir_compile_domain.h"
#include "xir/xxir_array.h"
#include "xir/xxir_value_internal.h"
#include "xir/xxir_type_arena.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"%d: %s\n",__LINE__,#x);exit(1);}} while(0)

typedef struct LocalSdkState {XrXirValue cell,view;uint32_t pc;} LocalSdkState;
typedef struct LocalSdkOwner {XrXirValue escaped;XrXirViewAccess access;uint32_t releases;} LocalSdkOwner;
static XrXirAction local_sdk_resume(XrXirCallView *view) {
    LocalSdkState *state=view->state;LocalSdkOwner *owner=(LocalSdkOwner *)view->environment;
    XrXirValueAdmission *a=xr_xir_call_admission(view);XrXirValue number={XR_XIR_I64,0,55};XrXirFaultDetail fault={0};
    if (!state->pc++) {
        XrXirValue array={0};const XrXirValue fields[]={{XR_XIR_I64,0,11},{XR_XIR_I64,0,22}};
        CHECK(xr_xir_array_new((XrXirType)256,fields,2,a,&array)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_cell_new(a->domain,(XrXirTypeArena *)a->arena,(XrXirType)257,&array,a,&state->cell)==XR_XIR_VALUE_OK);
        xr_xir_value_drop(&array);
        CHECK(xr_xir_call_view_access(view,&owner->access)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_call_view_local(view,(XrXirType)258,&state->cell,0,2,&state->view)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_view_set(&state->view,0,&number,&owner->access,&fault)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_value_move(&state->view,&owner->escaped)==XR_XIR_VALUE_OK);
        return (XrXirAction){.kind=XR_XIR_ACTION_SUSPEND};
    }
    XrXirValue result={0};
    CHECK(xr_xir_view_get(&owner->escaped,0,&owner->access,&result,&fault)==XR_XIR_VALUE_OK && result.payload==55);
    xr_xir_value_drop(&result);return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value=number};
}
static void local_sdk_release(XrXirCallView *view,XrXirCallStatus reason) {
    LocalSdkState *state=view->state;LocalSdkOwner *owner=(LocalSdkOwner *)view->environment;
    CHECK(reason==XR_XIR_CALL_RETURNED);++owner->releases;
    xr_xir_value_drop(&state->cell);xr_xir_value_drop(&state->view);
}
int main(void) {
    const XrCompileResourceLimits limits={1048576,1048576,4194304};XrXirCompileContext c={0};
    CHECK(xr_compile_resources_new(&limits,&c.resources)==XR_COMPILE_RESOURCE_OK);c.limits=xr_xir_compile_default_limits();
    const XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)256},{.kind=XR_XIR_TYPE_MUT_SLICE,.element=XR_XIR_I64}};
    const XrXirTypes types={nodes,3,NULL,NULL};XrXirTypeArena *arena=NULL;XrXirDomain *domain=NULL;
    CHECK(xr_xir_compile_type_arena_new(&c,&types,&arena)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_domain_new(c.resources,65536,&domain)==XR_XIR_VALUE_OK);
    const XrXirDomainBudgetControls caps={65536,65536,1000000,65536,65536};
    CHECK(xr_xir_domain_budget_bind(domain,&caps)==XR_XIR_VALUE_OK);
    LocalSdkOwner owner={0};XrXirCallAccounting accounting={0};XrXirCallConfig config={0};
    const XrXirCallEntry entry={.abi_version=XR_XIR_CALL_ABI_VERSION,.result=XR_XIR_I64,
        .state_bytes=sizeof(LocalSdkState),.resume=local_sdk_resume,.release=local_sdk_release,.environment=&owner};
    CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.entries=&entry;config.entry_count=1;config.accounting=&accounting;
    config.admission=(XrXirValueAdmission){arena,domain,NULL,NULL,1000000,65536};config.byte_limit=65536;
    const XrXirCallRequest request={0,NULL,0,NULL,NULL};
    XrXirCallBudget budget={.byte_limit=65536,.requested_limit=65536,.resume_limit=16,.work_domain=domain};
    XrXirCall *call=NULL;CHECK(xr_xir_call_new_budgeted(&config,&request,&budget,&call)==XR_XIR_CALL_READY);
    XrXirCallResult suspended=xr_xir_call_poll_bounded(call,16);int64_t length=0;XrXirValue result={0};
    CHECK(suspended.status==XR_XIR_CALL_SUSPENDED);
    CHECK(xr_xir_view_len(&owner.escaped,&owner.access,&length)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_call_resume(call,suspended.wake)==XR_XIR_CALL_READY);
    CHECK(xr_xir_call_poll_bounded(call,16).status==XR_XIR_CALL_RETURNED && owner.releases==1);
    CHECK(xr_xir_call_take_result(call,&result)==XR_XIR_CALL_RETURNED && result.payload==55);xr_xir_value_drop(&result);
    CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY && !accounting.live_bytes && accounting.allocations==accounting.frees);
    CHECK(!budget.live_bytes && budget.allocations==budget.frees);
    xr_xir_domain_drop(domain);xr_xir_compile_type_arena_drop(arena);xr_compile_resources_release(c.resources);
    CHECK(xr_xir_value_valid(&owner.escaped));xr_xir_value_drop(&owner.escaped);
    puts("local-call-views-sdk-ok");return 0;
}
