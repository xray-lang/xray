/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_panic.h - Immutable PanicInfo values of the panic channel
 *
 * KEY CONCEPT:
 *   A PanicInfo owns only the validated fault detail it was created from.
 *   Its message is derived from that detail by one formatter, so every
 *   backend observes the same text.
 */
#ifndef XXIR_PANIC_H
#define XXIR_PANIC_H
#include "xxir_value.h"
#include "xxir_fault.h"

/* Enough for the bounds text with two full-width i64 operands. */
#define XR_XIR_PANIC_MESSAGE_CAPACITY 96u
/* Returns the message length, or zero for an invalid detail or buffer. */
XR_FUNC size_t xr_xir_panic_message_format(XrXirFaultDetail detail, char *buffer, size_t capacity);
XR_FUNC XrXirValueStatus xr_xir_panic_info_new(XrXirDomain *domain, XrXirFaultDetail detail,
                                             XrXirValue *output);
XR_FUNC bool xr_xir_panic_info_detail(const XrXirValue *info, XrXirFaultDetail *detail);
/* The message string is allocated in the PanicInfo's own domain. */
XR_FUNC XrXirValueStatus xr_xir_panic_info_message(const XrXirValue *info, XrXirValue *output);
#endif // XXIR_PANIC_H
