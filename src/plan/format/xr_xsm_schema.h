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

#define XR_XSM_HEADER_SIZE 152u
#define XR_XSM_MAX_ARTIFACT_SIZE ((size_t) 256u * 1024u * 1024u)
#define XR_XSM_MAX_PAYLOAD_SIZE (XR_XSM_MAX_ARTIFACT_SIZE - XR_XSM_HEADER_SIZE)
#define XR_XSM_MAX_TABLE_STORAGE ((size_t) 192u * 1024u * 1024u)
#define XR_XSM_MAX_STRING_SIZE ((size_t) 1024u * 1024u)
#define XR_XSM_MAX_STRING_STORAGE ((size_t) 64u * 1024u * 1024u)
#define XR_XSM_MAX_DECODE_STORAGE ((size_t) 256u * 1024u * 1024u)

XR_FUNC bool xr_xsm_encode(const XrSemanticPlan *plan, uint8_t **bytes, size_t *size, char *error,
                           size_t error_size);
XR_FUNC bool xr_xsm_decode(const uint8_t *bytes, size_t size, XrSemanticPlan **plan, char *error,
                           size_t error_size);

#endif  // XR_XSM_SCHEMA_H
