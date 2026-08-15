/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xtp_encode.c - Canonical typed TargetPlan artifact encoder
 */

#include "xr_xtp_internal.h"
#include "xr_artifact_kind.h"
#include "../target/xr_target_verify.h"
#include "../../base/xmalloc.h"
#include "../../base/xsha256.h"
#include "../../shared/xr_align_guard.h"
#include <string.h>

typedef struct XrXtpSectionInput {
    XrXtpSectionKind kind;
    uint32_t flags;
    const void *rows;
    uint32_t count;
    size_t offset;
    size_t length;
} XrXtpSectionInput;

static bool checked_add(size_t left, size_t right, size_t *out) {
    if (left > SIZE_MAX - right)
        return false;
    *out = left + right;
    return true;
}

static void put_fingerprint(uint8_t *bytes, XrFingerprint fingerprint) {
    memcpy(bytes, fingerprint.bytes, XR_FINGERPRINT_BYTES);
}

static bool fill_section_inputs(const XrTargetPlan *plan, XrXtpSectionInput sections[]) {
    uint32_t count = 0;
    const XrTargetProfileDraft *profile = xr_target_profile_facts(xr_target_plan_profile(plan));
    if (!profile)
        return false;
    sections[0] =
        (XrXtpSectionInput) {XR_XTP_SECTION_TARGET_PROFILE, 0, profile, 1, 0, 0};
#define XR_XTP_FILL_SECTION(index, kind, accessor)                                                 \
    do {                                                                                           \
        const void *rows = xr_target_plan_##accessor(plan, &count);                               \
        sections[index] =                                                                          \
            (XrXtpSectionInput) {XR_XTP_SECTION_##kind, 0, rows, count, 0, 0};                    \
    } while (0)
    XR_XTP_FILL_SECTION(1, MACHINE_REPS, machine_reps);
    XR_XTP_FILL_SECTION(2, VALUE_REPS, value_reps);
    XR_XTP_FILL_SECTION(3, EXTENTS, extents);
    XR_XTP_FILL_SECTION(4, LAYOUTS, layouts);
    XR_XTP_FILL_SECTION(5, FIELDS, fields);
    XR_XTP_FILL_SECTION(6, STORAGE, storage);
    XR_XTP_FILL_SECTION(7, ALLOCATIONS, allocations);
    XR_XTP_FILL_SECTION(8, EXTENT_OPERANDS, extent_operands);
    XR_XTP_FILL_SECTION(9, FUNCTIONS, functions);
    XR_XTP_FILL_SECTION(10, SLOTS, slots);
    XR_XTP_FILL_SECTION(11, INSTRUCTIONS, instructions);
    XR_XTP_FILL_SECTION(12, CALLS, calls);
    XR_XTP_FILL_SECTION(13, CALL_ARGUMENTS, call_arguments);
    XR_XTP_FILL_SECTION(14, ROOT_MAPS, root_maps);
    XR_XTP_FILL_SECTION(15, ROOT_SLOTS, root_slots);
    XR_XTP_FILL_SECTION(16, CLEANUPS, cleanups);
    XR_XTP_FILL_SECTION(17, ADAPTERS, adapters);
    XR_XTP_FILL_SECTION(18, CAPABILITIES, capabilities);
    XR_XTP_FILL_SECTION(19, COROUTINES, coroutines);
    XR_XTP_FILL_SECTION(20, ENTRY_EXPECTATIONS, entry_expectations);
    XR_XTP_FILL_SECTION(21, DEBUG_FACTS, debug_facts);
#undef XR_XTP_FILL_SECTION
    sections[11].flags = XR_XTP_SECTION_FLAG_COMPACT;
    return true;
}

static bool compute_resources(const XrTargetPlan *plan, XrXtpSectionInput sections[],
                              XrXtpResourceManifest *resources, char *error,
                              size_t error_size) {
    memset(resources, 0, sizeof(*resources));
    for (uint32_t i = 0; i < XR_XTP_TABLE_SECTION_COUNT; i++) {
        uint32_t row_size = xr_xtp_wire_row_size(sections[i].kind);
        if (!row_size || sections[i].count > xr_xtp_table_count_limit(sections[i].kind) ||
            (sections[i].count && !sections[i].rows)) {
            xr_xtp_set_error(error, error_size, "XR_ARTIFACT_2003",
                             "TargetPlan table cannot be encoded");
            return false;
        }
        size_t compact_length = 0;
        uint64_t length = (uint64_t) sections[i].count * row_size;
        if (sections[i].kind == XR_XTP_SECTION_INSTRUCTIONS) {
            if (sections[i].flags != XR_XTP_SECTION_FLAG_COMPACT ||
                !xr_xtp_instruction_stream_size(
                    (const XrTargetInstructionRecord *) sections[i].rows,
                    sections[i].count, &compact_length)) {
                xr_xtp_set_error(error, error_size, "XR_ARTIFACT_2003",
                                 "instruction stream cannot be encoded");
                return false;
            }
            length = compact_length;
        } else if (sections[i].flags != 0) {
            xr_xtp_set_error(error, error_size, "XR_ARTIFACT_2003",
                             "typed section flags are invalid");
            return false;
        }
        if (length > SIZE_MAX || resources->table_bytes > UINT64_MAX - length ||
            resources->total_rows > UINT64_MAX - sections[i].count) {
            xr_xtp_set_error(error, error_size, "XR_EXEC_5003",
                             "typed table size overflows");
            return false;
        }
        sections[i].length = (size_t) length;
        resources->table_bytes += length;
        resources->total_rows += sections[i].count;
    }
    uint32_t function_count = 0;
    const XrTargetFunctionRecord *functions = xr_target_plan_functions(plan, &function_count);
    for (uint32_t i = 0; i < function_count; i++) {
        if (resources->total_frame_bytes > UINT64_MAX - functions[i].frame_size) {
            xr_xtp_set_error(error, error_size, "XR_EXEC_5003",
                             "frame byte manifest overflows");
            return false;
        }
        resources->total_frame_bytes += functions[i].frame_size;
    }
    const XrXtpSectionInput *instructions =
        &sections[(uint32_t) XR_XTP_SECTION_INSTRUCTIONS - 1u];
    uint64_t instruction_work =
        (uint64_t) instructions->length + instructions->count;
    if (resources->total_rows > UINT64_MAX - instruction_work) {
        xr_xtp_set_error(error, error_size, "XR_EXEC_5003",
                         "instruction decode work overflows");
        return false;
    }
    resources->verification_work_units = resources->total_rows + instruction_work;
    if (resources->total_rows > XR_XTP_MAX_TOTAL_ROWS ||
        resources->table_bytes > XR_XTP_MAX_TABLE_BYTES ||
        resources->total_frame_bytes > XR_XTP_MAX_TOTAL_FRAME_BYTES ||
        resources->verification_work_units > XR_XTP_MAX_VERIFY_WORK_UNITS) {
        xr_xtp_set_error(error, error_size, "XR_EXEC_5003",
                         "TargetPlan exceeds XTP hard budgets");
        return false;
    }
    return true;
}

static bool layout_sections(XrXtpSectionInput sections[], size_t *artifact_size,
                            char *error, size_t error_size) {
    size_t directory_length =
        (size_t) XR_XTP_TABLE_SECTION_COUNT * XR_XTP_DIRECTORY_ENTRY_SIZE;
    size_t cursor = 0;
    if (!checked_add(XR_XTP_HEADER_SIZE, directory_length, &cursor) ||
        !xr_checked_align_size(cursor, XR_XTP_SECTION_ALIGNMENT, &cursor)) {
        xr_xtp_set_error(error, error_size, "XR_EXEC_5003",
                         "artifact directory size overflows");
        return false;
    }
    for (uint32_t i = 0; i < XR_XTP_TABLE_SECTION_COUNT; i++) {
        sections[i].offset = cursor;
        if (!checked_add(cursor, sections[i].length, &cursor) ||
            !xr_checked_align_size(cursor, XR_XTP_SECTION_ALIGNMENT, &cursor) ||
            cursor > XR_XTP_MAX_ARTIFACT_SIZE) {
            xr_xtp_set_error(error, error_size, "XR_EXEC_5003",
                             "artifact byte budget is exhausted");
            return false;
        }
    }
    *artifact_size = cursor;
    return true;
}

static void write_header(uint8_t *artifact, size_t artifact_size, const XrTargetPlan *plan,
                         const XrXtpResourceManifest *resources) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(plan);
    const XrTargetProfile *profile = xr_target_plan_profile(plan);
    const XrTargetProfileDraft *facts = xr_target_profile_facts(profile);
    memcpy(artifact, xr_xtp_artifact_magic, XR_XTP_ARTIFACT_MAGIC_SIZE);
    xr_xtp_put_u32(artifact + 4, XR_XTP_SCHEMA_VERSION);
    xr_xtp_put_u32(artifact + 8, XR_XTP_HEADER_SIZE);
    xr_xtp_put_u32(artifact + 12, XR_XTP_DIRECTORY_ENTRY_SIZE);
    xr_xtp_put_u32(artifact + 16, XR_XTP_TABLE_SECTION_COUNT);
    xr_xtp_put_u64(artifact + 24, artifact_size);
    xr_xtp_put_u64(artifact + 32, XR_XTP_HEADER_SIZE);
    xr_xtp_put_u64(artifact + 40,
                   (uint64_t) XR_XTP_TABLE_SECTION_COUNT * XR_XTP_DIRECTORY_ENTRY_SIZE);
    xr_xtp_put_u64(artifact + 48, xr_target_plan_completed_family_mask(plan));
    xr_xtp_put_u32(artifact + 56, xr_semantic_plan_schema(semantic));
    xr_xtp_put_u32(artifact + 60, facts->schema_version);
    xr_xtp_put_u32(artifact + 64, xr_target_plan_schema_version(plan));
    put_fingerprint(artifact + 72, xr_semantic_plan_fingerprint(semantic));
    put_fingerprint(artifact + 104,
                    xr_semantic_plan_operation_registry_fingerprint(semantic));
    put_fingerprint(artifact + 136, xr_target_profile_fingerprint(profile));
    put_fingerprint(artifact + 168, xr_target_plan_fingerprint(plan));
    put_fingerprint(artifact + 200, facts->runtime_abi_fingerprint);
    put_fingerprint(artifact + 232, facts->provider_set_fingerprint);
    put_fingerprint(artifact + 264, facts->object_header_fingerprint);
    xr_xtp_put_u64(artifact + 296, resources->total_rows);
    xr_xtp_put_u64(artifact + 304, resources->table_bytes);
    xr_xtp_put_u64(artifact + 312, resources->total_frame_bytes);
    xr_xtp_put_u64(artifact + 320, resources->verification_work_units);
}

static bool write_sections(uint8_t *artifact, const XrXtpSectionInput sections[]) {
    for (uint32_t i = 0; i < XR_XTP_TABLE_SECTION_COUNT; i++) {
        uint8_t *entry = artifact + XR_XTP_HEADER_SIZE +
                         (size_t) i * XR_XTP_DIRECTORY_ENTRY_SIZE;
        xr_xtp_put_u32(entry, sections[i].kind);
        xr_xtp_put_u32(entry + 4, sections[i].flags);
        xr_xtp_put_u64(entry + 8, sections[i].offset);
        xr_xtp_put_u64(entry + 16, sections[i].length);
        xr_xtp_put_u64(entry + 24, sections[i].count);
        xr_xtp_put_u32(entry + 32,
                       sections[i].kind == XR_XTP_SECTION_INSTRUCTIONS
                           ? 0
                           : xr_xtp_wire_row_size(sections[i].kind));
        xr_xtp_put_u32(entry + 36, XR_XTP_SECTION_ALIGNMENT);
        if (sections[i].kind == XR_XTP_SECTION_INSTRUCTIONS) {
            size_t written = 0;
            if (!xr_xtp_instruction_stream_encode(
                    (const XrTargetInstructionRecord *) sections[i].rows,
                    sections[i].count, artifact + sections[i].offset,
                    sections[i].length, &written) ||
                written != sections[i].length)
                return false;
        } else if (!xr_xtp_encode_rows(sections[i].kind, sections[i].rows,
                                       sections[i].count,
                                       artifact + sections[i].offset)) {
            return false;
        }
        xr_sha256(artifact + sections[i].offset, sections[i].length, entry + 40);
    }
    return true;
}

static void write_full_digest(uint8_t *artifact, size_t size) {
    static const uint8_t zero[XR_FINGERPRINT_BYTES] = {0};
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, artifact, XR_XTP_FULL_DIGEST_OFFSET);
    xr_sha256_update(&context, zero, sizeof(zero));
    xr_sha256_update(&context, artifact + XR_XTP_FULL_DIGEST_OFFSET + sizeof(zero),
                     size - XR_XTP_FULL_DIGEST_OFFSET - sizeof(zero));
    xr_sha256_final(&context, artifact + XR_XTP_FULL_DIGEST_OFFSET);
}

XR_FUNC bool xr_xtp_encode_plan(const XrTargetPlan *plan, uint8_t **bytes, size_t *size,
                                char *error, size_t error_size) {
    if (bytes)
        *bytes = NULL;
    if (size)
        *size = 0;
    if (!plan || !bytes || !size || !xr_target_plan_is_verified(plan) ||
        xr_target_plan_schema_version(plan) != XR_TARGET_PLAN_SCHEMA_VERSION ||
        xr_target_plan_completed_family_mask(plan) != XR_TARGET_REQUIRED_FAMILIES) {
        xr_xtp_set_error(error, error_size, "XR_ARTIFACT_2004",
                         "encoder requires a complete verified TargetPlan");
        return false;
    }
    char verify_error[512] = {0};
    if (!xr_target_plan_verify(plan, verify_error, sizeof(verify_error))) {
        xr_xtp_set_error(error, error_size, "XR_TARGET_1000",
                         "TargetPlan failed independent verification");
        return false;
    }
    XrXtpSectionInput sections[XR_XTP_TABLE_SECTION_COUNT] = {0};
    XrXtpResourceManifest resources;
    size_t artifact_size = 0;
    if (!fill_section_inputs(plan, sections) ||
        !compute_resources(plan, sections, &resources, error, error_size) ||
        !layout_sections(sections, &artifact_size, error, error_size))
        return false;
    uint8_t *artifact = (uint8_t *) xr_calloc(artifact_size, 1);
    if (!artifact) {
        xr_xtp_set_error(error, error_size, "XR_EXEC_5003",
                         "artifact allocation failed");
        return false;
    }
    write_header(artifact, artifact_size, plan, &resources);
    if (!write_sections(artifact, sections)) {
        xr_free(artifact);
        xr_xtp_set_error(error, error_size, "XR_ARTIFACT_2003",
                         "typed sections cannot be encoded");
        return false;
    }
    write_full_digest(artifact, artifact_size);
    *bytes = artifact;
    *size = artifact_size;
    return true;
}
