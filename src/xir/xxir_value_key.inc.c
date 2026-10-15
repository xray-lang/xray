/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_value_key.inc.c - Allocation-free built-in hash key relations
 *
 * KEY CONCEPT:
 *   Normalize IEEE key classes with integer bits, without changing value equality.
 */
#include "../base/xhash.h"
static uint64_t key_scalar_bits(const XrXirValue *value) {
    uint64_t bits = (uint64_t)value->payload;
    if (value->type == XR_XIR_F32) {
        uint32_t word = (uint32_t)bits;
        if (!(word & UINT32_C(0x7fffffff))) return 0;
        if ((word & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000) &&
            (word & UINT32_C(0x007fffff))) return UINT64_C(0x7fc00000);
    } else if (value->type == XR_XIR_F64) {
        if (!(bits & UINT64_C(0x7fffffffffffffff))) return 0;
        if ((bits & UINT64_C(0x7ff0000000000000)) == UINT64_C(0x7ff0000000000000) &&
            (bits & UINT64_C(0x000fffffffffffff))) return UINT64_C(0x7ff8000000000000);
    }
    return bits;
}
static XrXirValueStatus key_charge(XrXirValueAdmission *admission, uint64_t work) {
    if (work > admission->work) return XR_XIR_VALUE_LIMIT;
    admission->work -= work;
    return xr_xir_domain_work(admission->domain, work) ? XR_XIR_VALUE_OK : XR_XIR_VALUE_LIMIT;
}
XR_FUNC XrXirValueStatus xr_xir_key_hash(const XrXirValue *value,
    XrXirValueAdmission *admission, uint64_t *output) {
    if (!value || !output || !admission || !xr_xir_domain_current(admission->domain))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_key_builtin_type((XrXirType)value->type)) return XR_XIR_VALUE_UNSUPPORTED;
    xr_xir_value_graph_begin();
    XrXirValueStatus status = XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_value_valid(value)) goto done;
    XirString *string = value->type == XR_XIR_STRING ? string_pointer(value) : NULL;
    uint64_t length = string ? (uint64_t)string->length : 8;
    if (length == UINT64_MAX) { status = XR_XIR_VALUE_LIMIT; goto done; }
    status = key_charge(admission, length + 1);
    if (status != XR_XIR_VALUE_OK) goto done;
    if (string) *output = xr_hash_bytes64(string->bytes, string->length);
    else {
        uint64_t bits = key_scalar_bits(value);
        uint8_t bytes[8];
        for (unsigned i = 0; i < 8; ++i) bytes[i] = (uint8_t)(bits >> (i * 8));
        *output = xr_hash_bytes64(bytes, sizeof(bytes));
    }
done:
    xr_xir_value_graph_end();
    return status;
}
XR_FUNC XrXirValueStatus xr_xir_key_equal(const XrXirValue *left,
    const XrXirValue *right, XrXirValueAdmission *admission, bool *output) {
    if (!left || !right || !output || !admission || !xr_xir_domain_current(admission->domain) ||
        left->type != right->type) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_key_builtin_type((XrXirType)left->type)) return XR_XIR_VALUE_UNSUPPORTED;
    xr_xir_value_graph_begin();
    XrXirValueStatus status = XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_value_valid(left) || !xr_xir_value_valid(right)) goto done;
    XirString *a = left->type == XR_XIR_STRING ? string_pointer(left) : NULL;
    XirString *b = a ? string_pointer(right) : NULL;
    uint64_t length = a && a->length == b->length ? (uint64_t)a->length : 0;
    if (length == UINT64_MAX) { status = XR_XIR_VALUE_LIMIT; goto done; }
    status = key_charge(admission, length + 1);
    if (status == XR_XIR_VALUE_OK)
        *output = a ? a->length == b->length && (!length || !memcmp(a->bytes, b->bytes, (size_t)length)) :
            key_scalar_bits(left) == key_scalar_bits(right);
done:
    xr_xir_value_graph_end();
    return status;
}
