/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xtp_text.c - Deterministic textual rendering and comparison of artifacts
 *
 * KEY CONCEPT:
 *   Row rendering expands from a reader-side field inventory whose total wire
 *   width is checked against the width the decoder recorded for the table, so
 *   a rendering that has fallen behind the codec stops rather than naming
 *   stale fields. Comparison reads fixed rows directly and expands the compact
 *   instruction stream through the same sequential iterator as materialization,
 *   so it reports an exact location without holding a whole rendering in memory.
 */

#include "xr_xtp_text.h"
#include "xr_xtp_internal.h"
#include "xr_xtp_row_fields.h"
#include "../semantic/xr_semantic_ids.h"
#include <inttypes.h>
#include <string.h>

/* Fields printed before a row continues on a further line. A row never
 * silently drops a field: it wraps, repeating its section and row index. */
#define XR_XTP_TEXT_FIELDS_PER_LINE 8u

/* Identity and resource fields, in the one order both rendering and
 * comparison use. */
#define XR_XTP_TEXT_HEADER_FIELD_COUNT 15u

/* The leading header fields are the ones a comparison must settle before the
 * tables mean anything: schema versions and family coverage. The rest are
 * derived from table content, so a comparison reaches them only after the
 * rows agree. Reporting a content fingerprint first would name a symptom and
 * hide the row that caused it. */
#define XR_XTP_TEXT_STRUCTURAL_FIELD_COUNT 4u

/* ========== Names ========== */

static const char *section_name(XrXtpSectionKind kind) {
    switch (kind) {
#define XR_XTP_TEXT_NAME_CASE(name, type, fields)                                                  \
    case XR_XTP_SECTION_##name:                                                                    \
        return #name;
        XR_XTP_TEXT_TYPED_ROWS(XR_XTP_TEXT_NAME_CASE)
#undef XR_XTP_TEXT_NAME_CASE
        case XR_XTP_SECTION_ROOT_SLOTS:
            return "ROOT_SLOTS";
        default:
            return "INVALID";
    }
}

static const char *opcode_name(uint16_t opcode) {
    const char *name = xr_target_instruction_opcode_name(opcode);
    return name ? name : "UNKNOWN";
}

typedef struct XrXtpTextFamilyName {
    uint64_t bit;
    const char *name;
} XrXtpTextFamilyName;

static const XrXtpTextFamilyName family_names[] = {
    {XR_TARGET_FAMILY_SCALAR, "SCALAR"},
    {XR_TARGET_FAMILY_AGGREGATE, "AGGREGATE"},
    {XR_TARGET_FAMILY_CALL_ADAPTER, "CALL_ADAPTER"},
    {XR_TARGET_FAMILY_CLOSURE_STORAGE, "CLOSURE_STORAGE"},
    {XR_TARGET_FAMILY_COROUTINE_STATE_CALL, "COROUTINE_STATE_CALL"},
    {XR_TARGET_FAMILY_STRING_LITERAL_STORAGE, "STRING_LITERAL_STORAGE"},
    {XR_TARGET_FAMILY_BIGINT_VALUE_STORAGE, "BIGINT_VALUE_STORAGE"},
    {XR_TARGET_FAMILY_DIRECT_LOCAL_CALLEE_STORAGE, "DIRECT_LOCAL_CALLEE_STORAGE"},
    {XR_TARGET_FAMILY_CHANNEL_ALLOCATION_STORAGE, "CHANNEL_ALLOCATION_STORAGE"},
    {XR_TARGET_FAMILY_CHANNEL_RECEIVE_STORAGE, "CHANNEL_RECEIVE_STORAGE"},
    {XR_TARGET_FAMILY_DIRECT_LOCAL_GO_CALLEE_STORAGE, "DIRECT_LOCAL_GO_CALLEE_STORAGE"},
    {XR_TARGET_FAMILY_SOURCE_IMPORT_STORAGE, "SOURCE_IMPORT_STORAGE"},
    {XR_TARGET_FAMILY_STRING_BYTE_SLICE_VIEW_STORAGE, "STRING_BYTE_SLICE_VIEW_STORAGE"},
    {XR_TARGET_FAMILY_DIRECT_LOCAL_UNIT_ENUM_ARGUMENT_STORAGE,
     "DIRECT_LOCAL_UNIT_ENUM_ARGUMENT_STORAGE"},
    {XR_TARGET_FAMILY_STRINGBUILDER_APPEND_RUNE_STORAGE, "STRINGBUILDER_APPEND_RUNE_STORAGE"},
    {XR_TARGET_FAMILY_STRINGBUILDER_TO_STRING_STORAGE, "STRINGBUILDER_TO_STRING_STORAGE"},
    {XR_TARGET_FAMILY_STRINGBUILDER_APPEND_STRING_STORAGE, "STRINGBUILDER_APPEND_STRING_STORAGE"},
    {XR_TARGET_FAMILY_JSON_NAMESPACE_VALUE_STORAGE, "JSON_NAMESPACE_VALUE_STORAGE"},
    {XR_TARGET_FAMILY_DIRECT_LOCAL_STRING_BOUNDARY_STORAGE, "DIRECT_LOCAL_STRING_BOUNDARY_STORAGE"},
    {XR_TARGET_FAMILY_ARRAY_ALLOCATION_STORAGE, "ARRAY_ALLOCATION_STORAGE"},
    {XR_TARGET_FAMILY_NATIVE_MODULE_NAMESPACE_STORAGE, "NATIVE_MODULE_NAMESPACE_STORAGE"},
    {XR_TARGET_FAMILY_NULLABLE_SCALAR_STORAGE, "NULLABLE_SCALAR_STORAGE"},
    {XR_TARGET_FAMILY_ARRAY_MEMBER_RESULT_STORAGE, "ARRAY_MEMBER_RESULT_STORAGE"},
    {XR_TARGET_FAMILY_SOURCE_CLASS_OBJECT_STORAGE, "SOURCE_CLASS_OBJECT_STORAGE"},
    {XR_TARGET_FAMILY_SOURCE_CLASS_INSTANCE_STORAGE, "SOURCE_CLASS_INSTANCE_STORAGE"},
    {XR_TARGET_FAMILY_SOURCE_CLASS_RECEIVER_STORAGE, "SOURCE_CLASS_RECEIVER_STORAGE"},
    {XR_TARGET_FAMILY_STRING_CONCAT_RESULT_STORAGE, "STRING_CONCAT_RESULT_STORAGE"},
    {XR_TARGET_FAMILY_SOURCE_CLASS_METHOD_RECEIVER_STORAGE, "SOURCE_CLASS_METHOD_RECEIVER_STORAGE"},
    {XR_TARGET_FAMILY_SOURCE_CLASS_ARGUMENT_STORAGE, "SOURCE_CLASS_ARGUMENT_STORAGE"},
    {XR_TARGET_FAMILY_DIRECT_LOCAL_GO_TASK_RESULT_STORAGE, "DIRECT_LOCAL_GO_TASK_RESULT_STORAGE"},
    {XR_TARGET_FAMILY_PANIC_CATCH_STORAGE, "PANIC_CATCH_STORAGE"},
    {XR_TARGET_FAMILY_ADT_ENUM_STORAGE, "ADT_ENUM_STORAGE"},
    {XR_TARGET_FAMILY_DIRECT_LOCAL_AGGREGATE_RESULT_STORAGE,
     "DIRECT_LOCAL_AGGREGATE_RESULT_STORAGE"},
};

/* ========== Row emission ========== */

typedef struct XrXtpTextRow {
    FILE *out;
    const char *prefix;
    const char *section;
    uint32_t row;
    uint32_t fields;
    bool open;
} XrXtpTextRow;

static void row_open(XrXtpTextRow *row) {
    fprintf(row->out, "%s[%s] %" PRIu32, row->prefix, row->section, row->row);
    row->fields = 0;
    row->open = true;
}

static void row_field_begin(XrXtpTextRow *row) {
    if (!row->open) {
        row_open(row);
        return;
    }
    if (row->fields >= XR_XTP_TEXT_FIELDS_PER_LINE) {
        fputc('\n', row->out);
        row_open(row);
    }
}

static void row_scalar(XrXtpTextRow *row, const char *name, uint64_t value) {
    row_field_begin(row);
    fprintf(row->out, " %s=%" PRIu64, name, value);
    row->fields++;
}

static void row_text(XrXtpTextRow *row, const char *name, const char *value) {
    row_field_begin(row);
    fprintf(row->out, " %s=%s", name, value);
    row->fields++;
}

static void row_stable_id(XrXtpTextRow *row, const char *name, XrStableId id) {
    char hex[XR_STABLE_ID_BYTES * 2 + 1];
    xr_stable_id_hex(id, hex);
    row_text(row, name, hex);
}

static void row_fingerprint(XrXtpTextRow *row, const char *name, XrFingerprint fingerprint) {
    char hex[XR_FINGERPRINT_BYTES * 2 + 1];
    xr_fingerprint_hex(fingerprint, hex);
    row_text(row, name, hex);
}

static void row_end(XrXtpTextRow *row) {
    if (!row->open)
        return;
    fputc('\n', row->out);
    row->open = false;
}

/* One renderer per typed row, expanded from the reader-side field inventory.
 * Naming the right fields depends on that inventory still matching the wire,
 * which section_layout_is_exact below proves before any row is rendered. */
#define XR_XTP_TEXT_U8(field) row_scalar(row, #field, (uint64_t) record->field);
#define XR_XTP_TEXT_U16(field) row_scalar(row, #field, (uint64_t) record->field);
#define XR_XTP_TEXT_U32(field) row_scalar(row, #field, (uint64_t) record->field);
#define XR_XTP_TEXT_U64(field) row_scalar(row, #field, (uint64_t) record->field);
#define XR_XTP_TEXT_ID(field) row_stable_id(row, #field, record->field);
#define XR_XTP_TEXT_FP(field) row_fingerprint(row, #field, record->field);
#define XR_XTP_TEXT_FIELD(kind, field) XR_XTP_TEXT_##kind(field)
#define XR_XTP_TEXT_DEFINE_RENDER(name, type, fields)                                              \
    static void render_##name(XrXtpTextRow *row, const type *record) {                             \
        fields(XR_XTP_TEXT_FIELD)                                                                  \
    }
XR_XTP_TEXT_TYPED_ROWS(XR_XTP_TEXT_DEFINE_RENDER)
#undef XR_XTP_TEXT_DEFINE_RENDER

/* Wire width the reader-side inventory accounts for, summed the same way the
 * codec sums its own. A table whose recorded row width differs is a table
 * this reader can no longer name, so rendering and comparison both stop. */
#define XR_XTP_TEXT_WIDTH_U8(field) +1u
#define XR_XTP_TEXT_WIDTH_U16(field) +2u
#define XR_XTP_TEXT_WIDTH_U32(field) +4u
#define XR_XTP_TEXT_WIDTH_U64(field) +8u
#define XR_XTP_TEXT_WIDTH_ID(field) +XR_STABLE_ID_BYTES
#define XR_XTP_TEXT_WIDTH_FP(field) +XR_FINGERPRINT_BYTES
#define XR_XTP_TEXT_WIDTH_FIELD(kind, field) XR_XTP_TEXT_WIDTH_##kind(field)

static uint32_t text_row_width(XrXtpSectionKind kind) {
    switch (kind) {
#define XR_XTP_TEXT_WIDTH_CASE(name, type, fields)                                                 \
    case XR_XTP_SECTION_##name:                                                                    \
        return (uint32_t) (0 fields(XR_XTP_TEXT_WIDTH_FIELD));
        XR_XTP_TEXT_TYPED_ROWS(XR_XTP_TEXT_WIDTH_CASE)
#undef XR_XTP_TEXT_WIDTH_CASE
        case XR_XTP_SECTION_ROOT_SLOTS:
            return 4u;
        default:
            return 0;
    }
}

static bool section_layout_is_exact(const XrXtpSectionView *view) {
    if (!view)
        return false;
    if (view->kind == XR_XTP_SECTION_INSTRUCTIONS)
        return view->flags == XR_XTP_SECTION_FLAG_COMPACT && view->row_size == 0;
    return view->flags == 0 && (view->count == 0 || view->row_size == text_row_width(view->kind));
}

/* Byte ranges inside a wire row that hold a digest of something else. A
 * changed instruction immediate also changes every digest computed over it,
 * so comparing digests in the same pass as ordinary fields would report
 * whichever derived row happens to come first and hide the row that actually
 * changed. These ranges are therefore compared only after every other byte of
 * every table already agrees. */
#define XR_XTP_TEXT_MAX_DIGEST_SPANS 8u

typedef struct XrXtpTextDigestSpan {
    uint32_t offset;
    uint32_t length;
} XrXtpTextDigestSpan;

#define XR_XTP_TEXT_SPAN_U8(field) cursor += 1u;
#define XR_XTP_TEXT_SPAN_U16(field) cursor += 2u;
#define XR_XTP_TEXT_SPAN_U32(field) cursor += 4u;
#define XR_XTP_TEXT_SPAN_U64(field) cursor += 8u;
#define XR_XTP_TEXT_SPAN_ID(field) cursor += XR_STABLE_ID_BYTES;
#define XR_XTP_TEXT_SPAN_FP(field)                                                                 \
    if (count < XR_XTP_TEXT_MAX_DIGEST_SPANS) {                                                    \
        spans[count].offset = cursor;                                                              \
        spans[count].length = XR_FINGERPRINT_BYTES;                                                \
        count++;                                                                                   \
    }                                                                                              \
    cursor += XR_FINGERPRINT_BYTES;
#define XR_XTP_TEXT_SPAN_FIELD(kind, field) XR_XTP_TEXT_SPAN_##kind(field)

static uint32_t section_digest_spans(XrXtpSectionKind kind, XrXtpTextDigestSpan *spans) {
    uint32_t cursor = 0;
    uint32_t count = 0;
    (void) cursor;
    switch (kind) {
#define XR_XTP_TEXT_SPAN_CASE(name, type, fields)                                                  \
    case XR_XTP_SECTION_##name:                                                                    \
        fields(XR_XTP_TEXT_SPAN_FIELD) break;
        XR_XTP_TEXT_TYPED_ROWS(XR_XTP_TEXT_SPAN_CASE)
#undef XR_XTP_TEXT_SPAN_CASE
        default:
            break;
    }
    return count;
}

/* True when the two rows agree everywhere a digest is not stored. */
static bool row_matches_outside_digests(const uint8_t *left, const uint8_t *right, uint32_t size,
                                        const XrXtpTextDigestSpan *spans, uint32_t span_count) {
    uint32_t cursor = 0;
    for (uint32_t i = 0; i < span_count; i++) {
        if (spans[i].offset > size)
            break;
        if (spans[i].offset > cursor &&
            memcmp(left + cursor, right + cursor, spans[i].offset - cursor) != 0)
            return false;
        cursor = spans[i].offset + spans[i].length;
        if (cursor > size)
            cursor = size;
    }
    return cursor >= size || memcmp(left + cursor, right + cursor, size - cursor) == 0;
}

typedef union XrXtpTextRowStorage {
#define XR_XTP_TEXT_UNION_MEMBER(name, type, fields) type name##_row;
    XR_XTP_TEXT_TYPED_ROWS(XR_XTP_TEXT_UNION_MEMBER)
#undef XR_XTP_TEXT_UNION_MEMBER
    uint32_t root_slot;
} XrXtpTextRowStorage;

static bool section_row_bytes(const XrXtpCandidate *candidate, const XrXtpSectionView *view,
                              uint32_t index, const uint8_t **bytes) {
    *bytes = NULL;
    if (!candidate || !view || view->kind == XR_XTP_SECTION_INSTRUCTIONS || !view->row_size ||
        index >= view->count)
        return false;
    size_t offset = view->offset + (size_t) index * (size_t) view->row_size;
    if (offset > candidate->size || candidate->size - offset < view->row_size)
        return false;
    *bytes = candidate->bytes + offset;
    return true;
}

static bool decode_row(const XrXtpCandidate *candidate, const XrXtpSectionView *view,
                       uint32_t index, XrXtpTextRowStorage *storage) {
    const uint8_t *bytes = NULL;
    if (!section_row_bytes(candidate, view, index, &bytes))
        return false;
    memset(storage, 0, sizeof(*storage));
    return xr_xtp_decode_rows(view->kind, bytes, 1u, storage);
}

static bool instruction_stream_init(const XrXtpCandidate *candidate, const XrXtpSectionView *view,
                                    XrXtpInstructionStream *stream) {
    return candidate && view && stream && view->kind == XR_XTP_SECTION_INSTRUCTIONS &&
           section_layout_is_exact(view) && view->offset <= candidate->size &&
           view->length <= candidate->size - view->offset &&
           xr_xtp_instruction_stream_init(stream, candidate->bytes + view->offset, view->length,
                                          view->count);
}

static void render_instruction_row(const XrTargetInstructionRecord *record, uint32_t index,
                                   const char *prefix, FILE *out) {
    XrXtpTextRow row = {out, prefix, "INSTRUCTIONS", index, 0, false};
    render_INSTRUCTIONS(&row, record);
    row_text(&row, "opcode-name", opcode_name(record->opcode));
    row_end(&row);
}

static bool render_decoded_row(const XrXtpCandidate *candidate, const XrXtpSectionView *view,
                               uint32_t index, const char *prefix, FILE *out) {
    XrXtpTextRowStorage storage;
    if (!decode_row(candidate, view, index, &storage))
        return false;
    XrXtpTextRow row = {out, prefix, section_name(view->kind), index, 0, false};
    switch (view->kind) {
#define XR_XTP_TEXT_RENDER_CASE(name, type, fields)                                                \
    case XR_XTP_SECTION_##name:                                                                    \
        render_##name(&row, &storage.name##_row);                                                  \
        break;
        XR_XTP_TEXT_TYPED_ROWS(XR_XTP_TEXT_RENDER_CASE)
#undef XR_XTP_TEXT_RENDER_CASE
        case XR_XTP_SECTION_ROOT_SLOTS:
            row_scalar(&row, "slot", storage.root_slot);
            break;
        default:
            break;
    }
    row_end(&row);
    return true;
}

static void emit_wire(FILE *out, const char *prefix, const uint8_t *bytes, uint32_t size) {
    fprintf(out, "%swire=", prefix);
    for (uint32_t i = 0; i < size; i++)
        fprintf(out, "%02x", bytes[i]);
    fputc('\n', out);
}

/* ========== Header fields ========== */

typedef enum XrXtpTextValueKind {
    XR_XTP_TEXT_VALUE_COUNT = 0,
    XR_XTP_TEXT_VALUE_MASK,
    XR_XTP_TEXT_VALUE_FINGERPRINT,
} XrXtpTextValueKind;

typedef struct XrXtpTextHeaderField {
    const char *name;
    XrXtpTextValueKind kind;
    uint64_t scalar;
    XrFingerprint fingerprint;
} XrXtpTextHeaderField;

static uint32_t collect_header_fields(const XrXtpCandidate *candidate,
                                      XrXtpTextHeaderField *fields) {
    XrXtpIdentity identity;
    XrXtpResourceManifest resources;
    if (!xr_xtp_candidate_identity(candidate, &identity) ||
        !xr_xtp_candidate_resources(candidate, &resources))
        return 0;
    uint32_t count = 0;
#define XR_XTP_TEXT_PUSH_COUNT(label, value)                                                       \
    fields[count].name = (label);                                                                  \
    fields[count].kind = XR_XTP_TEXT_VALUE_COUNT;                                                  \
    fields[count].scalar = (uint64_t) (value);                                                     \
    count++;
#define XR_XTP_TEXT_PUSH_MASK(label, value)                                                        \
    fields[count].name = (label);                                                                  \
    fields[count].kind = XR_XTP_TEXT_VALUE_MASK;                                                   \
    fields[count].scalar = (uint64_t) (value);                                                     \
    count++;
#define XR_XTP_TEXT_PUSH_FINGERPRINT(label, value)                                                 \
    fields[count].name = (label);                                                                  \
    fields[count].kind = XR_XTP_TEXT_VALUE_FINGERPRINT;                                            \
    fields[count].fingerprint = (value);                                                           \
    count++;
    XR_XTP_TEXT_PUSH_COUNT("identity.semantic-schema", identity.semantic_schema)
    XR_XTP_TEXT_PUSH_COUNT("identity.profile-schema", identity.profile_schema)
    XR_XTP_TEXT_PUSH_COUNT("identity.plan-schema", identity.plan_schema)
    XR_XTP_TEXT_PUSH_MASK("identity.completed-family-mask", identity.completed_family_mask)
    XR_XTP_TEXT_PUSH_FINGERPRINT("identity.semantic-fingerprint", identity.semantic_fingerprint)
    XR_XTP_TEXT_PUSH_FINGERPRINT("identity.operation-registry-fingerprint",
                                 identity.operation_registry_fingerprint)
    XR_XTP_TEXT_PUSH_FINGERPRINT("identity.profile-fingerprint", identity.profile_fingerprint)
    XR_XTP_TEXT_PUSH_FINGERPRINT("identity.plan-fingerprint", identity.plan_fingerprint)
    XR_XTP_TEXT_PUSH_FINGERPRINT("identity.runtime-fingerprint", identity.runtime_fingerprint)
    XR_XTP_TEXT_PUSH_FINGERPRINT("identity.provider-fingerprint", identity.provider_fingerprint)
    XR_XTP_TEXT_PUSH_FINGERPRINT("identity.object-fingerprint", identity.object_fingerprint)
    XR_XTP_TEXT_PUSH_COUNT("resources.total-rows", resources.total_rows)
    XR_XTP_TEXT_PUSH_COUNT("resources.table-bytes", resources.table_bytes)
    XR_XTP_TEXT_PUSH_COUNT("resources.total-frame-bytes", resources.total_frame_bytes)
    XR_XTP_TEXT_PUSH_COUNT("resources.verification-work-units", resources.verification_work_units)
#undef XR_XTP_TEXT_PUSH_COUNT
#undef XR_XTP_TEXT_PUSH_MASK
#undef XR_XTP_TEXT_PUSH_FINGERPRINT
    return count;
}

static void emit_header_value(FILE *out, const char *prefix, const XrXtpTextHeaderField *field) {
    char hex[XR_FINGERPRINT_BYTES * 2 + 1];
    switch (field->kind) {
        case XR_XTP_TEXT_VALUE_MASK:
            fprintf(out, "%s0x%016" PRIx64 "\n", prefix, field->scalar);
            return;
        case XR_XTP_TEXT_VALUE_FINGERPRINT:
            xr_fingerprint_hex(field->fingerprint, hex);
            fprintf(out, "%s%s\n", prefix, hex);
            return;
        case XR_XTP_TEXT_VALUE_COUNT:
        default:
            fprintf(out, "%s%" PRIu64 "\n", prefix, field->scalar);
            return;
    }
}

static bool header_fields_equal(const XrXtpTextHeaderField *left,
                                const XrXtpTextHeaderField *right) {
    if (left->kind != right->kind)
        return false;
    if (left->kind == XR_XTP_TEXT_VALUE_FINGERPRINT)
        return memcmp(left->fingerprint.bytes, right->fingerprint.bytes, XR_FINGERPRINT_BYTES) == 0;
    return left->scalar == right->scalar;
}

static void emit_family_names(FILE *out, uint64_t mask) {
    fputs("identity.completed-families=", out);
    uint64_t remaining = mask;
    bool printed = false;
    for (size_t i = 0; i < sizeof(family_names) / sizeof(family_names[0]); i++) {
        if ((mask & family_names[i].bit) == 0)
            continue;
        if (printed)
            fputc('|', out);
        fputs(family_names[i].name, out);
        printed = true;
        remaining &= ~family_names[i].bit;
    }
    if (!printed)
        fputs("none", out);
    if (remaining)
        fprintf(out, "|unnamed:0x%016" PRIx64, remaining);
    fputc('\n', out);
}

/* ========== Dump ========== */

XR_FUNC bool xr_xtp_candidate_dump(const XrXtpCandidate *candidate, FILE *out) {
    if (!candidate || !out)
        return false;
    XrXtpTextHeaderField fields[XR_XTP_TEXT_HEADER_FIELD_COUNT];
    uint32_t field_count = collect_header_fields(candidate, fields);
    if (field_count != XR_XTP_TEXT_HEADER_FIELD_COUNT)
        return false;

    fprintf(out, "xray-plan-dump revision=%" PRIu32 " schema=%" PRIu32 "\n", XR_XTP_TEXT_REVISION,
            XR_XTP_SCHEMA_VERSION);
    fprintf(out, "artifact.bytes=%" PRIu64 "\n", (uint64_t) candidate->size);
    for (uint32_t i = 0; i < field_count; i++) {
        char prefix[128];
        snprintf(prefix, sizeof(prefix), "%s=", fields[i].name);
        emit_header_value(out, prefix, &fields[i]);
        if (fields[i].kind == XR_XTP_TEXT_VALUE_MASK)
            emit_family_names(out, fields[i].scalar);
    }

    for (uint32_t kind = 1; kind < (uint32_t) XR_XTP_SECTION_COUNT; kind++) {
        const XrXtpSectionView *view = xr_xtp_candidate_section(candidate, (XrXtpSectionKind) kind);
        uint32_t count = view ? view->count : 0;
        uint32_t row_size = view ? view->row_size : 0;
        fprintf(out,
                "section %s count=%" PRIu32 " row-size=%" PRIu32 " flags=0x%08" PRIx32
                " bytes=%" PRIu64 "\n",
                section_name((XrXtpSectionKind) kind), count, row_size, view ? view->flags : 0,
                view ? (uint64_t) view->length : 0);
    }

    for (uint32_t kind = 1; kind < (uint32_t) XR_XTP_SECTION_COUNT; kind++) {
        const XrXtpSectionView *view = xr_xtp_candidate_section(candidate, (XrXtpSectionKind) kind);
        if (!view)
            continue;
        if (!section_layout_is_exact(view))
            return false;
        if (view->kind == XR_XTP_SECTION_INSTRUCTIONS) {
            XrXtpInstructionStream stream;
            if (!instruction_stream_init(candidate, view, &stream))
                return false;
            for (uint32_t index = 0; index < view->count; index++) {
                XrTargetInstructionRecord record;
                if (!xr_xtp_instruction_stream_next(&stream, &record))
                    return false;
                render_instruction_row(&record, index, "", out);
            }
            if (!xr_xtp_instruction_stream_finish(&stream))
                return false;
            continue;
        }
        for (uint32_t index = 0; index < view->count; index++)
            if (!render_decoded_row(candidate, view, index, "", out))
                return false;
    }
    return true;
}

/* ========== Comparison ========== */

static void report_row_side(const XrXtpCandidate *candidate, const XrXtpSectionView *view,
                            uint32_t index, const char *prefix, FILE *out) {
    const uint8_t *bytes = NULL;
    if (!render_decoded_row(candidate, view, index, prefix, out) ||
        !section_row_bytes(candidate, view, index, &bytes)) {
        fprintf(out, "%s[%s] %" PRIu32 " unreadable\n", prefix, section_name(view->kind), index);
        return;
    }
    emit_wire(out, prefix, bytes, view->row_size);
}

static void report_row_difference(const XrXtpCandidate *left, const XrXtpCandidate *right,
                                  const XrXtpSectionView *left_view,
                                  const XrXtpSectionView *right_view, uint32_t index,
                                  uint32_t context_rows, FILE *out) {
    fprintf(out, "plan-diff first-difference section=%s row=%" PRIu32 "\n",
            section_name(left_view->kind), index);
    uint32_t before = index < context_rows ? index : context_rows;
    for (uint32_t i = index - before; i < index; i++)
        render_decoded_row(left, left_view, i, "  ", out);
    report_row_side(left, left_view, index, "- ", out);
    report_row_side(right, right_view, index, "+ ", out);
    for (uint32_t i = index + 1u; i <= index + context_rows; i++) {
        if (i < left_view->count)
            render_decoded_row(left, left_view, i, "- ", out);
        if (i < right_view->count)
            render_decoded_row(right, right_view, i, "+ ", out);
    }
}

static bool instruction_rows_equal(const XrTargetInstructionRecord *left,
                                   const XrTargetInstructionRecord *right) {
    uint8_t left_bytes[32] = {0};
    uint8_t right_bytes[32] = {0};
    return xr_xtp_encode_rows(XR_XTP_SECTION_INSTRUCTIONS, left, 1u, left_bytes) &&
           xr_xtp_encode_rows(XR_XTP_SECTION_INSTRUCTIONS, right, 1u, right_bytes) &&
           memcmp(left_bytes, right_bytes, sizeof(left_bytes)) == 0;
}

static void report_instruction_side(const XrTargetInstructionRecord *record, uint32_t index,
                                    const char *prefix, FILE *out) {
    uint8_t bytes[32] = {0};
    render_instruction_row(record, index, prefix, out);
    if (!xr_xtp_encode_rows(XR_XTP_SECTION_INSTRUCTIONS, record, 1u, bytes))
        return;
    fprintf(out, "%scanonical-row=", prefix);
    for (uint32_t offset = 0; offset < sizeof(bytes); offset++)
        fprintf(out, "%02x", bytes[offset]);
    fputc('\n', out);
}

static bool compare_instruction_streams(const XrXtpCandidate *left, const XrXtpCandidate *right,
                                        const XrXtpSectionView *left_view,
                                        const XrXtpSectionView *right_view, uint32_t context_rows,
                                        FILE *out, bool *different) {
    *different = false;
    XrXtpInstructionStream left_stream;
    XrXtpInstructionStream right_stream;
    if (!instruction_stream_init(left, left_view, &left_stream) ||
        !instruction_stream_init(right, right_view, &right_stream))
        return false;
    XrTargetInstructionRecord prior[XR_XTP_TEXT_MAX_CONTEXT_ROWS];
    uint32_t prior_count = 0;
    uint32_t prior_cursor = 0;
    for (uint32_t index = 0; index < left_view->count; index++) {
        XrTargetInstructionRecord left_row;
        XrTargetInstructionRecord right_row;
        if (!xr_xtp_instruction_stream_next(&left_stream, &left_row) ||
            !xr_xtp_instruction_stream_next(&right_stream, &right_row))
            return false;
        if (!instruction_rows_equal(&left_row, &right_row)) {
            fprintf(out, "plan-diff first-difference section=INSTRUCTIONS row=%" PRIu32 "\n",
                    index);
            uint32_t shown = prior_count < context_rows ? prior_count : context_rows;
            uint32_t start = (prior_cursor + XR_XTP_TEXT_MAX_CONTEXT_ROWS - shown) %
                             XR_XTP_TEXT_MAX_CONTEXT_ROWS;
            for (uint32_t offset = 0; offset < shown; offset++) {
                uint32_t slot = (start + offset) % XR_XTP_TEXT_MAX_CONTEXT_ROWS;
                render_instruction_row(&prior[slot], index - shown + offset, "  ", out);
            }
            report_instruction_side(&left_row, index, "- ", out);
            report_instruction_side(&right_row, index, "+ ", out);
            for (uint32_t offset = 1; offset <= context_rows; offset++) {
                XrTargetInstructionRecord next_left;
                XrTargetInstructionRecord next_right;
                if (index + offset < left_view->count &&
                    xr_xtp_instruction_stream_next(&left_stream, &next_left))
                    render_instruction_row(&next_left, index + offset, "- ", out);
                if (index + offset < right_view->count &&
                    xr_xtp_instruction_stream_next(&right_stream, &next_right))
                    render_instruction_row(&next_right, index + offset, "+ ", out);
            }
            *different = true;
            return true;
        }
        if (context_rows) {
            prior[prior_cursor] = left_row;
            prior_cursor = (prior_cursor + 1u) % XR_XTP_TEXT_MAX_CONTEXT_ROWS;
            if (prior_count < XR_XTP_TEXT_MAX_CONTEXT_ROWS)
                prior_count++;
        }
    }
    return xr_xtp_instruction_stream_finish(&left_stream) &&
           xr_xtp_instruction_stream_finish(&right_stream);
}

XR_FUNC bool xr_xtp_candidate_diff(const XrXtpCandidate *left, const XrXtpCandidate *right,
                                   uint32_t context_rows, FILE *out, bool *identical) {
    if (!left || !right || !out || !identical)
        return false;
    *identical = false;
    if (context_rows > XR_XTP_TEXT_MAX_CONTEXT_ROWS)
        context_rows = XR_XTP_TEXT_MAX_CONTEXT_ROWS;

    XrXtpTextHeaderField left_fields[XR_XTP_TEXT_HEADER_FIELD_COUNT];
    XrXtpTextHeaderField right_fields[XR_XTP_TEXT_HEADER_FIELD_COUNT];
    if (collect_header_fields(left, left_fields) != XR_XTP_TEXT_HEADER_FIELD_COUNT ||
        collect_header_fields(right, right_fields) != XR_XTP_TEXT_HEADER_FIELD_COUNT)
        return false;
    for (uint32_t i = 0; i < XR_XTP_TEXT_STRUCTURAL_FIELD_COUNT; i++) {
        if (header_fields_equal(&left_fields[i], &right_fields[i]))
            continue;
        fprintf(out, "plan-diff first-difference field=%s\n", left_fields[i].name);
        emit_header_value(out, "- ", &left_fields[i]);
        emit_header_value(out, "+ ", &right_fields[i]);
        return true;
    }

    for (uint32_t kind = 1; kind < (uint32_t) XR_XTP_SECTION_COUNT; kind++) {
        const XrXtpSectionView *left_view = xr_xtp_candidate_section(left, (XrXtpSectionKind) kind);
        const XrXtpSectionView *right_view =
            xr_xtp_candidate_section(right, (XrXtpSectionKind) kind);
        uint32_t left_count = left_view ? left_view->count : 0;
        uint32_t right_count = right_view ? right_view->count : 0;
        uint32_t left_size = left_view ? left_view->row_size : 0;
        uint32_t right_size = right_view ? right_view->row_size : 0;
        uint32_t left_flags = left_view ? left_view->flags : 0;
        uint32_t right_flags = right_view ? right_view->flags : 0;
        if (left_count != right_count) {
            fprintf(out, "plan-diff first-difference section=%s field=count\n",
                    section_name((XrXtpSectionKind) kind));
            fprintf(out, "- %" PRIu32 "\n+ %" PRIu32 "\n", left_count, right_count);
            return true;
        }
        if (left_size != right_size) {
            fprintf(out, "plan-diff first-difference section=%s field=row-size\n",
                    section_name((XrXtpSectionKind) kind));
            fprintf(out, "- %" PRIu32 "\n+ %" PRIu32 "\n", left_size, right_size);
            return true;
        }
        if (left_flags != right_flags) {
            fprintf(out, "plan-diff first-difference section=%s field=flags\n",
                    section_name((XrXtpSectionKind) kind));
            fprintf(out, "- 0x%08" PRIx32 "\n+ 0x%08" PRIx32 "\n", left_flags, right_flags);
            return true;
        }
        if ((left_view && !section_layout_is_exact(left_view)) ||
            (right_view && !section_layout_is_exact(right_view)))
            return false;
    }

    /* Pass one ignores stored digests so the reported row is the one that
     * changed rather than one that merely summarises it; pass two compares
     * the digests, which by then can only differ on their own. */
    for (uint32_t pass = 0; pass < 2u; pass++) {
        for (uint32_t kind = 1; kind < (uint32_t) XR_XTP_SECTION_COUNT; kind++) {
            const XrXtpSectionView *left_view =
                xr_xtp_candidate_section(left, (XrXtpSectionKind) kind);
            const XrXtpSectionView *right_view =
                xr_xtp_candidate_section(right, (XrXtpSectionKind) kind);
            if (!left_view || !right_view)
                continue;
            if (kind == XR_XTP_SECTION_INSTRUCTIONS) {
                if (pass != 0)
                    continue;
                bool different = false;
                if (!compare_instruction_streams(left, right, left_view, right_view, context_rows,
                                                 out, &different))
                    return false;
                if (different)
                    return true;
                continue;
            }
            XrXtpTextDigestSpan spans[XR_XTP_TEXT_MAX_DIGEST_SPANS];
            uint32_t span_count =
                pass == 0 ? section_digest_spans((XrXtpSectionKind) kind, spans) : 0;
            for (uint32_t index = 0; index < left_view->count; index++) {
                const uint8_t *left_bytes = NULL;
                const uint8_t *right_bytes = NULL;
                if (!section_row_bytes(left, left_view, index, &left_bytes) ||
                    !section_row_bytes(right, right_view, index, &right_bytes))
                    return false;
                if (row_matches_outside_digests(left_bytes, right_bytes, left_view->row_size, spans,
                                                span_count))
                    continue;
                report_row_difference(left, right, left_view, right_view, index, context_rows, out);
                return true;
            }
        }
    }

    /* Every table row agrees, so any remaining identity or resource
     * difference is a fact derived from content that no longer follows from
     * it. Reported last precisely because it has no row to point at. */
    for (uint32_t i = XR_XTP_TEXT_STRUCTURAL_FIELD_COUNT; i < XR_XTP_TEXT_HEADER_FIELD_COUNT; i++) {
        if (header_fields_equal(&left_fields[i], &right_fields[i]))
            continue;
        fprintf(out, "plan-diff first-difference field=%s\n", left_fields[i].name);
        emit_header_value(out, "- ", &left_fields[i]);
        emit_header_value(out, "+ ", &right_fields[i]);
        return true;
    }

    if (left->size != right->size) {
        fprintf(out, "plan-diff first-difference field=artifact.bytes\n");
        fprintf(out, "- %" PRIu64 "\n+ %" PRIu64 "\n", (uint64_t) left->size,
                (uint64_t) right->size);
        return true;
    }
    for (size_t i = 0; i < left->size; i++) {
        if (left->bytes[i] == right->bytes[i])
            continue;
        fprintf(out, "plan-diff first-difference field=artifact.byte offset=%" PRIu64 "\n",
                (uint64_t) i);
        fprintf(out, "- 0x%02x\n+ 0x%02x\n", left->bytes[i], right->bytes[i]);
        return true;
    }

    *identical = true;
    fprintf(out, "plan-diff identical bytes=%" PRIu64 "\n", (uint64_t) left->size);
    return true;
}
