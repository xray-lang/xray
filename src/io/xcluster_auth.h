/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcluster_auth.h - Allocation-free proof projection for native I/O loops
 */

#ifndef XR_IO_CLUSTER_AUTH_H
#define XR_IO_CLUSTER_AUTH_H

#include "../base/xdefs.h"

#include <stdbool.h>
#include <stdint.h>

XR_FUNC void xr_cluster_auth_compute_proof(const char *secret, const uint8_t *nonce,
                                           uint8_t *proof_out);
XR_FUNC bool xr_cluster_auth_proof_equal(const uint8_t *left, const uint8_t *right);

#endif /* XR_IO_CLUSTER_AUTH_H */
