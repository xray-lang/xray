/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_typed_array.c - Unit tests for TypedArray and ArrayBuffer objects
 */

#include "../test_framework.h"
#include "../test_helper.h"
#include "runtime/object/xarray_buffer.h"
#include "runtime/object/xtyped_array.h"

static XrayIsolate *X = NULL;
static XrCoroutine *main_coro = NULL;

/* ========== Setup / Teardown ========== */

static void setup(void) {
    X = xray_isolate_new(NULL);
    ASSERT_NOT_NULL(X);
    main_coro = xr_test_init_coro(X);
    ASSERT_NOT_NULL(main_coro);
}

static void teardown(void) {
    if (X) {
        xray_isolate_delete(X);
        X = NULL;
        main_coro = NULL;
    }
}

/* ========== ArrayBuffer Tests ========== */

TEST(arraybuffer_new) {
    setup();
    XrArrayBuffer *buf = xr_array_buffer_new(main_coro, 16);
    ASSERT_NOT_NULL(buf);
    ASSERT_EQ_INT(buf->byte_length, 16);
    ASSERT_NOT_NULL(buf->data);
    teardown();
}

TEST(arraybuffer_new_zero_length) {
    setup();
    XrArrayBuffer *buf = xr_array_buffer_new(main_coro, 0);
    ASSERT_NOT_NULL(buf);
    ASSERT_EQ_INT(buf->byte_length, 0);
    teardown();
}

TEST(arraybuffer_new_large) {
    setup();
    XrArrayBuffer *buf = xr_array_buffer_new(main_coro, 1024 * 1024);
    ASSERT_NOT_NULL(buf);
    ASSERT_EQ_INT(buf->byte_length, 1024 * 1024);
    teardown();
}

TEST(arraybuffer_slice) {
    setup();
    XrArrayBuffer *buf = xr_array_buffer_new(main_coro, 16);
    ASSERT_NOT_NULL(buf);
    
    // Fill with test data
    for (int i = 0; i < 16; i++) {
        buf->data[i] = (uint8_t)i;
    }
    
    XrArrayBuffer *slice = xr_array_buffer_slice(main_coro, buf, 4, 12);
    ASSERT_NOT_NULL(slice);
    ASSERT_EQ_INT(slice->byte_length, 8);
    
    // Verify slice data
    for (int i = 0; i < 8; i++) {
        ASSERT_EQ_INT(slice->data[i], i + 4);
    }
    teardown();
}

TEST(arraybuffer_slice_negative_indices) {
    setup();
    XrArrayBuffer *buf = xr_array_buffer_new(main_coro, 16);
    ASSERT_NOT_NULL(buf);
    
    for (int i = 0; i < 16; i++) {
        buf->data[i] = (uint8_t)i;
    }
    
    // Negative end index means from end
    XrArrayBuffer *slice = xr_array_buffer_slice(main_coro, buf, 0, -4);
    ASSERT_NOT_NULL(slice);
    ASSERT_EQ_INT(slice->byte_length, 12);
    teardown();
}

/* ========== TypedArray Creation Tests ========== */

TEST(uint8array_new) {
    setup();
    XrTypedArray *arr = xr_typed_array_new(main_coro, XR_TYPED_UINT8, 10);
    ASSERT_NOT_NULL(arr);
    ASSERT_EQ_INT(arr->length, 10);
    ASSERT_EQ_INT(arr->element_type, XR_TYPED_UINT8);
    ASSERT_EQ_INT(xr_typed_array_byte_length(arr), 10);
    teardown();
}

TEST(int8array_new) {
    setup();
    XrTypedArray *arr = xr_typed_array_new(main_coro, XR_TYPED_INT8, 10);
    ASSERT_NOT_NULL(arr);
    ASSERT_EQ_INT(arr->length, 10);
    ASSERT_EQ_INT(arr->element_type, XR_TYPED_INT8);
    teardown();
}

TEST(uint16array_new) {
    setup();
    XrTypedArray *arr = xr_typed_array_new(main_coro, XR_TYPED_UINT16, 10);
    ASSERT_NOT_NULL(arr);
    ASSERT_EQ_INT(arr->length, 10);
    ASSERT_EQ_INT(xr_typed_array_byte_length(arr), 20);
    teardown();
}

TEST(int32array_new) {
    setup();
    XrTypedArray *arr = xr_typed_array_new(main_coro, XR_TYPED_INT32, 10);
    ASSERT_NOT_NULL(arr);
    ASSERT_EQ_INT(arr->length, 10);
    ASSERT_EQ_INT(xr_typed_array_byte_length(arr), 40);
    teardown();
}

TEST(float32array_new) {
    setup();
    XrTypedArray *arr = xr_typed_array_new(main_coro, XR_TYPED_FLOAT32, 10);
    ASSERT_NOT_NULL(arr);
    ASSERT_EQ_INT(arr->length, 10);
    ASSERT_EQ_INT(xr_typed_array_byte_length(arr), 40);
    teardown();
}

TEST(float64array_new) {
    setup();
    XrTypedArray *arr = xr_typed_array_new(main_coro, XR_TYPED_FLOAT64, 10);
    ASSERT_NOT_NULL(arr);
    ASSERT_EQ_INT(arr->length, 10);
    ASSERT_EQ_INT(xr_typed_array_byte_length(arr), 80);
    teardown();
}

/* ========== TypedArray Get/Set Tests ========== */

TEST(uint8array_get_set) {
    setup();
    XrTypedArray *arr = xr_typed_array_new(main_coro, XR_TYPED_UINT8, 4);
    
    xr_typed_array_set(arr, 0, xr_int(255));
    xr_typed_array_set(arr, 1, xr_int(128));
    xr_typed_array_set(arr, 2, xr_int(0));
    xr_typed_array_set(arr, 3, xr_int(64));
    
    ASSERT_EQ_INT(XR_TO_INT(xr_typed_array_get(arr, 0)), 255);
    ASSERT_EQ_INT(XR_TO_INT(xr_typed_array_get(arr, 1)), 128);
    ASSERT_EQ_INT(XR_TO_INT(xr_typed_array_get(arr, 2)), 0);
    ASSERT_EQ_INT(XR_TO_INT(xr_typed_array_get(arr, 3)), 64);
    teardown();
}

TEST(uint8array_overflow) {
    setup();
    XrTypedArray *arr = xr_typed_array_new(main_coro, XR_TYPED_UINT8, 2);
    
    // Value > 255 should wrap
    xr_typed_array_set(arr, 0, xr_int(256));
    xr_typed_array_set(arr, 1, xr_int(300));
    
    ASSERT_EQ_INT(XR_TO_INT(xr_typed_array_get(arr, 0)), 0);
    ASSERT_EQ_INT(XR_TO_INT(xr_typed_array_get(arr, 1)), 44);
    teardown();
}

TEST(int8array_signed) {
    setup();
    XrTypedArray *arr = xr_typed_array_new(main_coro, XR_TYPED_INT8, 4);
    
    xr_typed_array_set(arr, 0, xr_int(127));
    xr_typed_array_set(arr, 1, xr_int(-128));
    xr_typed_array_set(arr, 2, xr_int(-1));
    xr_typed_array_set(arr, 3, xr_int(0));
    
    ASSERT_EQ_INT(XR_TO_INT(xr_typed_array_get(arr, 0)), 127);
    ASSERT_EQ_INT(XR_TO_INT(xr_typed_array_get(arr, 1)), -128);
    ASSERT_EQ_INT(XR_TO_INT(xr_typed_array_get(arr, 2)), -1);
    ASSERT_EQ_INT(XR_TO_INT(xr_typed_array_get(arr, 3)), 0);
    teardown();
}

TEST(int32array_large_values) {
    setup();
    XrTypedArray *arr = xr_typed_array_new(main_coro, XR_TYPED_INT32, 4);
    
    xr_typed_array_set(arr, 0, xr_int(2147483647));   // INT32_MAX
    xr_typed_array_set(arr, 1, xr_int(-2147483648));  // INT32_MIN
    xr_typed_array_set(arr, 2, xr_int(1000000));
    xr_typed_array_set(arr, 3, xr_int(-1000000));
    
    ASSERT_EQ_INT(XR_TO_INT(xr_typed_array_get(arr, 0)), 2147483647);
    ASSERT_EQ_INT(XR_TO_INT(xr_typed_array_get(arr, 1)), -2147483648);
    ASSERT_EQ_INT(XR_TO_INT(xr_typed_array_get(arr, 2)), 1000000);
    ASSERT_EQ_INT(XR_TO_INT(xr_typed_array_get(arr, 3)), -1000000);
    teardown();
}

TEST(float32array_values) {
    setup();
    XrTypedArray *arr = xr_typed_array_new(main_coro, XR_TYPED_FLOAT32, 4);
    
    xr_typed_array_set(arr, 0, xr_float(3.5));
    xr_typed_array_set(arr, 1, xr_float(-2.5));
    xr_typed_array_set(arr, 2, xr_float(0.0));
    xr_typed_array_set(arr, 3, xr_float(1.0));
    
    ASSERT_FLOAT_EQ(XR_TO_FLOAT(xr_typed_array_get(arr, 0)), 3.5, 0.0001);
    ASSERT_FLOAT_EQ(XR_TO_FLOAT(xr_typed_array_get(arr, 1)), -2.5, 0.0001);
    ASSERT_FLOAT_EQ(XR_TO_FLOAT(xr_typed_array_get(arr, 2)), 0.0, 0.0001);
    ASSERT_FLOAT_EQ(XR_TO_FLOAT(xr_typed_array_get(arr, 3)), 1.0, 0.0001);
    teardown();
}

TEST(float64array_precision) {
    setup();
    XrTypedArray *arr = xr_typed_array_new(main_coro, XR_TYPED_FLOAT64, 2);
    
    double pi = 3.141592653589793;
    double e = 2.718281828459045;
    
    xr_typed_array_set(arr, 0, xr_float(pi));
    xr_typed_array_set(arr, 1, xr_float(e));
    
    ASSERT_FLOAT_EQ(XR_TO_FLOAT(xr_typed_array_get(arr, 0)), pi, 1e-15);
    ASSERT_FLOAT_EQ(XR_TO_FLOAT(xr_typed_array_get(arr, 1)), e, 1e-15);
    teardown();
}

/* ========== TypedArray Bounds Tests ========== */

TEST(typed_array_out_of_bounds_get) {
    setup();
    XrTypedArray *arr = xr_typed_array_new(main_coro, XR_TYPED_UINT8, 4);
    
    // Out of bounds should return null
    XrValue result = xr_typed_array_get(arr, 10);
    ASSERT_TRUE(XR_IS_NULL(result));
    teardown();
}

TEST(typed_array_out_of_bounds_set) {
    setup();
    XrTypedArray *arr = xr_typed_array_new(main_coro, XR_TYPED_UINT8, 4);
    
    // Set valid value first
    xr_typed_array_set(arr, 0, xr_int(100));
    
    // Out of bounds set should be ignored (no crash)
    xr_typed_array_set(arr, 10, xr_int(200));
    
    // Original value should be unchanged
    ASSERT_EQ_INT(XR_TO_INT(xr_typed_array_get(arr, 0)), 100);
    teardown();
}

/* ========== TypedArray from Buffer Tests ========== */

TEST(uint8array_from_buffer) {
    setup();
    XrArrayBuffer *buf = xr_array_buffer_new(main_coro, 16);
    ASSERT_NOT_NULL(buf);
    
    // Fill buffer with test data
    for (int i = 0; i < 16; i++) {
        buf->data[i] = (uint8_t)(i * 10);
    }
    
    XrTypedArray *arr = xr_typed_array_from_buffer(main_coro, buf, XR_TYPED_UINT8, 0, 16);
    ASSERT_NOT_NULL(arr);
    ASSERT_EQ_INT(arr->length, 16);
    
    // Verify data
    for (int i = 0; i < 16; i++) {
        ASSERT_EQ_INT(XR_TO_INT(xr_typed_array_get(arr, i)), i * 10);
    }
    teardown();
}

TEST(int32array_from_buffer_with_offset) {
    setup();
    XrArrayBuffer *buf = xr_array_buffer_new(main_coro, 32);
    ASSERT_NOT_NULL(buf);
    
    // Fill buffer with int32 values
    int32_t *data = (int32_t*)buf->data;
    for (int i = 0; i < 8; i++) {
        data[i] = i * 100;
    }
    
    // Create view starting at byte offset 8 (2 int32s)
    XrTypedArray *arr = xr_typed_array_from_buffer(main_coro, buf, XR_TYPED_INT32, 8, 4);
    ASSERT_NOT_NULL(arr);
    ASSERT_EQ_INT(arr->length, 4);
    
    // Should read from offset 2 in original buffer
    ASSERT_EQ_INT(XR_TO_INT(xr_typed_array_get(arr, 0)), 200);
    ASSERT_EQ_INT(XR_TO_INT(xr_typed_array_get(arr, 1)), 300);
    teardown();
}

/* ========== TypedArray Fill Tests ========== */

TEST(uint8array_fill) {
    setup();
    XrTypedArray *arr = xr_typed_array_new(main_coro, XR_TYPED_UINT8, 10);
    
    xr_typed_array_fill(arr, xr_int(42), 0, 10);
    
    for (int i = 0; i < 10; i++) {
        ASSERT_EQ_INT(XR_TO_INT(xr_typed_array_get(arr, i)), 42);
    }
    teardown();
}

TEST(uint8array_fill_partial) {
    setup();
    XrTypedArray *arr = xr_typed_array_new(main_coro, XR_TYPED_UINT8, 10);
    
    xr_typed_array_fill(arr, xr_int(0), 0, 10);
    xr_typed_array_fill(arr, xr_int(255), 3, 7);
    
    ASSERT_EQ_INT(XR_TO_INT(xr_typed_array_get(arr, 0)), 0);
    ASSERT_EQ_INT(XR_TO_INT(xr_typed_array_get(arr, 2)), 0);
    ASSERT_EQ_INT(XR_TO_INT(xr_typed_array_get(arr, 3)), 255);
    ASSERT_EQ_INT(XR_TO_INT(xr_typed_array_get(arr, 6)), 255);
    ASSERT_EQ_INT(XR_TO_INT(xr_typed_array_get(arr, 7)), 0);
    teardown();
}

/* ========== Main ========== */

int main(void) {
    printf("\n========== TypedArray Unit Tests ==========\n");
    
    RUN_TEST_SUITE("ArrayBuffer Creation");
    RUN_TEST(arraybuffer_new);
    RUN_TEST(arraybuffer_new_zero_length);
    RUN_TEST(arraybuffer_new_large);
    RUN_TEST(arraybuffer_slice);
    RUN_TEST(arraybuffer_slice_negative_indices);
    
    RUN_TEST_SUITE("TypedArray Creation");
    RUN_TEST(uint8array_new);
    RUN_TEST(int8array_new);
    RUN_TEST(uint16array_new);
    RUN_TEST(int32array_new);
    RUN_TEST(float32array_new);
    RUN_TEST(float64array_new);
    
    RUN_TEST_SUITE("TypedArray Get/Set");
    RUN_TEST(uint8array_get_set);
    RUN_TEST(uint8array_overflow);
    RUN_TEST(int8array_signed);
    RUN_TEST(int32array_large_values);
    RUN_TEST(float32array_values);
    RUN_TEST(float64array_precision);
    
    RUN_TEST_SUITE("TypedArray Bounds");
    RUN_TEST(typed_array_out_of_bounds_get);
    RUN_TEST(typed_array_out_of_bounds_set);
    
    RUN_TEST_SUITE("TypedArray from Buffer");
    RUN_TEST(uint8array_from_buffer);
    RUN_TEST(int32array_from_buffer_with_offset);
    
    RUN_TEST_SUITE("TypedArray Fill");
    RUN_TEST(uint8array_fill);
    RUN_TEST(uint8array_fill_partial);
    
    TEST_REPORT();
    return xr_tests_failed > 0 ? 1 : 0;
}
