/* Compile and execute the no-libc representation adapter with the host MSVC
 * toolchain. Cross-target compilation remains covered by the AOT filetests. */

#include "aot/xrt_core_freestanding.h"

int main(void) {
    if (xrt_typeof_id(XR_NULL_VAL) != XR_TYPE_IDENTITY_CORE_NULL)
        return 1;
    if (xrt_typeof_id(XR_TRUE_VAL) != XR_TYPE_IDENTITY_CORE_BOOL)
        return 2;
    if (xrt_typeof_id(XR_FROM_INT(7)) != XR_TYPE_IDENTITY_CORE_INT)
        return 3;
    if (xrt_typeof_id(XR_FROM_FLOAT(1.5)) != XR_TYPE_IDENTITY_CORE_FLOAT)
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
    return 0;
}
