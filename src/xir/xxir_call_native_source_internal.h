/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_native_source_internal.h - Native candidate exits at their real PC
 */
#ifndef XXIR_CALL_NATIVE_SOURCE_INTERNAL_H
#define XXIR_CALL_NATIVE_SOURCE_INTERNAL_H
#include "xxir_call.h"

#define XIR_CALL_NATIVE_SOURCE_VERSION 2u
/* Emitted with the code, retained by its ordinary code lease. These exact
 * native callbacks and value-region offsets identify the observation protocol;
 * a matching Checked packet alone cannot establish that the hooks exist. */
typedef struct XirCallNativeSourceCode {
    uint32_t version,count;
    const XrXirCallEntry *entries;
    const uint32_t *offsets;
} XirCallNativeSourceCode;

typedef struct XirCallNativeSourcePoint {
    XrXirCallView *view;
    const unsigned char *values;
    uint32_t bytes,entry,instruction;
    uint64_t frame;
} XirCallNativeSourcePoint;
/* Trusted in-process debug receiving only. The original callback lends its
 * bounded value storage synchronously; no pointer may escape this call. Source
 * ownership and exact Lowered layout must be checked before reading a value.
 * The producer has not advanced PC. A stop returns ordinary CONTINUE without
 * running that instruction; the sole debugger driver then stops polling. */
typedef struct XirCallNativeSourcePending {
    XrXirCallView *view;
    uint64_t frame;
    uint32_t entry,instruction,callee;
    bool invoked;
} XirCallNativeSourcePending;
/* A proposal emitted only after ordinary call-target resolution. The receiver
 * owns primitive facts; actual child ENTER must still prove driver admission. */
typedef struct XirCallNativeSourceObserver {
    void *owner;
    XrXirCallStatus (*before)(void *,const XirCallNativeSourcePoint *,bool *);
    XrXirCallStatus (*call)(void *,const XirCallNativeSourcePending *);
} XirCallNativeSourceObserver;
XR_FUNC XrXirCallStatus xir_call_native_source_before(XrXirCallView *,uint32_t,
    const unsigned char *,uint32_t,bool *);
XR_FUNC XrXirCallStatus xir_call_native_source_call(XrXirCallView *,uint32_t,uint32_t,bool);
#endif // XXIR_CALL_NATIVE_SOURCE_INTERNAL_H
