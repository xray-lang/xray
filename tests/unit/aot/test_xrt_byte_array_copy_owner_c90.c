/* Strict ISO C90 compile-and-run KAT for the restricted byte-array copy adapter. */
#include "aot/xrt_c90.h"

int main(void) {
    uint8_t bytes[6];
    XrByteArrayCopyResult copied;
    XrByteArrayCopyResult rejected;
    bytes[0] = 1;
    bytes[1] = 2;
    bytes[2] = 3;
    bytes[3] = 4;
    bytes[4] = 5;
    bytes[5] = 6;
    copied = xrt_byte_array_copy_semantics(XR_BYTE_ARRAY_COPY_WITHIN, bytes, 6, XR_ELEM_U8,
                                           bytes, 6, XR_ELEM_U8, 0, 2, 4);
    rejected = xrt_byte_array_copy_semantics(XR_BYTE_ARRAY_COPY_FROM, bytes, 6, XR_ELEM_U8,
                                             bytes, 6, XR_ELEM_U8, INT64_C(4294967296), 0, 1);
    if (copied.status != XR_BYTE_ARRAY_COPY_OK || !copied.changed || bytes[2] != 1 || bytes[5] != 4)
        return 1;
    if (rejected.status != XR_BYTE_ARRAY_COPY_OUT_OF_BOUNDS || rejected.changed)
        return 2;
    return 0;
}
