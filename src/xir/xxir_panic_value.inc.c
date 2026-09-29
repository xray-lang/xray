/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_panic_value.inc.c - PanicInfo ownership and canonical messages
 *
 * KEY CONCEPT:
 *   The object is created once from a validated detail and never mutated,
 *   so copies may share it without observable aliasing.
 */
XR_FUNC size_t xr_xir_panic_message_format(XrXirFaultDetail detail, char *buffer, size_t capacity) {
    if (!buffer || !capacity || !xr_xir_fault_panic_valid(detail)) return 0;
    int written;
    switch (detail.code) {
    case XR_XIR_PANIC_DIVIDE: written = snprintf(buffer, capacity, "division by zero"); break;
    case XR_XIR_PANIC_REMAINDER: written = snprintf(buffer, capacity, "modulo by zero"); break;
    case XR_XIR_PANIC_RANGE: written = snprintf(buffer, capacity, "numeric conversion is out of range"); break;
    case XR_XIR_PANIC_DEFER_ASYNC: written = snprintf(buffer, capacity, "defer cleanup cannot suspend or create tasks"); break;
    case XR_XIR_PANIC_BOUNDS:
        written = snprintf(buffer, capacity, "array index out of range: %" PRId64 " (length %" PRId64 ")",
            detail.index, detail.length);
        break;
    default: written = snprintf(buffer, capacity, "non-exhaustive match"); break;
    }
    return written > 0 && (size_t) written < capacity ? (size_t) written : 0;
}
XR_FUNC XrXirValueStatus xr_xir_panic_info_new(XrXirDomain *domain, XrXirFaultDetail detail,
                                             XrXirValue *output) {
    if (!domain || !unit_value(output) || !xr_xir_fault_panic_valid(detail)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_reference_retain(&domain->references)) return XR_XIR_VALUE_REFCOUNT_LIMIT;
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    XirPanicInfo *info = xr_xir_domain_allocate(domain, sizeof(*info), &status);
    if (!info) { xr_xir_domain_drop(domain); return status; }
    atomic_init(&info->object.references, 1);
    info->object.domain = domain;
    info->object.type = XR_XIR_PANIC_INFO;
    info->object.arena = NULL; info->object.kind = 0;
    info->detail = detail;
    output->type = XR_XIR_PANIC_INFO;
    memcpy(&output->payload, &info, sizeof(info));
    return XR_XIR_VALUE_OK;
}
XR_FUNC bool xr_xir_panic_info_detail(const XrXirValue *info, XrXirFaultDetail *detail) {
    if (!detail || !xr_xir_value_argument(info, NULL, XR_XIR_PANIC_INFO)) return false;
    *detail = ((const XirPanicInfo *) object_pointer(info))->detail;
    return true;
}
XR_FUNC XrXirValueStatus xr_xir_panic_info_message(const XrXirValue *info, XrXirValue *output) {
    XrXirFaultDetail detail;
    if (!unit_value(output) || !xr_xir_panic_info_detail(info, &detail)) return XR_XIR_VALUE_BAD_ARGUMENT;
    char text[XR_XIR_PANIC_MESSAGE_CAPACITY];
    size_t length = xr_xir_panic_message_format(detail, text, sizeof(text));
    if (!length) return XR_XIR_VALUE_BAD_ARGUMENT;
    return xr_xir_string_new(object_pointer(info)->domain, text, length, output);
}
