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
static void string_append_bytes_in_place(XirString *string, const char *bytes,
    size_t length, size_t runes) {
    XrStringCursor cursor = {string->bytes, string->length, string->runes, string->capacity,
        &string->length, &string->runes};
    xr_string_cursor_append(&cursor, bytes, length, runes);
    xr_string_cursor_commit(&cursor);
}
XR_FUNC bool xr_xir_root_string_append_cursor(XrXirDomain *domain, XirString *string,
    XrStringCursor *output) {
    XR_DCHECK(xr_xir_domain_current(domain), "the native Root retains its current domain");
    if (!output || output->bytes || !domain->root || string->object.domain != domain ||
        !xir_object_unique(&string->object)) return false;
    *output = (XrStringCursor){string->bytes, string->length, string->runes, string->capacity,
        &string->length, &string->runes};
    return true;
}
XR_FUNC bool xr_xir_root_string_append_in_place(XrXirDomain *domain, XirString *string,
    const XirString *suffix) {
    XR_DCHECK(xr_xir_domain_current(domain), "the native root retains its current domain");
    if (!domain->root || string->object.domain != domain ||
        suffix->length >= SIZE_MAX - string->length ||
        string->length + suffix->length + 1 > string->capacity ||
        !xir_object_unique(&string->object)) return false;
    /* Root domains cannot close or bind a budget. This leaf changes no graph
     * edges, storage ownership or allocation accounting, and calls no host. */
    string_append_bytes_in_place(string, suffix->bytes, suffix->length, suffix->runes);
    return true;
}
static XrXirValueStatus string_place_append_bytes(const XrXirValuePlace *place,
    const char *bytes, size_t length, size_t runes, XrXirValueAdmission *admission, XrXirValue *retired) {
    if (!place || place->type != XR_XIR_STRING || !place->payload || (!bytes && length) ||
        !admission || !xr_xir_domain_current(admission->domain) || !unit_value(retired) ||
        place->payload == &retired->payload)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValue owned = {XR_XIR_STRING, 0, 0};
    memcpy(&owned.payload, place->payload, sizeof(owned.payload));
    if (!xr_xir_value_argument(&owned, NULL, XR_XIR_STRING)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XirString *first = string_pointer(&owned);
    if (length >= SIZE_MAX - first->length) return XR_XIR_VALUE_LIMIT;
    bool receiving = first->object.domain == admission->domain;
    bool unique = receiving && !value_place_shared(place) && xir_object_unique(&first->object);
    if (!admission_is_compile(admission)) {
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
    }
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
        string_append_bytes_in_place(first, internal ? first->bytes + offset : bytes, length, runes);
    } else {
        XirString *joined = NULL;
        status = string_allocate(admission->domain, total, &joined);
        if (status == XR_XIR_VALUE_OK) {
            if (first->length) memcpy(joined->bytes, first->bytes, first->length);
            memcpy(joined->bytes + first->length, bytes, length);
            joined->runes = total_runes;
            XrXirValue result = string_value(joined);
            status = value_place_publish(place, &result);
            if (status != XR_XIR_VALUE_OK) { xr_xir_value_drop(&result); return status; }
            memcpy(place->payload, &result.payload, sizeof(result.payload));
            *retired = owned;
            owned = result;
        }
    }
    if (status == XR_XIR_VALUE_OK) memcpy(place->payload, &owned.payload, sizeof(owned.payload));
    return status;
}
XR_FUNC XrXirValueStatus xr_xir_string_place_append_deferred(const XrXirValuePlace *place,
    const XrXirValue *suffix, XrXirValueAdmission *admission, XrXirValue *retired) {
    xr_xir_value_graph_begin();
    XrXirValueStatus status = XR_XIR_VALUE_BAD_ARGUMENT;
    if (xr_xir_value_argument(suffix, NULL, XR_XIR_STRING)) {
        XirString *string = string_pointer(suffix);
        status = string_place_append_bytes(place, string->bytes, string->length, string->runes, admission, retired);
    }
    xr_xir_value_graph_end();
    return status;
}
XR_FUNC XrXirValueStatus xr_xir_string_place_append_utf8_deferred(const XrXirValuePlace *place,
    const char *bytes, size_t length, XrXirValueAdmission *admission, XrXirValue *retired) {
    xr_xir_value_graph_begin();
    XrXirValueStatus status = XR_XIR_VALUE_BAD_ARGUMENT;
    if ((!bytes && length) || length == SIZE_MAX || !admission ||
        !xr_xir_domain_current(admission->domain)) goto done;
    if (!admission_is_compile(admission) && (length + 1 > admission->work)) { status = XR_XIR_VALUE_LIMIT; goto done; }
    if (!admission_is_compile(admission)) admission->work -= length + 1;
    if (!xr_xir_domain_is_compile(admission->domain) && (!xr_xir_domain_work(admission->domain, length + 1))) { status = XR_XIR_VALUE_LIMIT; goto done; }
    XrUtf8ScanResult scan = xr_utf8_core_scan_strict((const uint8_t *)bytes, length);
    status = scan.error == XR_UTF8_OK ?
        string_place_append_bytes(place, bytes, length, scan.rune_count, admission, retired) : XR_XIR_VALUE_BAD_UTF8;
done:
    xr_xir_value_graph_end();
    return status;
}

XR_FUNC XrXirValueStatus xr_xir_string_place_append(const XrXirValuePlace *place,
    const XrXirValue *suffix, XrXirValueAdmission *admission) {
    XrXirValue retired = {0};
    XrXirValueStatus status = xr_xir_string_place_append_deferred(place, suffix, admission, &retired);
    xr_xir_value_drop(&retired);
    return status;
}
XR_FUNC XrXirValueStatus xr_xir_string_place_append_utf8(const XrXirValuePlace *place,
    const char *bytes, size_t length, XrXirValueAdmission *admission) {
    XrXirValue retired = {0};
    XrXirValueStatus status = xr_xir_string_place_append_utf8_deferred(place, bytes, length, admission, &retired);
    xr_xir_value_drop(&retired);
    return status;
}
