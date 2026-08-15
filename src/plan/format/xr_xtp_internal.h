/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xtp_internal.h - Internal typed artifact codec storage
 */

#ifndef XR_XTP_INTERNAL_H
#define XR_XTP_INTERNAL_H

#include "xr_xtp_schema.h"
#include "xr_xtp_instruction_stream.h"
#include "../target/xr_target_profile_internal.h"
#include <stdatomic.h>

#define XR_XTP_TABLE_SECTION_COUNT ((uint32_t) XR_XTP_SECTION_COUNT - 1u)
#define XR_XTP_FULL_DIGEST_OFFSET 328u

typedef struct XrXtpSectionView {
    XrXtpSectionKind kind;
    uint32_t flags;
    size_t offset;
    size_t length;
    uint32_t count;
    uint32_t row_size;
} XrXtpSectionView;

struct XrXtpCandidate {
    atomic_uint_least32_t references;
    uint8_t *bytes;
    size_t size;
    XrXtpIdentity identity;
    XrXtpResourceManifest resources;
    XrXtpSectionView sections[XR_XTP_TABLE_SECTION_COUNT];
};

XR_FUNC void xr_xtp_set_error(char *error, size_t error_size,
                              const char *code, const char *detail);
XR_FUNC bool xr_xtp_fingerprint_is_zero(XrFingerprint fingerprint);
XR_FUNC uint16_t xr_xtp_take_u16(const uint8_t *bytes);
XR_FUNC uint32_t xr_xtp_take_u32(const uint8_t *bytes);
XR_FUNC uint64_t xr_xtp_take_u64(const uint8_t *bytes);
XR_FUNC void xr_xtp_put_u16(uint8_t *bytes, uint16_t value);
XR_FUNC void xr_xtp_put_u32(uint8_t *bytes, uint32_t value);
XR_FUNC void xr_xtp_put_u64(uint8_t *bytes, uint64_t value);
XR_FUNC uint32_t xr_xtp_wire_row_size(XrXtpSectionKind kind);
XR_FUNC uint64_t xr_xtp_table_count_limit(XrXtpSectionKind kind);
XR_FUNC bool xr_xtp_encode_rows(XrXtpSectionKind kind, const void *rows, uint32_t count,
                                uint8_t *bytes);
XR_FUNC bool xr_xtp_decode_rows(XrXtpSectionKind kind, const uint8_t *bytes, uint32_t count,
                                void *rows);
XR_FUNC const XrXtpSectionView *xr_xtp_candidate_section(const XrXtpCandidate *candidate,
                                                        XrXtpSectionKind kind);
XR_FUNC bool xr_xtp_runtime_peak_within_budget(size_t artifact_bytes,
                                               size_t decoded_bytes);
XR_FUNC bool xr_xtp_materialize_target_plan(const XrXtpCandidate *candidate,
                                            const XrSemanticPlan *semantic_plan,
                                            const XrTargetProfile *expected_profile,
                                            XrTargetPlan **plan, char *error,
                                            size_t error_size);
XR_FUNC bool xr_xtp_materialize_target_plan_module_set(
    const XrXtpCandidate *candidate, const XrSemanticPlan *semantic_plan,
    const XrSemanticPlan *const *dependencies, uint32_t dependency_count,
    const XrTargetProfile *expected_profile, XrTargetPlan **plan, char *error,
    size_t error_size);

#endif  // XR_XTP_INTERNAL_H
