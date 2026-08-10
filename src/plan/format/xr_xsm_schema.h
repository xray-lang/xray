/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xsm_schema.h - Exact-version target-neutral SemanticPlan artifact
 */

#ifndef XR_XSM_SCHEMA_H
#define XR_XSM_SCHEMA_H

#include "../semantic/xr_semantic_plan.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XR_XSM_HEADER_SIZE 88u

XR_FUNC bool xr_xsm_encode(const XrSemanticPlan *plan, uint8_t **bytes, size_t *size, char *error,
                           size_t error_size);
XR_FUNC bool xr_xsm_decode(const uint8_t *bytes, size_t size, XrSemanticPlan **plan, char *error,
                           size_t error_size);

#endif  // XR_XSM_SCHEMA_H
