/* C++11 compile-and-run contract for the C-layout AOT atomic bridge. */

#include "shared/xr_array_abi.h"
#include "shared/xr_obj_header.h"
#include "shared/xr_sync_core.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

static_assert(std::is_standard_layout<XrObjHeader>::value,
              "XrObjHeader must remain standard-layout in C++");
static_assert(sizeof(XrObjHeader) == 16, "XrObjHeader C++ ABI size");
static_assert(offsetof(XrObjHeader, type) == 0, "XrObjHeader.type C++ ABI offset");
static_assert(offsetof(XrObjHeader, extra) == 2, "XrObjHeader.extra C++ ABI offset");
static_assert(offsetof(XrObjHeader, refcount) == 4, "XrObjHeader.refcount C++ ABI offset");
static_assert(offsetof(XrObjHeader, objsize) == 8, "XrObjHeader.objsize C++ ABI offset");
static_assert(offsetof(XrObjHeader, _rsv) == 12, "XrObjHeader._rsv C++ ABI offset");

int main() {
    _Atomic(int32_t) value = 0;
    atomic_store_explicit(&value, 3, memory_order_release);
    if (atomic_load_explicit(&value, memory_order_acquire) != 3)
        return 1;
    if (atomic_fetch_add_explicit(&value, 4, memory_order_acq_rel) != 3)
        return 2;

    int32_t expected = 7;
    if (!atomic_compare_exchange_strong_explicit(&value, &expected, 11, memory_order_acq_rel,
                                                 memory_order_acquire))
        return 3;
    expected = 7;
    if (atomic_compare_exchange_weak_explicit(&value, &expected, 13, memory_order_acq_rel,
                                              memory_order_acquire))
        return 4;
    if (expected != 11)
        return 5;

    atomic_flag flag = ATOMIC_FLAG_INIT;
    if (atomic_flag_test_and_set_explicit(&flag, memory_order_acquire))
        return 6;
    if (!atomic_flag_test_and_set_explicit(&flag, memory_order_acquire))
        return 7;
    atomic_flag_clear_explicit(&flag, memory_order_release);
    if (atomic_flag_test_and_set_explicit(&flag, memory_order_acquire))
        return 8;

    _Atomic(int64_t) floating_bits = INT64_C(0x3ff8000000000000);
    if (xr_atomic_f64_fetch_update_core(&floating_bits, 2.5, 0, 3) != 1.5)
        return 9;
    if (atomic_load(&floating_bits) != INT64_C(0x4010000000000000))
        return 10;
    if (xr_atomic_f64_fetch_update_core(&floating_bits, 2.5, 1, 3) != 4.0)
        return 11;
    if (atomic_load(&floating_bits) != INT64_C(0x3ff8000000000000))
        return 12;
    XrAtomicStorageCore storage;
    xr_atomic_storage_init_core(&storage, INT64_MAX);
    if (!xr_atomic_storage_retain_core(&storage))
        return 13;
    if (xr_atomic_i64_fetch_add_core(&storage.value, 1, 4) != INT64_MAX ||
        xr_atomic_i64_load_core(&storage.value, 4) != INT64_MIN)
        return 14;
    int64_t compare = 0;
    if (xr_atomic_i64_compare_exchange_core(&storage.value, &compare, 7, 4) || compare != INT64_MIN)
        return 15;
    if (xr_atomic_storage_release_core(&storage) != XR_ATOMIC_STORAGE_RELEASE_RETAINED ||
        xr_atomic_storage_release_core(&storage) != XR_ATOMIC_STORAGE_RELEASE_LAST)
        return 16;
    xr_sync_core_fence(4);
    return 0;
}
