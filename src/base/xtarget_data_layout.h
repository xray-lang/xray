/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtarget_data_layout.h - Backend-neutral target ABI data layout
 */

#ifndef XTARGET_DATA_LAYOUT_H
#define XTARGET_DATA_LAYOUT_H

#include "xdefs.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum XrTargetEndian {
    XR_TARGET_ENDIAN_LITTLE = 1,
    XR_TARGET_ENDIAN_BIG = 2,
} XrTargetEndian;

typedef struct XrTargetTypeLayout {
    uint32_t size;
    uint32_t align;
} XrTargetTypeLayout;

/* One immutable semantic ABI fact shared by analyzer, Xi, VM bytecode and
 * AOT.  stable_hash is derived from every field below; it is never a host
 * pointer identity and is safe to persist in layout/cache metadata. */
typedef struct XrTargetDataLayout {
    XrTargetTypeLayout i8;
    XrTargetTypeLayout u8;
    XrTargetTypeLayout i16;
    XrTargetTypeLayout u16;
    XrTargetTypeLayout i32;
    XrTargetTypeLayout u32;
    XrTargetTypeLayout i64;
    XrTargetTypeLayout u64;
    XrTargetTypeLayout f32;
    XrTargetTypeLayout f64;
    XrTargetTypeLayout boolean;
    XrTargetTypeLayout pointer;
    XrTargetTypeLayout isize;
    XrTargetTypeLayout usize;
    XrTargetTypeLayout xr_value;
    XrTargetEndian endian;
    uint32_t abi_id;
    uint64_t stable_hash;
} XrTargetDataLayout;

XR_FUNC bool xr_target_data_layout_init(XrTargetDataLayout *out_layout, uint32_t pointer_size,
                                        XrTargetEndian endian);
XR_FUNC bool xr_target_data_layout_init_native(XrTargetDataLayout *out_layout);
XR_FUNC bool xr_target_data_layout_init_ilp32(XrTargetDataLayout *out_layout);
XR_FUNC bool xr_target_data_layout_init_lp64(XrTargetDataLayout *out_layout);
XR_FUNC bool xr_target_data_layout_validate(const XrTargetDataLayout *layout);
XR_FUNC uint64_t xr_target_data_layout_hash(const XrTargetDataLayout *layout);
XR_FUNC const XrTargetDataLayout *xr_target_data_layout_host(void);

#endif /* XTARGET_DATA_LAYOUT_H */
