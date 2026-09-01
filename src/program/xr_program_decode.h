/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_decode.h - Bounded structural XrProgram decoder
 */

#ifndef XR_PROGRAM_DECODE_H
#define XR_PROGRAM_DECODE_H

#include "xr_program.h"
#include "xr_program_schema_gen.h"

typedef struct XrProgramDecodeBudget {
    size_t max_bytes;
    uint64_t max_records;
    uint64_t max_operations;
} XrProgramDecodeBudget;

typedef struct XrProgramSectionView {
    uint16_t stable_id;
    uint64_t offset;
    uint64_t size;
} XrProgramSectionView;

/* The artifact remains borrowed. All durable references inside this view are
 * byte offsets and counts; no decoded row stores a host pointer. */
typedef struct XrProgramView {
    const uint8_t *artifact;
    size_t artifact_size;
    uint16_t format_major;
    uint16_t format_minor;
    uint32_t core_spec_epoch;
    uint8_t core_spec_fingerprint[XR_PROGRAM_DIGEST_SIZE];
    uint8_t semantic_profile_fingerprint[XR_PROGRAM_DIGEST_SIZE];
    uint64_t required_feature_count;
    uint64_t payload_offset;
    uint32_t section_count;
    XrProgramSectionView sections[XR_PROGRAM_SECTION_COUNT];
    XrProgramId id;
} XrProgramView;

typedef enum XrProgramDecodeStatus {
    XR_PROGRAM_DECODE_OK = 0,
    XR_PROGRAM_DECODE_INVALID_INPUT,
    XR_PROGRAM_DECODE_TRUNCATED,
    XR_PROGRAM_DECODE_BAD_MAGIC,
    XR_PROGRAM_DECODE_UNSUPPORTED_VERSION,
    XR_PROGRAM_DECODE_UNSUPPORTED_FEATURE,
    XR_PROGRAM_DECODE_NONCANONICAL,
    XR_PROGRAM_DECODE_RESOURCE_LIMIT,
    XR_PROGRAM_DECODE_OUT_OF_MEMORY,
    XR_PROGRAM_DECODE_INVALID_SECTION,
    XR_PROGRAM_DECODE_UNSUPPORTED_SECTION_CONTENT,
} XrProgramDecodeStatus;

XR_FUNC XrProgramDecodeBudget xr_program_decode_default_budget(void);
XR_FUNC XrProgramDecodeStatus xr_program_decode_structure(const uint8_t *bytes, size_t size,
                                                          const XrProgramDecodeBudget *budget,
                                                          XrProgramView *view_out, char *diagnostic,
                                                          size_t diagnostic_size);
XR_FUNC XrProgramDecodeStatus xr_program_reencode(const XrProgramView *view,
                                                  XrProgramArtifact *artifact_out, char *diagnostic,
                                                  size_t diagnostic_size);
XR_FUNC const char *xr_program_decode_status_name(XrProgramDecodeStatus status);

#endif /* XR_PROGRAM_DECODE_H */
