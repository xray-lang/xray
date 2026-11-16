/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Duplicate diagnostics own fixed String storage after the Map guard unwinds. */
#include "xnative_execution.h"
#include "shared/xr_literal_quote_core.h"
#include "shared/xr_utf8_core.h"

typedef struct MapLiteralMessage { char *bytes; size_t count, capacity; } MapLiteralMessage;
static int map_literal_message_write(void *context, const void *bytes, size_t count) {
    MapLiteralMessage *message = context;
    if (count > SIZE_MAX - message->count) return 0;
    if (message->bytes) {
        if (message->count > message->capacity || count > message->capacity - message->count) return 0;
        if (count) memcpy(message->bytes + message->count, bytes, count);
    }
    message->count += count;
    return 1;
}
static bool map_literal_message_format(MapLiteralMessage *message, const XrtMapLiteral *literal,
    const XrtMapLiteralInput *inputs, XrtMapLiteralDuplicate duplicate) {
    XR_CHECK(duplicate.first < literal->count && duplicate.second < literal->count &&
        !literal->items[duplicate.first].kind && !literal->items[duplicate.second].kind,
        "duplicate ordinals name two explicit Map literal inputs");
    XrtMapLiteralOrigin first = literal->items[duplicate.first], second = literal->items[duplicate.second];
    const XrtStringBacking *key = inputs[duplicate.second].source.key;
    char left[32], right[32];
    int a = snprintf(left, sizeof(left), "%s%u:%u and ", literal->path_length ? ":" : "", first.line, first.column);
    int b = snprintf(right, sizeof(right), "%s%u:%u)", literal->path_length ? ":" : "", second.line, second.column);
    if (a < 0 || (size_t)a >= sizeof(left) || b < 0 || (size_t)b >= sizeof(right)) return false;
    XrValueFormatSink sink = {message, map_literal_message_write};
    return xr_value_format_text(sink, "duplicate key ") && xr_literal_quote_bytes(sink, key->bytes, key->length) &&
        xr_value_format_text(sink, " in Map literal (entries at ") &&
        xr_value_format_write(sink, literal->path, literal->path_length) &&
        xr_value_format_write(sink, left, (size_t)a) &&
        xr_value_format_write(sink, literal->path, literal->path_length) &&
        xr_value_format_write(sink, right, (size_t)b);
}
static XrtManagedStatus map_literal_message_create(XrtManagedFrame *parent, const XrtBackingTypes *types,
    const XrtMapLiteral *literal, const XrtMapLiteralInput *inputs, XrtMapLiteralDuplicate duplicate,
    XrtManagedHeader **owned) {
    MapLiteralMessage message = {0};
    if (!map_literal_message_format(&message, literal, inputs, duplicate) ||
        message.count > SIZE_MAX - sizeof(XrtStringBacking) - 1) return XRT_MANAGED_OVERFLOW;
    message.capacity = message.count;
    XrtManagedRoot *root = xrt_managed_frame_root(parent);
    XrtManagedFrame *frame = NULL;
    XrtManagedStatus status = xrt_managed_frame_enter(root, 1, &frame);
    if (status != XRT_MANAGED_OK) return status;
    XrtManagedHeader *payload = NULL;
    status = xrt_managed_slot_prepare(frame, 0, types->string,
        sizeof(XrtStringBacking) + message.capacity + 1, &payload);
    if (status == XRT_MANAGED_OK) {
        XrtStringBacking *text = (XrtStringBacking *)payload;
        message.bytes = text->bytes; message.count = 0;
        if (!map_literal_message_format(&message, literal, inputs, duplicate) || message.count != message.capacity)
            status = XRT_MANAGED_OVERFLOW;
        if (status == XRT_MANAGED_OK) {
            XrUtf8ScanResult scan = xr_utf8_core_scan_strict((const uint8_t *)text->bytes, message.count);
            status = scan.error == XR_UTF8_OK ? XRT_MANAGED_OK : XRT_MANAGED_BAD_UTF8;
            text->length = message.count; text->runes = scan.rune_count; text->bytes[message.count] = 0;
        }
        if (status == XRT_MANAGED_OK) status = xrt_managed_slot_commit(frame, 0);
        else XR_CHECK(xrt_managed_slot_abort(frame, 0) == XRT_MANAGED_OK,
            "an incomplete duplicate message retires its actual pending String");
        if (status == XRT_MANAGED_OK) status = xrt_managed_slot_take(frame, 0, owned);
    }
    XR_CHECK(xrt_managed_frame_leave(root, frame) == XRT_MANAGED_OK,
        "the ordinary diagnostic frame retires before the message leaves execution");
    return status;
}
static void map_literal_release_inputs(XrtManagedSlotLocation guard,
    const XrtMapLiteral *literal, const XrtMapLiteralInput *inputs) {
    XR_CHECK(xrt_managed_domain_enter(guard) == XRT_MANAGED_OK,
        "literal pin retirement uses the same live caller Root domain");
    xrt_map_literal_inputs_drop(literal, inputs);
    xrt_managed_domain_leave(guard);
}
XRAY_API void xrt_native_map_literal_create(XrtManagedFrame *frame, size_t slot,
    const XrtMapLiteral *literal, const XrtMapLiteralInput *inputs, uint32_t source_id) {
    XrtValueRoot *owner = xrt_value_root_current(source_id);
    XrtManagedSlotLocation guard = {0};
    const XrtBackingTypes *types = NULL;
    XrtManagedStatus status = xrt_managed_slot_location(frame, slot, &guard);
    bool entered = false, held = false;
    size_t capacity = 0;
    if (status == XRT_MANAGED_OK) {
        status = xrt_managed_domain_enter(guard); entered = status == XRT_MANAGED_OK;
    }
    if (status == XRT_MANAGED_OK) {
        status = xrt_map_literal_inputs_retain(literal, inputs, &capacity); held = status == XRT_MANAGED_OK;
    }
    if (status == XRT_MANAGED_OK) status = xrt_native_values_backings(owner, &types);
    if (status != XRT_MANAGED_OK && held) { xrt_map_literal_inputs_drop(literal, inputs); held = false; }
    if (entered) xrt_managed_domain_leave(guard);
    XrtMapLiteralDuplicate duplicate = {0};
    if (status == XRT_MANAGED_OK)
        status = xrt_map_backing_literal_create(frame, slot, types, literal, inputs, &duplicate);
    XrtManagedHeader *message = NULL;
    if (status == XRT_MANAGED_DUPLICATE_KEY)
        status = map_literal_message_create(frame, types, literal, inputs, duplicate, &message);
    if (held) map_literal_release_inputs(guard, literal, inputs);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, source_id);
    if (message) xrt_message_panic_take(&message, 434, source_id);
}
