/* Strict ISO C90 compile-and-run KAT for the restricted data-pointer adapter. */
#include "aot/xrt_c90.h"

int main(void) {
    unsigned char storage[2];
    XrDataPointerProjection borrowed;
    XrDataPointerProjection stable;
    storage[0] = 3;
    storage[1] = 4;
    borrowed = xrt_data_pointer_project(storage, XR_DATA_POINTER_OWNER_BORROW);
    stable = xrt_data_pointer_project("ok", XR_DATA_POINTER_STATIC);
    if (borrowed.address != storage || borrowed.lifetime != XR_DATA_POINTER_OWNER_BORROW)
        return 1;
    if (stable.address == 0 || stable.lifetime != XR_DATA_POINTER_STATIC)
        return 2;
    return 0;
}
