/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_assert_panics_inbox_gates.inc.c - Active view and physical result ownership
 */
typedef struct PanicsInboxWitness {
    XrXirDomain *domain;
    XrXirValue result;
    XrXirCallView saved;
    XrXirDomainStats baseline;
    uint32_t released, consumed;
} PanicsInboxWitness;
static XrXirAction panics_inbox_child(XrXirCallView *view) {
    PanicsInboxWitness *w=view->instance;
    return (XrXirAction){XR_XIR_ACTION_RETURN,0,NULL,0,w->result,{0},0};
}
static void panics_inbox_release(XrXirCallView *view,XrXirCallStatus status) {
    CHECK(status==XR_XIR_CALL_RETURNED);PanicsInboxWitness *w=view->instance;
    xr_xir_value_drop(&w->result);++w->released;
}
static XrXirAction panics_inbox_parent(XrXirCallView *view) {
    PanicsInboxWitness *w=view->instance;uint32_t *step=view->state;
    if (!(*step)++) return (XrXirAction){XR_XIR_ACTION_CALL,1,NULL,0,{0},{0},0};
    CHECK(view->inbox.status==XR_XIR_CALL_RETURNED && w->released==1);
    XrXirCallResult original=view->inbox;XrXirCallView copy=*view;
    uint64_t bytes=xr_xir_domain_stats(w->domain).live_bytes;CHECK(bytes>w->baseline.live_bytes);
    CHECK(xr_xir_call_discard_inbox(NULL,XR_XIR_STRING)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_call_discard_inbox(&copy,XR_XIR_STRING)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_call_discard_inbox(view,XR_XIR_I64)==XR_XIR_CALL_BAD_STATE);
    void *state=view->state;view->state=NULL;
    CHECK(xr_xir_call_discard_inbox(view,XR_XIR_STRING)==XR_XIR_CALL_BAD_STATE);view->state=state;
    view->inbox.value.reserved=1;
    CHECK(xr_xir_call_discard_inbox(view,XR_XIR_STRING)==XR_XIR_CALL_BAD_STATE);view->inbox=original;
    view->inbox.status=XR_XIR_CALL_THROWN;
    CHECK(xr_xir_call_discard_inbox(view,XR_XIR_STRING)==XR_XIR_CALL_BAD_STATE);view->inbox=original;
    view->phase=XR_XIR_CALL_EXIT;
    CHECK(xr_xir_call_discard_inbox(view,XR_XIR_STRING)==XR_XIR_CALL_BAD_STATE);view->phase=XR_XIR_CALL_NORMAL;
    CHECK(xr_xir_domain_stats(w->domain).live_bytes==bytes && view->inbox.value.payload==original.value.payload);
    w->saved=*view;runtime_attempts=0;runtime_fail_at=0;
    CHECK(xr_xir_call_discard_inbox(view,XR_XIR_STRING)==XR_XIR_CALL_READY);
    CHECK(!runtime_attempts && xr_xir_call_result_empty(&view->inbox));runtime_fail_at=SIZE_MAX;
    XrXirDomainStats stats=xr_xir_domain_stats(w->domain);
    CHECK(stats.live_bytes==w->baseline.live_bytes && stats.allocations==w->baseline.allocations+2 &&
        stats.frees==w->baseline.frees+2);
    CHECK(xr_xir_call_discard_inbox(view,XR_XIR_STRING)==XR_XIR_CALL_BAD_STATE);++w->consumed;
    return (XrXirAction){XR_XIR_ACTION_RETURN,0,NULL,0,{0},{0},0};
}
static void panics_inbox_gates(void) {
    PanicsInboxWitness witness={0};
    CHECK(xr_xir_domain_new(1048576,&witness.domain)==XR_XIR_VALUE_OK);
    witness.baseline=xr_xir_domain_stats(witness.domain);
    CHECK(xr_xir_string_new(witness.domain,"owned callback result",21,&witness.result)==XR_XIR_VALUE_OK);
    const XrXirCallEntry entries[]={
        {XR_XIR_CALL_ABI_VERSION,NULL,0,XR_XIR_UNIT,sizeof(uint32_t),panics_inbox_parent,NULL,NULL,0,0},
        {XR_XIR_CALL_ABI_VERSION,NULL,0,XR_XIR_STRING,0,panics_inbox_child,panics_inbox_release,NULL,0,0}
    };
    XrXirCallAccounting accounting={0};
    XrXirCallConfig config={entries,2,&witness,1048576,100,16,&accounting,{0},{0}};XrXirCall *call=NULL;
    CHECK(xr_xir_call_new(&config,0,NULL,0,&call)==XR_XIR_CALL_READY);
    CHECK(xr_xir_call_poll(call).status==XR_XIR_CALL_RETURNED && witness.released==1 && witness.consumed==1);
    CHECK(xr_xir_call_discard_inbox(&witness.saved,XR_XIR_STRING)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY);xr_xir_domain_drop(witness.domain);
    CHECK(!runtime_live && !runtime_bytes && !accounting.live_bytes && accounting.allocations==accounting.frees);
    puts("Exact active inbox: rejected views unchanged; zero allocations; owner physically refunded before continuation");
}
