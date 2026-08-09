/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcopy_kind.c - Lightweight value transfer classification.
 */

#include "xdeep_copy.h"
#include "../runtime/object/xstring.h"

XrCopyKind xr_value_copy_kind(XrValue value) {
    if (XR_IS_NUM(value) || XR_IS_BOOL(value) || XR_IS_NULL(value))
        return XR_COPY_IMMEDIATE;
    if (!XR_IS_PTR(value))
        return XR_COPY_IMMEDIATE;

    XrObjHeader *obj = XR_VALUE_GCPTR(value);
    /* Scheduler-owned handles (Task, Coroutine, CoroPool, and future managed
     * control-plane objects) are identity values, never execution-local data
     * graphs. Their signed atomic-band RC and MANAGED lifetime make compiler
     * dup/drop no-ops; classifying them as deep-copy candidates makes a valid
     * Task argument fail closed at every `go` boundary. Keep the rule on the
     * object-model flag so adding another managed handle cannot silently drift
     * from transfer semantics. */
    if (obj && XR_OBJ_GET_FLAG(obj, XR_OBJ_MANAGED))
        return XR_COPY_SHARED_REF;

    uint8_t type = XR_HEAP_TYPE(value);
    switch (type) {
        case XR_TSTRING:
            return XR_OBJ_IS_SHARED(obj) ? XR_COPY_SHARED_REF : XR_COPY_DEEP;
        case XR_TCHANNEL:
        case XR_TATOMIC:
        case XR_TWORKQUEUE:
        case XR_TRESULTGROUP:
        case XR_TCOUNTDOWNLATCH:
        case XR_TSEMAPHORE:
        case XR_TEVENTCOUNT:
        case XR_TTHREAD:
            return XR_COPY_SHARED_REF;
        case XR_TARRAY:
        case XR_TMAP:
        case XR_TFUNCTION:
            return XR_COPY_DEEP;
        default:
            return XR_COPY_DEEP;
    }
}
