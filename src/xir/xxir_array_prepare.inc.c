/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_array_prepare.inc.c - Private replacement storage before Array publication
 */
static unsigned char *array_slot(XirArray *array, size_t index) {
    XR_CHECK(index < array->capacity, "array slot requires reserved capacity");
    return array->data ? array->data + index * array->stride : NULL;
}
static void array_prepared_transfer(StoragePrepared *prepared, XirArray *array, size_t index) {
    XR_CHECK(prepared->owns && prepared->stride == array->stride, "array transfer needs an owned exact slot");
    if (array->stride) memcpy(array_slot(array, index), prepared->bytes, array->stride);
    prepared->owns = false;
}
