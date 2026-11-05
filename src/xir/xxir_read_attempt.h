/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_read_attempt.h - Lexical owners of complete metadata read attempts
 *
 * KEY CONCEPT:
 *   Preparation is a private outcome, not a compiler budget failure. Only
 *   capacity survives an attempt; every parent discards its derived prefix.
 */
#ifndef XXIR_READ_ATTEMPT_H
#define XXIR_READ_ATTEMPT_H
#include "xxir_read_scratch.h"

typedef enum XirReadStepKind {
    XIR_READ_COMPLETE,
    XIR_READ_NEED_PREPARE,
    XIR_READ_FAILED
} XirReadStepKind;
typedef struct XirReadStep {
    XirReadStepKind kind;
    XrXirStatus status;
} XirReadStep;
typedef struct XirReadOperation {
    XirReadScratch capacity;
} XirReadOperation;
typedef struct XirReadAttempt {
    bool active;
} XirReadAttempt;
/* This request is borrowed only by synchronous callees. Neither capacity nor
 * a published owner retains it or its control after the lexical call returns. */
typedef struct XirReadRequest {
    XirReadOperation *operation;
    XrCompileRequestControl *control;
    XirReadAttempt *attempt;
} XirReadRequest;

static inline bool xir_read_request_matches(const XrXirCompileContext *work,
    const XirReadRequest *request) {
    return xir_compile_context_valid(work) && (!request || (request->operation &&
        request->operation->capacity.resources==work->resources &&
        (!request->attempt || request->attempt->active)));
}

static inline XirReadStep xir_read_step(const XirReadOperation *operation,XrXirStatus status) {
    if(operation && operation->capacity.preparation_failed)
        return (XirReadStep){XIR_READ_FAILED,status==XR_XIR_OK?XR_XIR_BAD_STRUCTURE:status};
    if(operation && operation->capacity.required) {
        if(status!=XR_XIR_BUDGET) return (XirReadStep){XIR_READ_FAILED,status==XR_XIR_OK?XR_XIR_BAD_STRUCTURE:status};
        return (XirReadStep){XIR_READ_NEED_PREPARE,XR_XIR_OK};
    }
    return (XirReadStep){status==XR_XIR_OK?XIR_READ_COMPLETE:XIR_READ_FAILED,status};
}
static inline XrXirStatus xir_read_begin(const XrXirCompileContext *work,XirReadOperation *operation) {
    if(!operation) return XR_XIR_BAD_STRUCTURE;
    return xir_read_scratch_begin(work,&operation->capacity);
}
/* Every borrowed Input, expected/current/role view and derived prefix must
 * already have ended. A successful prepare grants capacity, not admission. */
static inline XrXirStatus xir_read_prepare(const XrXirCompileContext *work,
    XirReadOperation *operation,XrCompileRequestControl *control) {
    if(!operation) return XR_XIR_BAD_STRUCTURE;
    return xir_read_scratch_prepare(work,&operation->capacity,control);
}
static inline void xir_read_operation_free(XirReadOperation *operation) {
    if(operation) xir_read_scratch_free(&operation->capacity);
}
#endif // XXIR_READ_ATTEMPT_H
