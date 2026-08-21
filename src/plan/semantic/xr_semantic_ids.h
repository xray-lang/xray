/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_ids.h - Stable semantic identities and fingerprints
 */

#ifndef XR_SEMANTIC_IDS_H
#define XR_SEMANTIC_IDS_H

#include "../../base/xdefs.h"
#include "../../base/xstable_id.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XR_SEMANTIC_SCHEMA_VERSION UINT32_C(36)

XR_FUNC bool xr_stable_id_from_key(const char *canonical_key, XrStableId *id,
                                   XrFingerprint *key_digest);
XR_FUNC void xr_semantic_fingerprint(const uint8_t *bytes, size_t size, XrFingerprint *out);
XR_FUNC bool xr_stable_id_equal(XrStableId left, XrStableId right);
XR_FUNC bool xr_fingerprint_equal(XrFingerprint left, XrFingerprint right);
XR_FUNC int xr_stable_id_compare(XrStableId left, XrStableId right);
XR_FUNC void xr_stable_id_hex(XrStableId id, char out[XR_STABLE_ID_BYTES * 2 + 1]);
XR_FUNC void xr_fingerprint_hex(XrFingerprint fingerprint, char out[XR_FINGERPRINT_BYTES * 2 + 1]);

#endif  // XR_SEMANTIC_IDS_H
