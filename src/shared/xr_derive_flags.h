/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_derive_flags.h - Shared compile-time derive metadata flags
 */

#ifndef XR_DERIVE_FLAGS_H
#define XR_DERIVE_FLAGS_H

#include <stdint.h>

enum {
    XR_DERIVE_INSPECT = 1u << 0,
    XR_DERIVE_JSON = 1u << 1,
    XR_DERIVE_EQ = 1u << 2,
    XR_DERIVE_HASH = 1u << 3,
};

#define XR_DERIVE_KNOWN_MASK                                                                       \
    ((uint32_t) (XR_DERIVE_INSPECT | XR_DERIVE_JSON | XR_DERIVE_EQ | XR_DERIVE_HASH))

#endif  // XR_DERIVE_FLAGS_H
