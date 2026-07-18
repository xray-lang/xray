/* task-208: authoritative FFI scalar descriptors and native-size memory IO. */

#include "../test_framework.h"

#include "runtime/value/xffi_sig.h"
#include "vm/xvm_ffi.h"
#include "shared/xr_array_core.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

TEST(ffi_scalar_descriptor_matrix_is_complete) {
    ASSERT_EQ_INT(XR_FFI_T_COUNT, 15);
    ASSERT_TRUE(xr_abi_scalar_desc(UINT8_MAX) == NULL);
    ASSERT_FALSE(xr_ffi_type_is_memory_scalar(XR_FFI_T_VOID));

    for (uint8_t code = XR_FFI_T_BOOL; code < XR_FFI_T_COUNT; code++) {
        const XrAbiScalarDesc *desc = xr_abi_scalar_desc(code);
        ASSERT_NOT_NULL(desc);
        ASSERT_EQ_INT(desc->ffi_type, code);
        ASSERT_TRUE(desc->is_memory_scalar);
        ASSERT_NOT_NULL(desc->c_type);
        ASSERT_TRUE(xr_ffi_type_is_memory_scalar(code));
    }

    const XrAbiScalarDesc *usize = xr_abi_scalar_desc(XR_FFI_T_SIZE);
    const XrAbiScalarDesc *isize = xr_abi_scalar_desc(XR_FFI_T_SSIZE);
    ASSERT_EQ_INT(usize->native_type, XR_NATIVE_USIZE);
    ASSERT_EQ_INT(isize->native_type, XR_NATIVE_ISIZE);
    ASSERT_FALSE(usize->is_signed);
    ASSERT_TRUE(isize->is_signed);
    ASSERT_EQ_INT(xr_abi_scalar_width(usize, 4), 4);
    ASSERT_EQ_INT(xr_abi_scalar_width(usize, 8), 8);
    ASSERT_EQ_INT(xr_abi_scalar_width(isize, 4), 4);
    ASSERT_EQ_INT(xr_abi_scalar_width(isize, 8), 8);
    ASSERT_EQ_INT(xr_abi_scalar_width(usize, 16), 0);
    ASSERT_TRUE(strcmp(usize->c_type, "size_t") == 0);
    ASSERT_TRUE(strcmp(isize->c_type, "ptrdiff_t") == 0);
}

TEST(ffi_native_size_memory_roundtrip) {
    uint8_t bytes[32] = {0};
    uintptr_t usize_bits = UINTPTR_MAX;
    xr_Integer signed_min =
        sizeof(ptrdiff_t) == 4 ? (xr_Integer) INT32_MIN : (xr_Integer) INT64_MIN;

    xr_ffi_ptr_store((uintptr_t) &bytes[1], XR_FFI_T_SIZE, xr_int((xr_Integer) usize_bits),
                     XR_ENDIAN_LE);
    xr_ffi_ptr_store((uintptr_t) &bytes[1 + sizeof(size_t)], XR_FFI_T_SSIZE, xr_int(signed_min),
                     XR_ENDIAN_LE);

    XrValue loaded_usize = xr_ffi_ptr_load((uintptr_t) &bytes[1], XR_FFI_T_SIZE, XR_ENDIAN_LE);
    XrValue loaded_isize =
        xr_ffi_ptr_load((uintptr_t) &bytes[1 + sizeof(size_t)], XR_FFI_T_SSIZE, XR_ENDIAN_LE);
    ASSERT_TRUE(XR_IS_INT(loaded_usize));
    ASSERT_TRUE(XR_IS_INT(loaded_isize));
    ASSERT_EQ_UINT((uint64_t) XR_TO_INT(loaded_usize), (uint64_t) (xr_Integer) usize_bits);
    ASSERT_EQ_INT(XR_TO_INT(loaded_isize), signed_min);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("FFI scalar descriptors");
RUN_TEST(ffi_scalar_descriptor_matrix_is_complete);
RUN_TEST(ffi_native_size_memory_roundtrip);
TEST_MAIN_END()
