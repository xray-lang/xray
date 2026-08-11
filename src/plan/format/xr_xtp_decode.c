/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xtp_decode.c - Bounded immutable typed artifact decoder
 *
 * KEY CONCEPT:
 *   This decoder validates exact container facts and owns a byte snapshot. It
 *   does not construct a TargetPlan or invoke any runtime/provider behavior.
 */

#include "xr_xtp_internal.h"
#include "../../base/xmalloc.h"
#include "../../base/xsha256.h"
#include <limits.h>
#include <string.h>

#define XR_XTP_DIRECTORY_KIND_OFFSET 0u
#define XR_XTP_DIRECTORY_FLAGS_OFFSET 4u
#define XR_XTP_DIRECTORY_SECTION_OFFSET 8u
#define XR_XTP_DIRECTORY_LENGTH_OFFSET 16u
#define XR_XTP_DIRECTORY_COUNT_OFFSET 24u
#define XR_XTP_DIRECTORY_ROW_SIZE_OFFSET 32u
#define XR_XTP_DIRECTORY_ALIGNMENT_OFFSET 36u
#define XR_XTP_DIRECTORY_DIGEST_OFFSET 40u

static bool bytes_are_zero(const uint8_t *bytes, size_t size) {
    for (size_t i = 0; i < size; i++)
        if (bytes[i] != 0)
            return false;
    return true;
}

static bool checked_align(size_t value, size_t alignment, size_t *out) {
    size_t mask = alignment - 1u;
    if ((alignment & mask) != 0 || value > SIZE_MAX - mask)
        return false;
    *out = (value + mask) & ~mask;
    return true;
}

static bool full_digest_matches(const uint8_t *bytes, size_t size) {
    static const uint8_t zero[XR_FINGERPRINT_BYTES] = {0};
    uint8_t digest[XR_FINGERPRINT_BYTES];
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, bytes, XR_XTP_FULL_DIGEST_OFFSET);
    xr_sha256_update(&context, zero, sizeof(zero));
    xr_sha256_update(&context, bytes + XR_XTP_FULL_DIGEST_OFFSET + sizeof(zero),
                     size - XR_XTP_FULL_DIGEST_OFFSET - sizeof(zero));
    xr_sha256_final(&context, digest);
    return memcmp(bytes + XR_XTP_FULL_DIGEST_OFFSET, digest, sizeof(digest)) == 0;
}

static void take_fingerprint(const uint8_t *bytes, XrFingerprint *fingerprint) {
    memcpy(fingerprint->bytes, bytes, XR_FINGERPRINT_BYTES);
}

static bool parse_identity(const uint8_t *bytes, XrXtpIdentity *identity) {
    identity->completed_family_mask = xr_xtp_take_u64(bytes + 48);
    identity->semantic_schema = xr_xtp_take_u32(bytes + 56);
    identity->profile_schema = xr_xtp_take_u32(bytes + 60);
    identity->plan_schema = xr_xtp_take_u32(bytes + 64);
    take_fingerprint(bytes + 72, &identity->semantic_fingerprint);
    take_fingerprint(bytes + 104, &identity->operation_registry_fingerprint);
    take_fingerprint(bytes + 136, &identity->profile_fingerprint);
    take_fingerprint(bytes + 168, &identity->plan_fingerprint);
    take_fingerprint(bytes + 200, &identity->runtime_fingerprint);
    take_fingerprint(bytes + 232, &identity->provider_fingerprint);
    take_fingerprint(bytes + 264, &identity->object_fingerprint);
    return identity->completed_family_mask == XR_TARGET_REQUIRED_FAMILIES &&
           identity->semantic_schema == XR_SEMANTIC_SCHEMA_VERSION &&
           identity->profile_schema == XR_TARGET_PROFILE_SCHEMA_VERSION &&
           identity->plan_schema == XR_TARGET_PLAN_SCHEMA_VERSION &&
           !xr_xtp_fingerprint_is_zero(identity->semantic_fingerprint) &&
           !xr_xtp_fingerprint_is_zero(identity->operation_registry_fingerprint) &&
           !xr_xtp_fingerprint_is_zero(identity->profile_fingerprint) &&
           !xr_xtp_fingerprint_is_zero(identity->plan_fingerprint) &&
           !xr_xtp_fingerprint_is_zero(identity->runtime_fingerprint) &&
           !xr_xtp_fingerprint_is_zero(identity->provider_fingerprint) &&
           !xr_xtp_fingerprint_is_zero(identity->object_fingerprint);
}

static bool parse_header(const uint8_t *bytes, size_t size, XrXtpIdentity *identity,
                         XrXtpResourceManifest *resources, size_t *directory_length,
                         char *error, size_t error_size) {
    static const uint8_t magic[4] = {'X', 'T', 'P', 'F'};
    if (!bytes || size < XR_XTP_HEADER_SIZE || size > XR_XTP_MAX_ARTIFACT_SIZE) {
        xr_xtp_set_error(error, error_size, "artifact byte budget is invalid");
        return false;
    }
    uint32_t schema = xr_xtp_take_u32(bytes + 4);
    uint32_t header_size = xr_xtp_take_u32(bytes + 8);
    uint32_t directory_entry_size = xr_xtp_take_u32(bytes + 12);
    uint32_t section_count = xr_xtp_take_u32(bytes + 16);
    uint64_t artifact_size = xr_xtp_take_u64(bytes + 24);
    uint64_t directory_offset = xr_xtp_take_u64(bytes + 32);
    uint64_t raw_directory_length = xr_xtp_take_u64(bytes + 40);
    uint64_t exact_directory_length =
        (uint64_t) XR_XTP_TABLE_SECTION_COUNT * XR_XTP_DIRECTORY_ENTRY_SIZE;
    if (memcmp(bytes, magic, sizeof(magic)) != 0 || schema != XR_XTP_SCHEMA_VERSION ||
        header_size != XR_XTP_HEADER_SIZE ||
        directory_entry_size != XR_XTP_DIRECTORY_ENTRY_SIZE ||
        section_count != XR_XTP_TABLE_SECTION_COUNT || artifact_size != size ||
        directory_offset != XR_XTP_HEADER_SIZE ||
        raw_directory_length != exact_directory_length ||
        raw_directory_length > size - XR_XTP_HEADER_SIZE || !bytes_are_zero(bytes + 20, 4) ||
        !bytes_are_zero(bytes + 68, 4) || !bytes_are_zero(bytes + 360, 88)) {
        xr_xtp_set_error(error, error_size, "exact schema or header contract is invalid");
        return false;
    }
    if (!parse_identity(bytes, identity)) {
        xr_xtp_set_error(error, error_size, "artifact identity or family coverage is invalid");
        return false;
    }
    resources->total_rows = xr_xtp_take_u64(bytes + 296);
    resources->table_bytes = xr_xtp_take_u64(bytes + 304);
    resources->total_frame_bytes = xr_xtp_take_u64(bytes + 312);
    resources->verification_work_units = xr_xtp_take_u64(bytes + 320);
    if (resources->total_rows > XR_XTP_MAX_TOTAL_ROWS ||
        resources->table_bytes > XR_XTP_MAX_TABLE_BYTES ||
        resources->total_frame_bytes > XR_XTP_MAX_TOTAL_FRAME_BYTES ||
        resources->verification_work_units > XR_XTP_MAX_VERIFY_WORK_UNITS ||
        resources->verification_work_units != resources->total_rows) {
        xr_xtp_set_error(error, error_size, "resource manifest exceeds its hard budget");
        return false;
    }
    if (!full_digest_matches(bytes, size)) {
        xr_xtp_set_error(error, error_size, "complete artifact digest is invalid");
        return false;
    }
    *directory_length = (size_t) raw_directory_length;
    return true;
}

static bool parse_directory(const uint8_t *bytes, size_t size, size_t directory_length,
                            const XrXtpResourceManifest *resources, XrXtpSectionView views[],
                            char *error, size_t error_size) {
    size_t expected_offset = 0;
    if (!checked_align(XR_XTP_HEADER_SIZE + directory_length, XR_XTP_SECTION_ALIGNMENT,
                       &expected_offset) || expected_offset > size ||
        !bytes_are_zero(bytes + XR_XTP_HEADER_SIZE + directory_length,
                        expected_offset - XR_XTP_HEADER_SIZE - directory_length)) {
        xr_xtp_set_error(error, error_size, "directory padding is not canonical");
        return false;
    }
    uint64_t total_rows = 0;
    uint64_t table_bytes = 0;
    for (uint32_t i = 0; i < XR_XTP_TABLE_SECTION_COUNT; i++) {
        const uint8_t *entry = bytes + XR_XTP_HEADER_SIZE +
                               (size_t) i * XR_XTP_DIRECTORY_ENTRY_SIZE;
        XrXtpSectionKind kind = (XrXtpSectionKind) xr_xtp_take_u32(
            entry + XR_XTP_DIRECTORY_KIND_OFFSET);
        uint64_t raw_offset = xr_xtp_take_u64(entry + XR_XTP_DIRECTORY_SECTION_OFFSET);
        uint64_t raw_length = xr_xtp_take_u64(entry + XR_XTP_DIRECTORY_LENGTH_OFFSET);
        uint64_t raw_count = xr_xtp_take_u64(entry + XR_XTP_DIRECTORY_COUNT_OFFSET);
        uint32_t row_size = xr_xtp_take_u32(entry + XR_XTP_DIRECTORY_ROW_SIZE_OFFSET);
        uint32_t alignment = xr_xtp_take_u32(entry + XR_XTP_DIRECTORY_ALIGNMENT_OFFSET);
        XrXtpSectionKind expected_kind = (XrXtpSectionKind) (i + 1u);
        uint32_t expected_row_size = xr_xtp_wire_row_size(expected_kind);
        if (kind != expected_kind || xr_xtp_take_u32(entry + XR_XTP_DIRECTORY_FLAGS_OFFSET) != 0 ||
            row_size != expected_row_size || alignment != XR_XTP_SECTION_ALIGNMENT ||
            !bytes_are_zero(entry + 72, 8) || raw_count > xr_xtp_table_count_limit(kind) ||
            raw_count > UINT32_MAX ||
            (kind == XR_XTP_SECTION_TARGET_PROFILE && raw_count != 1) ||
            (row_size && raw_count > UINT64_MAX / row_size) ||
            raw_length != raw_count * row_size || raw_length > XR_XTP_MAX_TABLE_BYTES ||
            raw_offset != expected_offset || raw_offset > size || raw_length > size - raw_offset) {
            xr_xtp_set_error(error, error_size, "typed section directory is invalid");
            return false;
        }
        uint8_t digest[XR_FINGERPRINT_BYTES];
        xr_sha256(bytes + expected_offset, (size_t) raw_length, digest);
        if (memcmp(entry + XR_XTP_DIRECTORY_DIGEST_OFFSET, digest, sizeof(digest)) != 0) {
            xr_xtp_set_error(error, error_size, "typed section digest is invalid");
            return false;
        }
        views[i] = (XrXtpSectionView) {
            .kind = kind,
            .offset = expected_offset,
            .length = (size_t) raw_length,
            .count = (uint32_t) raw_count,
            .row_size = row_size,
        };
        total_rows += raw_count;
        table_bytes += raw_length;
        size_t section_end = expected_offset + (size_t) raw_length;
        if (!checked_align(section_end, XR_XTP_SECTION_ALIGNMENT, &expected_offset) ||
            expected_offset > size ||
            !bytes_are_zero(bytes + section_end, expected_offset - section_end)) {
            xr_xtp_set_error(error, error_size, "section padding is not canonical");
            return false;
        }
    }
    if (expected_offset != size || total_rows != resources->total_rows ||
        table_bytes != resources->table_bytes) {
        xr_xtp_set_error(error, error_size, "resource manifest does not match typed tables");
        return false;
    }
    return true;
}

XR_FUNC bool xr_xtp_decode_candidate(const uint8_t *bytes, size_t size,
                                     XrXtpCandidate **candidate, char *error,
                                     size_t error_size) {
    if (candidate)
        *candidate = NULL;
    if (!candidate) {
        xr_xtp_set_error(error, error_size, "candidate output is required");
        return false;
    }
    XrXtpIdentity identity = {0};
    XrXtpResourceManifest resources = {0};
    size_t directory_length = 0;
    if (!parse_header(bytes, size, &identity, &resources, &directory_length, error, error_size))
        return false;
    XrXtpSectionView views[XR_XTP_TABLE_SECTION_COUNT] = {0};
    if (!parse_directory(bytes, size, directory_length, &resources, views, error, error_size))
        return false;
    XrXtpCandidate *decoded = (XrXtpCandidate *) xr_calloc(1, sizeof(*decoded));
    if (!decoded) {
        xr_xtp_set_error(error, error_size, "candidate allocation failed");
        return false;
    }
    atomic_init(&decoded->references, 1);
    decoded->bytes = (uint8_t *) xr_malloc(size);
    if (!decoded->bytes) {
        xr_xtp_candidate_release(decoded);
        xr_xtp_set_error(error, error_size, "candidate byte snapshot allocation failed");
        return false;
    }
    memcpy(decoded->bytes, bytes, size);
    decoded->size = size;
    decoded->identity = identity;
    decoded->resources = resources;
    memcpy(decoded->sections, views, sizeof(views));
    *candidate = decoded;
    return true;
}
