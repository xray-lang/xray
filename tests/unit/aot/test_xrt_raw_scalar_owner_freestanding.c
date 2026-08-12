/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xrt_raw_scalar_owner_freestanding.c - freestanding raw scalar owner KAT
 */

#include "../test_framework.h"
#include "aot/xrt_core_freestanding.h"

_Noreturn void xr_hook_panic(const char *message, size_t len) {
    (void) fwrite(message, 1, len, stderr);
    (void) fputc('\n', stderr);
    abort();
}

TEST(freestanding_raw_scalar_owner_preserves_width_endian_and_signedness) {
    uint8_t bytes[32] = {0};
    uint8_t *ptr = bytes + 1;

    xrt_raw_scalar_access_store_i64(ptr, XR_RAW_SCALAR_I16, sizeof(void *),
                                    XR_RAW_ENDIAN_LE, -4660);
    ASSERT_EQ_INT(bytes[1], UINT8_C(0xcc));
    ASSERT_EQ_INT(bytes[2], UINT8_C(0xed));
    ASSERT_EQ_INT(xrt_raw_scalar_access_load_i64(ptr, XR_RAW_SCALAR_I16, sizeof(void *),
                                                XR_RAW_ENDIAN_LE),
                  -4660);

    xrt_raw_scalar_access_store_i64(ptr, XR_RAW_SCALAR_U32, sizeof(void *),
                                    XR_RAW_ENDIAN_BE, INT64_C(0x12345678));
    ASSERT_EQ_INT(bytes[1], UINT8_C(0x12));
    ASSERT_EQ_INT(bytes[4], UINT8_C(0x78));
    ASSERT_EQ_INT(xrt_raw_scalar_access_load_i64(ptr, XR_RAW_SCALAR_U32, sizeof(void *),
                                                XR_RAW_ENDIAN_BE),
                  INT64_C(0x12345678));
}

TEST(freestanding_raw_scalar_owner_preserves_float_and_pointer_bits) {
    uint8_t bytes[32] = {0};
    uint8_t *ptr = bytes + 1;
    uint8_t target = 0;
    double value = xr_raw_f64_from_bits(UINT64_C(0x7ff8000000001234));

    xrt_raw_scalar_access_store_f64(ptr, XR_RAW_SCALAR_F64, sizeof(void *),
                                    XR_RAW_ENDIAN_NATIVE, value);
    ASSERT_EQ_INT(xr_raw_f64_to_bits(xrt_raw_scalar_access_load_f64(
                      ptr, XR_RAW_SCALAR_F64, sizeof(void *), XR_RAW_ENDIAN_NATIVE)),
                  UINT64_C(0x7ff8000000001234));

    xrt_raw_scalar_access_store_pointer(ptr, XR_RAW_SCALAR_PTR, sizeof(void *),
                                        XR_RAW_ENDIAN_NATIVE, &target);
    ASSERT_EQ_PTR(xrt_raw_scalar_access_load_pointer(
                      ptr, XR_RAW_SCALAR_PTR, sizeof(void *), XR_RAW_ENDIAN_NATIVE),
                  &target);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("AOT Freestanding Raw Scalar Owner");
RUN_TEST(freestanding_raw_scalar_owner_preserves_width_endian_and_signedness);
RUN_TEST(freestanding_raw_scalar_owner_preserves_float_and_pointer_bits);

TEST_MAIN_END()
