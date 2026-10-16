/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_math.inc.c - System entropy billed to the current activation
 *
 * KEY CONCEPT:
 *   Rejection sampling consumes ordinary admission work on every draw. Neither
 *   a copied callback view nor compiler evaluation can acquire host entropy.
 */
#include "xxir_math_runtime.h"
#include "../os/os_random.h"
static XrXirRunStatus call_math_entropy(void *context,unsigned char *bytes,size_t length) {
    XrXirCallView *view=context;
    if (length!=sizeof(uint64_t) || !bytes || xr_xir_call_execution_status(view)!=XR_XIR_CALL_READY)
        return XR_XIR_RUN_BAD_ARGUMENT;
    if (!call_cell_work(view->activation,1)) return XR_XIR_RUN_STEP_LIMIT;
    xr_random_bytes(bytes,length);return XR_XIR_RUN_OK;
}
XR_FUNC XrXirRunStatus xr_xir_call_math_random(XrXirCallView *view,uint32_t operation,
    int64_t minimum,int64_t maximum,int64_t *output) {
    if (xr_xir_call_execution_status(view)!=XR_XIR_CALL_READY) return XR_XIR_RUN_BAD_ARGUMENT;
    return xr_xir_math_random(operation,(XrXirMathEntropy){view,call_math_entropy},minimum,maximum,output);
}
