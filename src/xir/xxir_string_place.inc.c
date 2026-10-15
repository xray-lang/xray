/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_string_place.inc.c - Atomic append to an authenticated owned String root
 *
 * KEY CONCEPT:
 *   Borrowing the root avoids an artificial retain, while real aliases keep COW.
 */
static XrXirValueStatus string_place_append_bytes(const XrXirValuePlace *place,
    const char *bytes, size_t length, size_t runes, XrXirValueAdmission *admission) {
    if (!place || place->type != XR_XIR_STRING || !place->payload || (!bytes && length) ||
        !admission || !xr_xir_domain_current(admission->domain))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValue owned = {XR_XIR_STRING, 0, 0};
    memcpy(&owned.payload, place->payload, sizeof(owned.payload));
    if (!xr_xir_value_argument(&owned, NULL, XR_XIR_STRING)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XirString *first = string_pointer(&owned);
    if (length >= SIZE_MAX - first->length) return XR_XIR_VALUE_LIMIT;
    bool receiving = first->object.domain == admission->domain;
    bool unique = receiving && atomic_load_explicit(&first->object.references, memory_order_acquire) == 1;
    uint64_t work = (uint64_t)length + 1;
    if (length && !unique) {
        if (first->length > UINT64_MAX - work) return XR_XIR_VALUE_LIMIT;
        work += first->length;
    } else if (length && first->length + length + 1 > first->capacity) {
        if (first->capacity > UINT64_MAX - work) return XR_XIR_VALUE_LIMIT;
        work += first->capacity;
    }
    if (work > admission->work) return XR_XIR_VALUE_LIMIT;
    admission->work -= work;
    if (!xr_xir_domain_work(admission->domain, work)) return XR_XIR_VALUE_LIMIT;
    if (!length) return XR_XIR_VALUE_OK;
    size_t total = first->length + length, total_runes = first->runes + runes;
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    if (unique) {
        /* Self append may move the backing allocation. Preserve a byte offset,
         * never an interior pointer, across a successful resize. */
        uintptr_t address = (uintptr_t)bytes, base = (uintptr_t)first->bytes;
        bool internal = address >= base && address - base <= first->length &&
            length <= first->length - (address - base);
        size_t offset = internal ? (size_t)(address - base) : 0;
        if (total + 1 > first->capacity) status = string_resize(first, string_capacity(total + 1));
        if (status != XR_XIR_VALUE_OK) return status;
        memmove(first->bytes + first->length, internal ? first->bytes + offset : bytes, length);
        first->length = total; first->runes = total_runes; first->bytes[total] = 0;
    } else {
        XirString *joined = NULL;
        status = string_allocate(admission->domain, total, &joined);
        if (status == XR_XIR_VALUE_OK) {
            if (first->length) memcpy(joined->bytes, first->bytes, first->length);
            memcpy(joined->bytes + first->length, bytes, length);
            joined->runes = total_runes;
            xr_xir_value_drop_graph_operation(&owned);
            owned = string_value(joined);
        }
    }
    if (status == XR_XIR_VALUE_OK) memcpy(place->payload, &owned.payload, sizeof(owned.payload));
    return status;
}
XR_FUNC XrXirValueStatus xr_xir_string_place_append(const XrXirValuePlace *place,
    const XrXirValue *suffix, XrXirValueAdmission *admission) {
    xr_xir_value_graph_begin();
    XrXirValueStatus status = XR_XIR_VALUE_BAD_ARGUMENT;
    if (xr_xir_value_argument(suffix, NULL, XR_XIR_STRING)) {
        XirString *string = string_pointer(suffix);
        status = string_place_append_bytes(place, string->bytes, string->length, string->runes, admission);
    }
    xr_xir_value_graph_end();
    return status;
}
XR_FUNC XrXirValueStatus xr_xir_string_place_append_utf8(const XrXirValuePlace *place,
    const char *bytes, size_t length, XrXirValueAdmission *admission) {
    xr_xir_value_graph_begin();
    XrXirValueStatus status = XR_XIR_VALUE_BAD_ARGUMENT;
    if ((!bytes && length) || length == SIZE_MAX || !admission ||
        !xr_xir_domain_current(admission->domain)) goto done;
    if (length + 1 > admission->work) { status = XR_XIR_VALUE_LIMIT; goto done; }
    admission->work -= length + 1;
    if (!xr_xir_domain_work(admission->domain, length + 1)) { status = XR_XIR_VALUE_LIMIT; goto done; }
    XrUtf8ScanResult scan = xr_utf8_core_scan_strict((const uint8_t *)bytes, length);
    status = scan.error == XR_UTF8_OK ?
        string_place_append_bytes(place, bytes, length, scan.rune_count, admission) : XR_XIR_VALUE_BAD_UTF8;
done:
    xr_xir_value_graph_end();
    return status;
}
