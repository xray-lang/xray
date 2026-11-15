/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_c_transfer_strings.c - Typed factory adapter outside generated C
 */
#include "lir_c_transfer_strings.h"
#include "runtime/native/xnative_string_backing.h"
static XrtBackingTypes strings;
XRAY_API XrtManagedStatus lir_c_transfer_strings_init(XrtBillingOwner *billing) {
    return xrt_backing_types_init(billing, &strings);
}
XRAY_API void lir_c_transfer_strings_drop(void) { xrt_backing_types_drop(&strings); }
XRAY_API XrtManagedStatus lir_c_transfer_string_make(XrtManagedFrame *frame, size_t slot,
    int identifier, XrtManagedHeader **output) {
    char byte = (char)('a' + identifier % 26);
    XrtManagedStatus status = xrt_string_backing_create(frame, slot, &strings, &byte, 1, 0);
    return status == XRT_MANAGED_OK ? xrt_managed_slot_borrow(frame, slot, output) : status;
}
XRAY_API bool lir_c_transfer_string_matches(const XrtManagedHeader *header, int identifier) {
    const XrtStringBacking *string = (const XrtStringBacking *)header;
    return string && string->length == 1 && string->runes == 1 && string->capacity == 2 &&
        string->bytes[0] == (char)('a' + identifier % 26) && !string->bytes[1];
}
