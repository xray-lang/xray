/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_panic.h - Immutable PanicInfo values of the panic channel
 *
 * KEY CONCEPT:
 *   Fault facts remain allocation-free. Assertion messages own an independent
 *   STRING; intrinsic faults derive canonical messages from their facts.
 */
#ifndef XXIR_PANIC_H
#define XXIR_PANIC_H
#include "xxir_value.h"
#include "xxir_fault.h"
#include "../shared/xr_error_core.h"

typedef struct XrXirPanicPayload {
    XrXirFaultDetail detail;
    XrXirValue message;
} XrXirPanicPayload;

XR_FUNC bool xr_xir_panic_empty(const XrXirPanicPayload *panic);
XR_FUNC bool xr_xir_panic_valid(const XrXirPanicPayload *panic);
/* Copy publishes only to an empty output; move consumes an owned source. */
XR_FUNC XrXirValueStatus xr_xir_panic_copy(const XrXirPanicPayload *source, XrXirPanicPayload *output);
XR_FUNC void xr_xir_panic_move(XrXirPanicPayload *source, XrXirPanicPayload *output);
XR_FUNC void xr_xir_panic_drop(XrXirPanicPayload *panic);

/* Enough for the bounds text with two full-width i64 operands. */
#define XR_XIR_PANIC_MESSAGE_CAPACITY 96u
/* Returns the message length, or zero for an invalid detail or buffer. */
XR_FUNC size_t xr_xir_panic_message_format(XrXirFaultDetail detail, char *buffer, size_t capacity);
XR_FUNC XrXirValueStatus xr_xir_panic_info_new(XrXirDomain *domain, const XrXirPanicPayload *panic,
                                             XrXirValue *output);
XR_FUNC bool xr_xir_panic_info_detail(const XrXirValue *info, XrXirFaultDetail *detail);
/* Returns an owned STRING independent of the PanicInfo and execution owner. */
XR_FUNC XrXirValueStatus xr_xir_panic_info_message(const XrXirValue *info, XrXirValue *output);
/* The view borrows the payload or supplied scratch until the caller returns. */
XR_FUNC bool xr_xir_panic_message_borrow(const XrXirPanicPayload *panic, char *scratch,
    size_t capacity, XrErrorCoreMessageView *output);
#endif // XXIR_PANIC_H
