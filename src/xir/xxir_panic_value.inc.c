/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_panic_value.inc.c - PanicInfo ownership and canonical messages
 *
 * KEY CONCEPT:
 *   The object is created once from a validated payload and never mutated,
 *   so copies may share it without observable aliasing.
 */
static size_t xr_xir_panic_message_format_graph_operation(XrXirFaultDetail detail, char *buffer, size_t capacity) {
    if (!buffer || !capacity || !xr_xir_fault_panic_valid(detail) ||
        detail.code == XR_XIR_PANIC_ASSERTION) return 0;
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
    case XR_XIR_PANIC_MATCH: written = snprintf(buffer, capacity, "non-exhaustive match"); break;
    case XR_XIR_PANIC_NULL_UNWRAP: written = snprintf(buffer, capacity, "cannot unwrap a null value"); break;
    default: return 0;
    }
    return written > 0 && (size_t) written < capacity ? (size_t) written : 0;
}
XR_FUNCDEF size_t xr_xir_panic_message_format(XrXirFaultDetail detail, char *buffer, size_t capacity) {
    xr_xir_value_graph_begin();
    size_t graph_outcome = xr_xir_panic_message_format_graph_operation(detail, buffer, capacity);
    xr_xir_value_graph_end();
    return graph_outcome;
}
static bool xr_xir_panic_empty_graph_operation(const XrXirPanicPayload *panic) {
    return panic && xr_xir_fault_empty(panic->detail) && unit_value(&panic->message);
}
XR_FUNCDEF bool xr_xir_panic_empty(const XrXirPanicPayload *panic) {
    xr_xir_value_graph_begin();
    bool graph_outcome = xr_xir_panic_empty_graph_operation(panic);
    xr_xir_value_graph_end();
    return graph_outcome;
}
static bool xr_xir_panic_valid_graph_operation(const XrXirPanicPayload *panic) {
    if (!panic) return false;
    if (xr_xir_panic_empty(panic)) return true;
    if (!xr_xir_fault_panic_valid(panic->detail)) return false;
    return panic->detail.code == XR_XIR_PANIC_ASSERTION ?
        xr_xir_value_argument(&panic->message, NULL, XR_XIR_STRING) : unit_value(&panic->message);
}
XR_FUNCDEF bool xr_xir_panic_valid(const XrXirPanicPayload *panic) {
    xr_xir_value_graph_begin();
    bool graph_outcome = xr_xir_panic_valid_graph_operation(panic);
    xr_xir_value_graph_end();
    return graph_outcome;
}
static XrXirValueStatus xr_xir_panic_copy_graph_operation(const XrXirPanicPayload *source, XrXirPanicPayload *output) {
    if (!xr_xir_panic_empty(output) || !xr_xir_panic_valid(source)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirPanicPayload copy = {source->detail, {0}};
    XrXirValueStatus status = xr_xir_value_copy(&source->message, &copy.message);
    if (status == XR_XIR_VALUE_OK) *output = copy;
    return status;
}
XR_FUNCDEF XrXirValueStatus xr_xir_panic_copy(const XrXirPanicPayload *source, XrXirPanicPayload *output) {
    xr_xir_value_graph_begin();
    XrXirValueStatus graph_outcome = xr_xir_panic_copy_graph_operation(source, output);
    xr_xir_value_graph_end();
    return graph_outcome;
}
static void xr_xir_panic_move_graph_operation(XrXirPanicPayload *source, XrXirPanicPayload *output) {
    XR_CHECK(source != output && xr_xir_panic_valid(source) && xr_xir_panic_empty(output),
        "panic move requires a valid owner and empty destination");
    *output = *source;
    *source = (XrXirPanicPayload){0};
}
XR_FUNCDEF void xr_xir_panic_move(XrXirPanicPayload *source, XrXirPanicPayload *output) {
    xr_xir_value_graph_begin();
    xr_xir_panic_move_graph_operation(source, output);
    xr_xir_value_graph_end();
}
static void xr_xir_panic_drop_graph_operation(XrXirPanicPayload *panic) {
    if (!panic) return;
    XR_CHECK(xr_xir_panic_valid(panic), "invalid owned panic release");
    xr_xir_value_drop(&panic->message);
    *panic = (XrXirPanicPayload){0};
}
XR_FUNCDEF void xr_xir_panic_drop(XrXirPanicPayload *panic) {
    xr_xir_value_graph_begin();
    xr_xir_panic_drop_graph_operation(panic);
    xr_xir_value_graph_end();
}
static bool xr_xir_panic_message_borrow_graph_operation(const XrXirPanicPayload *panic, char *scratch,
    size_t capacity, XrErrorCoreMessageView *output) {
    if (!output || !xr_xir_panic_valid(panic) || xr_xir_panic_empty(panic)) return false;
    const char *bytes = NULL;
    size_t length = 0;
    if (panic->detail.code == XR_XIR_PANIC_ASSERTION) {
        if (!xr_xir_string_view(&panic->message, &bytes, &length)) return false;
    } else {
        length = xr_xir_panic_message_format(panic->detail, scratch, capacity);
        if (!length) return false;
        bytes = scratch;
    }
    *output = (XrErrorCoreMessageView){(int)panic->detail.code, bytes, length, true};
    return true;
}
XR_FUNCDEF bool xr_xir_panic_message_borrow(const XrXirPanicPayload *panic, char *scratch,
    size_t capacity, XrErrorCoreMessageView *output) {
    xr_xir_value_graph_begin();
    bool graph_outcome = xr_xir_panic_message_borrow_graph_operation(panic, scratch, capacity, output);
    xr_xir_value_graph_end();
    return graph_outcome;
}
static XrXirValueStatus xr_xir_panic_info_new_graph_operation(XrXirDomain *domain, const XrXirPanicPayload *panic,
                                             XrXirValue *output) {
    if (!domain || !unit_value(output) || !xr_xir_panic_valid(panic) || xr_xir_panic_empty(panic))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_reference_retain(&domain->references)) return XR_XIR_VALUE_REFCOUNT_LIMIT;
    XrXirPanicPayload owned = {0};
    XrXirValueStatus status = xr_xir_panic_copy(panic, &owned);
    if (status != XR_XIR_VALUE_OK) { xr_xir_domain_drop(domain); return status; }
    XirPanicInfo *info = xr_xir_domain_allocate(domain, sizeof(*info), &status);
    if (!info) { xr_xir_panic_drop(&owned); xr_xir_domain_drop(domain); return status; }
    atomic_init(&info->object.references, 1);
    info->object.domain = domain;
    info->object.type = XR_XIR_PANIC_INFO;
    info->object.arena = NULL; info->object.kind = 0;
    graph_object_initialize(&info->object);
    info->panic = (XrXirPanicPayload){0};
    xr_xir_panic_move(&owned, &info->panic);
    xr_xir_value_object_publish(&info->object);
    output->type = XR_XIR_PANIC_INFO;
    memcpy(&output->payload, &info, sizeof(info));
    return XR_XIR_VALUE_OK;
}
XR_FUNCDEF XrXirValueStatus xr_xir_panic_info_new(XrXirDomain *domain, const XrXirPanicPayload *panic,
                                             XrXirValue *output) {
    xr_xir_value_graph_begin();
    XrXirValueStatus graph_outcome = xr_xir_panic_info_new_graph_operation(domain, panic, output);
    xr_xir_value_graph_end();
    return graph_outcome;
}
static bool xr_xir_panic_info_detail_graph_operation(const XrXirValue *info, XrXirFaultDetail *detail) {
    if (!detail || !xr_xir_value_argument(info, NULL, XR_XIR_PANIC_INFO)) return false;
    *detail = ((const XirPanicInfo *) object_pointer(info))->panic.detail;
    return true;
}
XR_FUNCDEF bool xr_xir_panic_info_detail(const XrXirValue *info, XrXirFaultDetail *detail) {
    xr_xir_value_graph_begin();
    bool graph_outcome = xr_xir_panic_info_detail_graph_operation(info, detail);
    xr_xir_value_graph_end();
    return graph_outcome;
}
static XrXirValueStatus xr_xir_panic_info_message_graph_operation(const XrXirValue *info, XrXirValue *output) {
    XrXirFaultDetail detail;
    if (!unit_value(output) || !xr_xir_panic_info_detail(info, &detail)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (detail.code == XR_XIR_PANIC_ASSERTION)
        return xr_xir_value_copy(&((const XirPanicInfo *)object_pointer(info))->panic.message, output);
    char text[XR_XIR_PANIC_MESSAGE_CAPACITY];
    size_t length = xr_xir_panic_message_format(detail, text, sizeof(text));
    if (!length) return XR_XIR_VALUE_BAD_ARGUMENT;
    return xr_xir_string_new(object_pointer(info)->domain, text, length, output);
}
XR_FUNCDEF XrXirValueStatus xr_xir_panic_info_message(const XrXirValue *info, XrXirValue *output) {
    xr_xir_value_graph_begin();
    XrXirValueStatus graph_outcome = xr_xir_panic_info_message_graph_operation(info, output);
    xr_xir_value_graph_end();
    return graph_outcome;
}
