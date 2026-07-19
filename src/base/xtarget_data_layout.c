/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtarget_data_layout.c - Backend-neutral target ABI data layout
 */

#include "xtarget_data_layout.h"

#include <stddef.h>
#include <stdatomic.h>
#include <string.h>

static XrTargetTypeLayout type_layout(uint32_t size, uint32_t align) {
    XrTargetTypeLayout layout = {.size = size, .align = align};
    return layout;
}

static bool type_layout_valid(const XrTargetTypeLayout *layout, uint32_t required_size) {
    return layout && layout->size == required_size && layout->align != 0 &&
           (layout->align & (layout->align - 1u)) == 0 && layout->align <= layout->size &&
           layout->size % layout->align == 0;
}

static uint64_t hash_word(uint64_t hash, uint64_t word) {
    for (uint32_t i = 0; i < 8; i++) {
        hash ^= (uint8_t) (word >> (i * 8u));
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

XR_FUNC uint64_t xr_target_data_layout_hash(const XrTargetDataLayout *layout) {
    if (!layout)
        return 0;
    const XrTargetTypeLayout *types[] = {
        &layout->i8,      &layout->u8,      &layout->i16,   &layout->u16,   &layout->i32,
        &layout->u32,     &layout->i64,     &layout->u64,   &layout->f32,   &layout->f64,
        &layout->boolean, &layout->pointer, &layout->isize, &layout->usize, &layout->xr_value,
    };
    uint64_t hash = UINT64_C(1469598103934665603);
    for (uint32_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        hash = hash_word(hash, types[i]->size);
        hash = hash_word(hash, types[i]->align);
    }
    hash = hash_word(hash, layout->endian);
    hash = hash_word(hash, layout->abi_id);
    return hash ? hash : UINT64_C(1);
}

XR_FUNC bool xr_target_data_layout_validate(const XrTargetDataLayout *layout) {
    if (!layout || !type_layout_valid(&layout->i8, 1) || !type_layout_valid(&layout->u8, 1) ||
        !type_layout_valid(&layout->i16, 2) || !type_layout_valid(&layout->u16, 2) ||
        !type_layout_valid(&layout->i32, 4) || !type_layout_valid(&layout->u32, 4) ||
        !type_layout_valid(&layout->i64, 8) || !type_layout_valid(&layout->u64, 8) ||
        !type_layout_valid(&layout->f32, 4) || !type_layout_valid(&layout->f64, 8) ||
        !type_layout_valid(&layout->boolean, 1) ||
        (layout->pointer.size != 4 && layout->pointer.size != 8) ||
        !type_layout_valid(&layout->pointer, layout->pointer.size) ||
        !type_layout_valid(&layout->isize, layout->pointer.size) ||
        !type_layout_valid(&layout->usize, layout->pointer.size) ||
        !type_layout_valid(&layout->xr_value, 16) ||
        (layout->endian != XR_TARGET_ENDIAN_LITTLE && layout->endian != XR_TARGET_ENDIAN_BIG) ||
        layout->abi_id == 0)
        return false;
    uint32_t value_align = layout->i64.align;
    if (layout->f64.align > value_align)
        value_align = layout->f64.align;
    if (layout->pointer.align > value_align)
        value_align = layout->pointer.align;
    return layout->xr_value.align == value_align && layout->stable_hash != 0 &&
           layout->stable_hash == xr_target_data_layout_hash(layout);
}

XR_FUNC bool xr_target_data_layout_init(XrTargetDataLayout *out_layout, uint32_t pointer_size,
                                        XrTargetEndian endian) {
    if (!out_layout || (pointer_size != 4 && pointer_size != 8) ||
        (endian != XR_TARGET_ENDIAN_LITTLE && endian != XR_TARGET_ENDIAN_BIG))
        return false;
    XrTargetDataLayout layout;
    memset(&layout, 0, sizeof(layout));
    layout.i8 = layout.u8 = layout.boolean = type_layout(1, 1);
    layout.i16 = layout.u16 = type_layout(2, 2);
    layout.i32 = layout.u32 = layout.f32 = type_layout(4, 4);
    layout.i64 = layout.u64 = layout.f64 = type_layout(8, 8);
    layout.pointer = layout.isize = layout.usize = type_layout(pointer_size, pointer_size);
    layout.xr_value = type_layout(16, 8);
    layout.endian = endian;
    layout.abi_id = (pointer_size == 4 ? 0x20u : 0x40u) | (uint32_t) endian;
    layout.stable_hash = xr_target_data_layout_hash(&layout);
    if (!xr_target_data_layout_validate(&layout))
        return false;
    *out_layout = layout;
    return true;
}

XR_FUNC bool xr_target_data_layout_init_ilp32(XrTargetDataLayout *out_layout) {
    return xr_target_data_layout_init(out_layout, 4, XR_TARGET_ENDIAN_LITTLE);
}

XR_FUNC bool xr_target_data_layout_init_lp64(XrTargetDataLayout *out_layout) {
    return xr_target_data_layout_init(out_layout, 8, XR_TARGET_ENDIAN_LITTLE);
}

XR_FUNC bool xr_target_data_layout_init_native(XrTargetDataLayout *out_layout) {
    const uint16_t marker = 1;
    XrTargetEndian endian =
        *(const uint8_t *) &marker ? XR_TARGET_ENDIAN_LITTLE : XR_TARGET_ENDIAN_BIG;
    XrTargetDataLayout layout;
    memset(&layout, 0, sizeof(layout));
    layout.i8 = layout.u8 = layout.boolean = type_layout(1, 1);
    layout.i16 = layout.u16 = type_layout(2, (uint32_t) _Alignof(int16_t));
    layout.i32 = layout.u32 = type_layout(4, (uint32_t) _Alignof(int32_t));
    layout.i64 = layout.u64 = type_layout(8, (uint32_t) _Alignof(int64_t));
    layout.f32 = type_layout(4, (uint32_t) _Alignof(float));
    layout.f64 = type_layout(8, (uint32_t) _Alignof(double));
    layout.pointer = type_layout((uint32_t) sizeof(void *), (uint32_t) _Alignof(void *));
    layout.isize = type_layout((uint32_t) sizeof(ptrdiff_t), (uint32_t) _Alignof(ptrdiff_t));
    layout.usize = type_layout((uint32_t) sizeof(size_t), (uint32_t) _Alignof(size_t));
    uint32_t value_align = layout.i64.align;
    if (layout.f64.align > value_align)
        value_align = layout.f64.align;
    if (layout.pointer.align > value_align)
        value_align = layout.pointer.align;
    layout.xr_value = type_layout(16, value_align);
    layout.endian = endian;
    layout.abi_id = (layout.pointer.size == 4 ? 0x20u : 0x40u) | (uint32_t) endian;
    layout.stable_hash = xr_target_data_layout_hash(&layout);
    if (!xr_target_data_layout_validate(&layout))
        return false;
    *out_layout = layout;
    return true;
}

XR_FUNC const XrTargetDataLayout *xr_target_data_layout_host(void) {
    static XrTargetDataLayout layout;
    /* 0 = uninitialized, 1 = one thread is initializing, 2 = ready,
     * 3 = initialization failed permanently. */
    static atomic_uint state;
    unsigned current = atomic_load_explicit(&state, memory_order_acquire);
    if (current == 0) {
        unsigned expected = 0;
        if (atomic_compare_exchange_strong_explicit(&state, &expected, 1, memory_order_acq_rel,
                                                    memory_order_acquire)) {
            bool ok = xr_target_data_layout_init_native(&layout);
            atomic_store_explicit(&state, ok ? 2u : 3u, memory_order_release);
            return ok ? &layout : NULL;
        }
        current = expected;
    }
    while (current == 1)
        current = atomic_load_explicit(&state, memory_order_acquire);
    return current == 2 ? &layout : NULL;
}
