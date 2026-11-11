/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_array_unsigned.inc.c - Exact unsigned payloads on canonical compact Arrays
 */
static XrXirValue array_unsigned_value(XrXirType type, uint64_t bits) {
    XrXirValue value = {.type=(uint32_t)type};
    memcpy(&value.payload, &bits, sizeof(bits));
    return value;
}
XRAY_API XrtArray *xrt_array_u8_new(const uint8_t *values, size_t count, uint32_t source_id) {
    return array_new(values, count, XRT_ARRAY_U8, source_id);
}
XRAY_API XrtArray *xrt_array_u64_new(const uint64_t *values, size_t count, uint32_t source_id) {
    return array_new(values, count, XRT_ARRAY_U64, source_id);
}
XRAY_API XrtArray *xrt_array_u8_repeat(int64_t length, uint8_t fill, uint32_t source_id) {
    return array_repeat(length, array_unsigned_value(XR_XIR_U8, fill), XRT_ARRAY_U8, source_id);
}
XRAY_API XrtArray *xrt_array_u64_repeat(int64_t length, uint64_t fill, uint32_t source_id) {
    return array_repeat(length, array_unsigned_value(XR_XIR_U64, fill), XRT_ARRAY_U64, source_id);
}
XRAY_API uint8_t xrt_array_u8_get(const XrtArray *array, int64_t index, uint32_t source_id) {
    XrtValueRoot *root = array_get(array, index, XRT_ARRAY_U8, source_id);
    uint8_t value = (uint8_t)root->pending.payload;
    xr_xir_value_drop(&root->pending);
    return value;
}
XRAY_API uint64_t xrt_array_u64_get(const XrtArray *array, int64_t index, uint32_t source_id) {
    XrtValueRoot *root = array_get(array, index, XRT_ARRAY_U64, source_id);
    uint64_t value;
    memcpy(&value, &root->pending.payload, sizeof(value));
    xr_xir_value_drop(&root->pending);
    return value;
}
XRAY_API void xrt_array_u8_set(XrtArray **array, int64_t index, uint8_t value, uint32_t source_id) {
    array_mutate(array, index, array_unsigned_value(XR_XIR_U8, value), false, source_id);
}
XRAY_API void xrt_array_u64_set(XrtArray **array, int64_t index, uint64_t value, uint32_t source_id) {
    array_mutate(array, index, array_unsigned_value(XR_XIR_U64, value), false, source_id);
}
XRAY_API void xrt_array_u8_push(XrtArray **array, uint8_t value, uint32_t source_id) {
    array_mutate(array, 0, array_unsigned_value(XR_XIR_U8, value), true, source_id);
}
XRAY_API void xrt_array_u64_push(XrtArray **array, uint64_t value, uint32_t source_id) {
    array_mutate(array, 0, array_unsigned_value(XR_XIR_U64, value), true, source_id);
}
