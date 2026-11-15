/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * managed_public_cpp.cpp - Opaque C-owned atomic storage with real C linkage
 */
#include "xray_native_managed.h"
#include "xray_native_managed_root.h"
#include "xray_native_place.h"
#include <type_traits>
template<class T, class = void> struct Complete : std::false_type {};
template<class T> struct Complete<T, decltype(void(sizeof(T)))> : std::true_type {};
static_assert(!Complete<XrtManagedHeader>::value, "C++ cannot fabricate a C atomic Header");
int main() {
    XrtBillingOwner *billing = nullptr;
    XrtManagedRoot *root = nullptr;
    XrtManagedFrame *frame = nullptr;
    if (xrt_billing_owner_new(nullptr, SIZE_MAX, &billing) != XRT_MANAGED_OK) return 1;
    if (xrt_managed_root_new(billing, &root) != XRT_MANAGED_OK) return 2;
    if (xrt_managed_frame_enter(root, 1, &frame) != XRT_MANAGED_OK) return 3;
    XrtPlace place = {frame, 0, nullptr, XRT_PLACE_NULLABLE};
    const XrtManagedHeader *borrowed = nullptr;
    if (xrt_place_borrow(place, &borrowed) != XRT_MANAGED_OK || borrowed) return 4;
    if (xrt_managed_frame_leave(root, frame) != XRT_MANAGED_OK) return 5;
    if (xrt_managed_root_destroy(&root) != XRT_MANAGED_OK || root) return 6;
    xrt_billing_owner_drop(billing);
    return 0;
}
