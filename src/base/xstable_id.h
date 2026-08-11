/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstable_id.h - Durable identity and fingerprint value types
 *
 * KEY CONCEPT:
 *   Stable identities cross compiler, plan, runtime, and audit boundaries as
 *   bytes. They never carry process-local pointers or insertion-order state.
 */

#ifndef XSTABLE_ID_H
#define XSTABLE_ID_H

#include <stdint.h>

#define XR_STABLE_ID_BYTES 16
#define XR_FINGERPRINT_BYTES 32

typedef struct XrStableId {
    uint8_t bytes[XR_STABLE_ID_BYTES];
} XrStableId;

typedef struct XrFingerprint {
    uint8_t bytes[XR_FINGERPRINT_BYTES];
} XrFingerprint;

#endif  // XSTABLE_ID_H
