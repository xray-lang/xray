/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xtp_schema.h - Exact typed TargetPlan artifact contract
 *
 * KEY CONCEPT:
 *   Raw decoding establishes only a bounded immutable candidate. A candidate
 *   becomes executable only after typed materialization freezes and
 *   independently verifies the complete TargetPlan against exact identities.
 */

#ifndef XR_XTP_SCHEMA_H
#define XR_XTP_SCHEMA_H

#include "../target/xr_target_plan.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XR_XTP_SCHEMA_VERSION UINT32_C(20)
#define XR_XTP_HEADER_SIZE UINT32_C(448)
#define XR_XTP_DIRECTORY_ENTRY_SIZE UINT32_C(80)
#define XR_XTP_SECTION_ALIGNMENT UINT32_C(16)

#define XR_XTP_MAX_ARTIFACT_SIZE ((size_t) 256u * 1024u * 1024u)
#define XR_XTP_MAX_TABLE_BYTES ((uint64_t) 192u * 1024u * 1024u)
#define XR_XTP_MAX_DECODED_TABLE_BYTES ((size_t) 256u * 1024u * 1024u)
#define XR_XTP_MAX_RUNTIME_LOAD_PEAK_BYTES ((size_t) 1024u * 1024u * 1024u)
#define XR_XTP_MAX_TOTAL_ROWS UINT64_C(64000000)
#define XR_XTP_MAX_TOTAL_FRAME_BYTES UINT64_C(4294967295)
#define XR_XTP_MAX_VERIFY_WORK_UNITS UINT64_C(64000000)

typedef enum XrXtpSectionKind {
    XR_XTP_SECTION_INVALID = 0,
    XR_XTP_SECTION_TARGET_PROFILE,
    XR_XTP_SECTION_MACHINE_REPS,
    XR_XTP_SECTION_VALUE_REPS,
    XR_XTP_SECTION_EXTENTS,
    XR_XTP_SECTION_LAYOUTS,
    XR_XTP_SECTION_FIELDS,
    XR_XTP_SECTION_STORAGE,
    XR_XTP_SECTION_ALLOCATIONS,
    XR_XTP_SECTION_EXTENT_OPERANDS,
    XR_XTP_SECTION_FUNCTIONS,
    XR_XTP_SECTION_SLOTS,
    XR_XTP_SECTION_INSTRUCTIONS,
    XR_XTP_SECTION_CALLS,
    XR_XTP_SECTION_CALL_ARGUMENTS,
    XR_XTP_SECTION_ROOT_MAPS,
    XR_XTP_SECTION_ROOT_SLOTS,
    XR_XTP_SECTION_CLEANUPS,
    XR_XTP_SECTION_ADAPTERS,
    XR_XTP_SECTION_CAPABILITIES,
    XR_XTP_SECTION_COROUTINES,
    XR_XTP_SECTION_COUNT,
} XrXtpSectionKind;

typedef struct XrXtpIdentity {
    uint32_t semantic_schema;
    uint32_t profile_schema;
    uint32_t plan_schema;
    uint64_t completed_family_mask;
    XrFingerprint semantic_fingerprint;
    XrFingerprint operation_registry_fingerprint;
    XrFingerprint profile_fingerprint;
    XrFingerprint plan_fingerprint;
    XrFingerprint runtime_fingerprint;
    XrFingerprint provider_fingerprint;
    XrFingerprint object_fingerprint;
} XrXtpIdentity;

typedef struct XrXtpResourceManifest {
    uint64_t total_rows;
    uint64_t table_bytes;
    uint64_t total_frame_bytes;
    uint64_t verification_work_units;
} XrXtpResourceManifest;

typedef struct XrXtpCandidate XrXtpCandidate;

XR_FUNC bool xr_xtp_encode_plan(const XrTargetPlan *plan, uint8_t **bytes, size_t *size,
                                char *error, size_t error_size);
XR_FUNC void xr_xtp_encoded_free(uint8_t *bytes);
XR_FUNC bool xr_xtp_decode_candidate(const uint8_t *bytes, size_t size,
                                     XrXtpCandidate **candidate, char *error,
                                     size_t error_size);
XR_FUNC XrXtpCandidate *xr_xtp_candidate_retain(XrXtpCandidate *candidate);
XR_FUNC void xr_xtp_candidate_release(XrXtpCandidate *candidate);
XR_FUNC bool xr_xtp_candidate_identity(const XrXtpCandidate *candidate,
                                       XrXtpIdentity *identity);
XR_FUNC bool xr_xtp_candidate_resources(const XrXtpCandidate *candidate,
                                        XrXtpResourceManifest *resources);

#endif  // XR_XTP_SCHEMA_H
