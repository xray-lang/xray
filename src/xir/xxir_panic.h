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
#include "../shared/xr_value_format_core.h"

typedef struct XrXirPanicPayload {
    XrXirFaultDetail detail;
    XrXirValue message;
    XrXirValue error;
    XrXirValue replaced_error;
} XrXirPanicPayload;

#define XR_XIR_TRY_ASSERT_MESSAGE "try! failed: "
/* Borrows the actual concrete enum; the panic owner must remain alive. */
XR_FUNC bool xr_xir_panic_error_borrow(const XrXirPanicPayload *panic, XrXirValue *output);
/* One complete graph walk for actual Task outcome reception. Every carried
 * enum must satisfy its complete Sendable definition in the receiving arena. */
XR_FUNC XrXirValueStatus xr_xir_panic_graph_admit(const XrXirValue *value, XrXirType type, XrXirValueAdmission *admission);
XR_FUNC bool xr_xir_panic_error_format(const XrXirPanicPayload *panic, XrValueFormatSink sink);
XR_FUNC bool xr_xir_panic_empty(const XrXirPanicPayload *panic);
XR_FUNC bool xr_xir_panic_valid(const XrXirPanicPayload *panic);
/* Ordinary Call reception checks every carried graph without Task widening. */
XR_FUNC XrXirValueStatus xr_xir_panic_payload_admit(const XrXirPanicPayload *panic, XrXirValueAdmission *admission);
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
/* Copies the authenticated optional Error into the receiving admission domain. */
XR_FUNC XrXirValueStatus xr_xir_panic_info_replaced_error(const XrXirValue *info,
    XrXirType type, XrXirValueAdmission *admission, XrXirValue *output);
/* The view borrows the payload or supplied scratch until the caller returns. */
XR_FUNC bool xr_xir_panic_message_borrow(const XrXirPanicPayload *panic, char *scratch,
    size_t capacity, XrErrorCoreMessageView *output);
#endif // XXIR_PANIC_H
