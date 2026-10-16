/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_sdk_return_move_consumer.c - Owned return survives producer and Call release
 *
 * KEY CONCEPT:
 *   The SDK consumer first reads its owner after every producer has retired.
 */
#include "xir/xxir_call_move_internal.h"
#include <stdio.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);return 1;}} while (0)
typedef struct ReturnState {XrXirValue value;} ReturnState;
typedef struct ReturnOwner {XrXirDomain *domain;unsigned releases;bool passed;} ReturnOwner;
static XrXirAction return_resume(XrXirCallView *view) {
    ReturnOwner *owner=(ReturnOwner *)view->environment;ReturnState *state=view->state;
    XrXirAction action={0};
    if (xr_xir_string_new(owner->domain,"m\0v",3,&state->value)!=XR_XIR_VALUE_OK ||
        xr_xir_call_return_move(view,&state->value,&action)!=XR_XIR_CALL_READY)
        return xr_xir_call_fault(XR_XIR_RUN_OUT_OF_MEMORY);
    return action;
}
static void return_release(XrXirCallView *view,XrXirCallStatus status) {
    ReturnOwner *owner=(ReturnOwner *)view->environment;ReturnState *state=view->state;
    ++owner->releases;owner->passed=status==XR_XIR_CALL_RETURNED && !state->value.type && !state->value.payload;
    xr_xir_value_drop(&state->value);
}
int main(void) {
    ReturnOwner owner={0};CHECK(xr_xir_domain_new(65536,&owner.domain)==XR_XIR_VALUE_OK);
    const XrXirCallEntry entry={.abi_version=XR_XIR_CALL_ABI_VERSION,.result=XR_XIR_STRING,
        .state_bytes=sizeof(ReturnState),.resume=return_resume,.release=return_release,.environment=&owner};
    XrXirCallAccounting accounting={0};XrXirCallConfig config={0};
    CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.entries=&entry;config.entry_count=1;config.accounting=&accounting;
    config.admission.domain=owner.domain;config.admission.work=4096;
    XrXirCall *call=NULL;CHECK(xr_xir_call_new(&config,0,NULL,0,&call)==XR_XIR_CALL_READY);
    XrXirCallResult result={0};do {result=xr_xir_call_poll_bounded(call,1);} while (result.status==XR_XIR_CALL_READY);
    XrXirValue output={0};CHECK(result.status==XR_XIR_CALL_RETURNED && xr_xir_call_take_result(call,&output)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY && owner.releases==1 && owner.passed &&
        !accounting.live_bytes && accounting.allocations==accounting.frees);
    xr_xir_domain_drop(owner.domain);const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(&output,&bytes,&length) && length==3 && !memcmp(bytes,"m\0v",3));
    xr_xir_value_drop(&output);puts("return-move-sdk-ok");return 0;
}
