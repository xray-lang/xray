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
#define XR_XSM_MAX_PROGRAM_MODULE_SET_ENCODED_STORAGE XR_XSM_MAX_ARTIFACT_SIZE
#define XR_XSM_MAX_PROGRAM_MODULE_SET_RETAINED_STORAGE XR_XSM_MAX_DECODE_STORAGE

XR_FUNC bool xr_xsm_encode(const XrSemanticPlan *plan, uint8_t **bytes, size_t *size, char *error,
                           size_t error_size);
XR_FUNC bool xr_xsm_decode(const uint8_t *bytes, size_t size, XrSemanticPlan **plan, char *error,
                           size_t error_size);
XR_FUNC bool xr_xsm_decode_module_set(
    const uint8_t *bytes, size_t size, const XrSemanticPlan *const *dependencies,
    uint32_t dependency_count, XrSemanticPlan **plan, char *error, size_t error_size);
XR_FUNC bool xr_xsm_decode_program_module_set(
    const uint8_t *const *artifact_bytes, const size_t *artifact_sizes,
    uint32_t artifact_count, XrSemanticPlan ***semantic_modules,
    char *error, size_t error_size);
XR_FUNC void xr_xsm_decoded_program_module_set_free(
    XrSemanticPlan **semantic_modules, uint32_t semantic_module_count);

#endif  // XR_XSM_SCHEMA_H
