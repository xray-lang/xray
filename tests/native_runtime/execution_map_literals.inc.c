/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Full literal diagnostics survive every execution and input owner. */
typedef struct MapLiteralCall {
    unsigned mode;
    char key[1536], path[768];
    bool completed;
} MapLiteralCall;
static void map_literal_root(void *context) {
    MapLiteralCall *call = context;
    XrtManagedRoot *root = xrt_native_managed_root(1); XrtManagedFrame *frame = NULL;
    managed_require(xrt_managed_frame_enter(root, 4, &frame));
    xrt_native_string_create(frame, 0, call->mode == 3 ? call->key : "x", call->mode == 3 ? sizeof(call->key) : 1, 1);
    xrt_native_string_create(frame, 1, "y", 1, 1);
    XrtManagedHeader *x = NULL, *y = NULL, *spread = NULL;
    managed_require(xrt_managed_slot_borrow(frame, 0, &x));
    managed_require(xrt_managed_slot_borrow(frame, 1, &y));
    XrtStringBacking *keys[] = {(XrtStringBacking *)x, (XrtStringBacking *)y};
    int64_t values[] = {8, 9}; XrtMapInput map = {keys, values, 2};
    xrt_native_map_create(frame, 2, &map, 1);
    managed_require(xrt_managed_slot_borrow(frame, 2, &spread));
    XrtMapLiteralOrigin origins[] = {{0, 10, 3}, {0, 11, 7}, {0, 12, 9}};
    XrtMapLiteralInput inputs[] = {{{.key=(const XrtStringBacking *)x}, 1},
        {{.key=(const XrtStringBacking *)y}, 2}, {{.key=(const XrtStringBacking *)x}, 3}};
    XrtMapLiteral literal = {"literal.xr", 10, origins, call->mode ? 3u : 2u};
    if (call->mode == 1 || call->mode == 2 || call->mode == 4) {
        origins[1].kind = 1; inputs[1].source.spread = (const XrtMapBacking *)spread;
        if (call->mode == 1) inputs[2].source.key = (const XrtStringBacking *)y;
    }
    if (call->mode == 3) { literal.path = call->path; literal.path_length = sizeof(call->path); }
    if (call->mode == 4) { literal.path = NULL; literal.path_length = 0; }
    if (call->mode == 5) origins[1].kind = 2;
    xrt_native_map_literal_create(frame, 3, &literal, inputs, 1);
    XrtManagedHeader *result = NULL;
    managed_require(xrt_managed_slot_borrow(frame, 3, &result));
    int64_t value = 0;
    CHECK(((const XrtMapBacking *)result)->length == 2 &&
        xrt_native_map_get(result, (const XrtStringBacking *)x, &value, 1) && value == (call->mode == 1 ? 8 : 1));
    CHECK(xrt_native_map_get(result, (const XrtStringBacking *)y, &value, 1) && value == (call->mode == 1 ? 3 : 2));
    managed_require(xrt_managed_frame_leave(root, frame)); call->completed = true;
}
static size_t map_literal_expected(const MapLiteralCall *call, char *bytes) {
    if (call->mode != 3) {
        const char *text = call->mode == 4 ?
            "duplicate key \"x\" in Map literal (entries at 10:3 and 12:9)" :
            "duplicate key \"x\" in Map literal (entries at literal.xr:10:3 and literal.xr:12:9)";
        size_t length = strlen(text); memcpy(bytes, text, length); return length;
    }
    size_t length = 0;
    const char prefix[] = "duplicate key \"", escaped[] = "\\u0000\\\"\\\\\\n";
    const char middle[] = "\" in Map literal (entries at ";
    memcpy(bytes + length, prefix, sizeof(prefix) - 1); length += sizeof(prefix) - 1;
    memcpy(bytes + length, call->key, 7); length += 7;
    memcpy(bytes + length, escaped, sizeof(escaped) - 1); length += sizeof(escaped) - 1;
    memcpy(bytes + length, call->key + 11, sizeof(call->key) - 11); length += sizeof(call->key) - 11;
    memcpy(bytes + length, middle, sizeof(middle) - 1); length += sizeof(middle) - 1;
    memcpy(bytes + length, call->path, sizeof(call->path)); length += sizeof(call->path);
    memcpy(bytes + length, ":10:3 and ", 10); length += 10;
    memcpy(bytes + length, call->path, sizeof(call->path)); length += sizeof(call->path);
    memcpy(bytes + length, ":12:9)", 6); return length + 6;
}
static void managed_map_literals(void) {
    XrtLocationOwner *map = locations("map-literal-wrapper.xr");
    for (unsigned mode = 0; mode < 6; ++mode) {
        for (size_t allowance = 0; allowance < 64; ++allowance) {
            XrtValueRoot owner = {0}; XrtBillingOwner *billing = NULL; XrtExecutionFailure failure = {0};
            MapLiteralCall call = {.mode=mode};
            memset(call.key, 'k', sizeof(call.key)); memset(call.path, 'p', sizeof(call.path));
            call.key[7] = 0; call.key[8] = '"'; call.key[9] = '\\'; call.key[10] = '\n';
            memcpy(call.key + 11, "\xc3\xa9", 2);
            CHECK(xrt_native_values_prepare(&owner, &billing) == XRT_MANAGED_OK && xrt_billing_owner_retain(billing));
#if defined(XR_NATIVE_VALUE_ALLOCATIONS)
            xrt_billing_owner_fail_after(billing, allowance);
#else
            (void)allowance;
#endif
            XrtExecutionStatus status = xrt_run_embedded_owned(map, &failure, map_literal_root, &call, &owner);
            CHECK(xrt_value_root_empty(&owner) && !xrt_value_root_active());
            bool injected = status == XRT_EXECUTION_FAILURE && failure.kind == XRT_FAILURE_VALUE &&
                failure.detail.value.status == XR_XIR_VALUE_OOM;
            if (!injected) {
                if (mode < 2) CHECK(status == XRT_EXECUTION_COMPLETED && call.completed);
                else if (mode < 5) {
                    CHECK(status == XRT_EXECUTION_FAILURE && !call.completed &&
                        failure.kind == XRT_FAILURE_MESSAGE_PANIC && failure.detail.message.code == 434 &&
                        failure.detail.message.source_id == 1 && failure.detail.message.message);
                    char expected[4096]; size_t length = map_literal_expected(&call, expected);
                    CHECK(length < sizeof(expected));
                    memset(call.key, 0xa5, sizeof(call.key)); memset(call.path, 0xa5, sizeof(call.path));
                    const XrtStringBacking *message = (const XrtStringBacking *)failure.detail.message.message;
                    CHECK(message->length == length && !memcmp(message->bytes, expected, length));
                } else CHECK(status == XRT_EXECUTION_FAILURE && !call.completed && failure.kind == XRT_FAILURE_VALUE &&
                    failure.detail.value.status == XR_XIR_VALUE_BAD_ARGUMENT);
            } else CHECK(!call.completed);
            xrt_execution_failure_drop(&failure);
            CHECK(xrt_billing_owner_stats(billing).blocks == 1);
            xrt_billing_owner_drop(billing);
            if (!injected) break;
            CHECK(allowance + 1 < 64);
        }
    }
    xrt_locations_release(map);
}
