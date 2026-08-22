/* Compile and execute the no-libc representation adapter with the host MSVC
 * toolchain. Cross-target compilation remains covered by the AOT filetests. */

#include "aot/xrt_core_freestanding.h"

int main(void) {
    if (xrt_typeof_id(XR_NULL_VAL) != XR_TYPE_IDENTITY_CORE_NULL)
        return 1;
    if (xrt_typeof_id(XR_TRUE_VAL) != XR_TYPE_IDENTITY_CORE_BOOL)
        return 2;
    if (xrt_typeof_id(XR_FROM_INT(7)) != XR_TYPE_IDENTITY_CORE_I64)
        return 3;
    if (xrt_typeof_id(XR_FROM_FLOAT(1.5)) != XR_TYPE_IDENTITY_CORE_F64)
        return 4;
    if (xrt_typeof_id(XR_FROM_RUNE('X')) != XR_TYPE_IDENTITY_CORE_RUNE)
        return 5;

    XRT_STR_LIT_DEF(literal_header, "x");
    XrValue literal = xr_str_lit(&literal_header);
    char dynamic_data[] = "x";
    xrt_str_t dynamic_header = {1, 1, 0, 0, dynamic_data};
    XrValue dynamic = xr_str_value_from_ptr(&dynamic_header);
    if (xrt_typeof_id(literal) != XR_TYPE_IDENTITY_CORE_STRING)
        return 6;
    if (xrt_typeof_id(dynamic) != XR_TYPE_IDENTITY_CORE_STRING)
        return 7;

    XrValue array = xr_mkheap((void *) (uintptr_t) 1, XR_TARRAY);
    XrValue map = xr_mkheap((void *) (uintptr_t) 1, XR_TMAP);
    XrValue set = xr_mkheap((void *) (uintptr_t) 1, XR_TSET);
    if (xrt_typeof_id(array) != XR_TYPE_IDENTITY_CORE_ARRAY)
        return 8;
    if (xrt_typeof_id(map) != XR_TYPE_IDENTITY_CORE_MAP)
        return 9;
    if (xrt_typeof_id(set) != XR_TYPE_IDENTITY_CORE_SET)
        return 10;
    if (xrt_bits_exact_eval(xr_bits_exact_kernel_rotl, INT64_C(0x81), INT64_C(1),
                            XR_NATIVE_U8) != INT64_C(0x03))
        return 11;
    if (xrt_bits_exact_eval(xr_bits_exact_kernel_rotr, INT64_C(0x81), INT64_C(1),
                            XR_NATIVE_U8) != INT64_C(0xc0))
        return 12;
    if (xrt_bits_exact_eval(xr_bits_exact_kernel_bswap, INT64_C(0x1234), INT64_C(0),
                            XR_NATIVE_U16) != INT64_C(0x3412))
        return 13;
    if (xrt_bits_exact_eval(xr_bits_exact_kernel_popcount, INT64_C(0x0101), INT64_C(0),
                            XR_NATIVE_U16) != INT64_C(2))
        return 14;
    if (xrt_bits_exact_eval(xr_bits_exact_kernel_clz, INT64_C(0x0100), INT64_C(0),
                            XR_NATIVE_U16) != INT64_C(7))
        return 15;
    if (xrt_bits_exact_eval(xr_bits_exact_kernel_ctz, INT64_C(0x0100), INT64_C(0),
                            XR_NATIVE_U16) != INT64_C(8))
        return 16;
    if (xrt_bits_exact_eval(xr_bits_exact_kernel_mul_high, -INT64_C(1), INT64_C(2),
                            XR_NATIVE_U64) != INT64_C(1))
        return 17;
    if (xrt_numeric_width_eval(xr_numeric_narrow_i8, INT64_C(0x1ff)) != -INT64_C(1))
        return 18;
    if (xrt_numeric_width_eval(xr_numeric_narrow_u8, INT64_C(0x1ff)) != INT64_C(0xff))
        return 19;
    if (xrt_numeric_width_eval(xr_numeric_narrow_i16, INT64_C(0x18000)) !=
        -INT64_C(0x8000))
        return 20;
    if (xrt_numeric_width_eval(xr_numeric_narrow_u16, INT64_C(0x18000)) !=
        INT64_C(0x8000))
        return 21;
    if (xrt_numeric_width_eval(xr_numeric_narrow_i32, INT64_C(0x180000000)) != INT32_MIN)
        return 22;
    if (xrt_numeric_width_eval(xr_numeric_narrow_u32, INT64_C(0x180000000)) !=
        INT64_C(0x80000000))
        return 23;
    if (xrt_numeric_width_eval(xr_numeric_narrow_f32, 16777217.0) != 16777216.0)
        return 24;
    if (xr_numeric_double_to_bits(xrt_numeric_width_eval(
            xr_numeric_narrow_f32,
            xr_numeric_double_from_bits(UINT64_C(0x7ff123456789abcd)))) !=
        xr_numeric_double_to_bits(
            (double) xr_numeric_float_from_bits(XR_NUMERIC_CANONICAL_F32_NAN)))
        return 25;
    if (xr_numeric_double_to_bits(xrt_numeric_width_eval(
            xr_numeric_narrow_f32, xr_numeric_power_of_two(128))) !=
        UINT64_C(0x7ff0000000000000))
        return 26;
    if (xrt_numeric_width_eval(xr_numeric_widen_i8, INT64_C(0xff)) != -INT64_C(1))
        return 27;
    if (xrt_numeric_width_eval(xr_numeric_widen_u8, INT64_C(0xff)) != INT64_C(0xff))
        return 28;
    if (xrt_numeric_width_eval(xr_numeric_widen_i16, INT64_C(0xffff)) != -INT64_C(1))
        return 29;
    if (xrt_numeric_width_eval(xr_numeric_widen_u16, INT64_C(0xffff)) != INT64_C(0xffff))
        return 30;
    if (xrt_numeric_width_eval(xr_numeric_widen_i32, INT64_C(0xffffffff)) != -INT64_C(1))
        return 31;
    if (xrt_numeric_width_eval(xr_numeric_widen_u32, -INT64_C(1)) != INT64_C(0xffffffff))
        return 32;
    if (xrt_numeric_width_eval(xr_numeric_widen_f32, 16777217.0) != 16777216.0)
        return 33;
    return 0;
}
