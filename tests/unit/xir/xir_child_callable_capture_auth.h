/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_child_callable_capture_auth.h - Authentic captured producer refusals
 *
 * KEY CONCEPT:
 *   A copied callable advertisement cannot replace its real Program producer.
 *   Refusal preserves the output and existing staged invocation.
 */
#ifndef XIR_CHILD_CALLABLE_CAPTURE_AUTH_H
#define XIR_CHILD_CALLABLE_CAPTURE_AUTH_H
#include "xir/xxir_instance_function_internal.h"
static void child_capture_auth_probes(XrXirCallView *view,const XrXirAction *action) {
    const XrXirFunctionBinding *binding=xr_xir_function_binding(&action->value);
    CHECK(binding&&binding->capture_count==1&&binding->captures);
    XirFunctionProducer *producer=(XirFunctionProducer *)xr_xir_function_producer(&binding->captures[0]);
    CHECK(producer);
    XirFunctionProducer saved=*producer;
    for(unsigned mode=0;mode<2;++mode) {
        if(mode)producer->owner=(const struct XrXirEffects *)(uintptr_t)1;
        else producer->site=UINT32_MAX;
        uint32_t output=UINT32_C(0xfedcba98);
        size_t attempts=runtime_attempts;
        XrXirCallStatus status=xr_xir_instance_resolve_function_at(view,2,&action->value,NULL,0,&output);
        *producer=saved;
        CHECK(status==XR_XIR_CALL_BAD_ARGUMENT&&output==UINT32_C(0xfedcba98));
        CHECK(runtime_attempts==attempts);
    }
}
#endif
