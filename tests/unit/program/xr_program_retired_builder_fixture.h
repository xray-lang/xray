/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_retired_builder_fixture.h - Retired identities fail at admission
 */
#ifndef XR_PROGRAM_RETIRED_BUILDER_FIXTURE_H
#define XR_PROGRAM_RETIRED_BUILDER_FIXTURE_H

#include "xr_program_text_fixture.h"
#include "program/xr_program_verify.h"

#include <stdio.h>
#include <string.h>

_Static_assert(XR_PROGRAM_FORMAT_MAJOR == 3u && XR_PROGRAM_FORMAT_MINOR == 12u,
               "retirement controls require the current wire format");
_Static_assert(XR_CORE_PROGRAM_BUILTIN_TYPE_COUNT == 19u,
               "retired type must not occupy a builtin row");

/* The same bounded uvar walk used by artifact mutation tests. This helper
 * visits only the known, already admitted text fixture prefix. */
static uint64_t xr_retired_builder_take(const uint8_t *bytes, size_t size, size_t *cursor) {
    uint64_t value = 0u;
    for (unsigned shift = 0u; shift < 64u && *cursor < size; shift += 7u) {
        uint8_t byte = bytes[(*cursor)++];
        if (shift == 63u && byte > 1u)
            return UINT64_MAX;
        value |= (uint64_t) (byte & UINT8_C(0x7f)) << shift;
        if ((byte & UINT8_C(0x80)) == 0u)
            return value;
    }
    return UINT64_MAX;
}

static bool xr_retired_builder_concat_offsets(const XrProgramArtifact *artifact,
                                               size_t *operation, size_t *result_type) {
    XrProgramView view;
    if (xr_program_decode_structure(artifact->bytes, artifact->size, NULL, &view, NULL, 0u) !=
        XR_PROGRAM_DECODE_OK)
        return false;
    size_t cursor = (size_t) view.sections[XR_PROGRAM_SECTION_CODE - 1u].offset;
    if (xr_retired_builder_take(artifact->bytes, artifact->size, &cursor) != 1u ||
        xr_retired_builder_take(artifact->bytes, artifact->size, &cursor) != 0u ||
        xr_retired_builder_take(artifact->bytes, artifact->size, &cursor) != 1u ||
        xr_retired_builder_take(artifact->bytes, artifact->size, &cursor) != 0u ||
        xr_retired_builder_take(artifact->bytes, artifact->size, &cursor) != 0u ||
        xr_retired_builder_take(artifact->bytes, artifact->size, &cursor) < 4u)
        return false;
    for (unsigned index = 0u; index < 3u; ++index) {
        uint16_t expected = index < 2u ? XR_CORE_OP_CORE_CONSTANT_STRING : XR_CORE_OP_CORE_CONSTANT_I64;
        uint16_t type = index < 2u ? XR_CORE_TYPE_STRING : XR_CORE_TYPE_I64;
        uint8_t owner = index < 2u ? XR_CORE_IR_OWNER : XR_CORE_IR_NON_OWNER;
        if (xr_retired_builder_take(artifact->bytes, artifact->size, &cursor) != expected ||
            xr_retired_builder_take(artifact->bytes, artifact->size, &cursor) == 0u ||
            xr_retired_builder_take(artifact->bytes, artifact->size, &cursor) != type ||
            xr_retired_builder_take(artifact->bytes, artifact->size, &cursor) != XR_CORE_IR_VALUE ||
            xr_retired_builder_take(artifact->bytes, artifact->size, &cursor) != owner ||
            xr_retired_builder_take(artifact->bytes, artifact->size, &cursor) != 0u ||
            xr_retired_builder_take(artifact->bytes, artifact->size, &cursor) != XR_CORE_IR_IMMEDIATE_CONSTANT ||
            xr_retired_builder_take(artifact->bytes, artifact->size, &cursor) == UINT64_MAX ||
            xr_retired_builder_take(artifact->bytes, artifact->size, &cursor) != 0u)
            return false;
    }
    *operation = cursor;
    if (xr_retired_builder_take(artifact->bytes, artifact->size, &cursor) !=
            XR_CORE_OP_CORE_STRING_CONCAT ||
        xr_retired_builder_take(artifact->bytes, artifact->size, &cursor) == 0u)
        return false;
    *result_type = cursor;
    return xr_retired_builder_take(artifact->bytes, artifact->size, &cursor) == XR_CORE_TYPE_STRING &&
           *operation + 1u < artifact->size &&
           artifact->bytes[*operation] == UINT8_C(0x97) &&
           artifact->bytes[*operation + 1u] == UINT8_C(0x01);
}

static bool xr_retired_builder_admission(const XrProgramArtifact *artifact,
                                          XrProgramDecodeStatus expected) {
    XrProgramView view;
    char message[128] = {0};
    XrProgramDecodeStatus decoded = xr_program_decode_structure(
        artifact->bytes, artifact->size, NULL, &view, message, sizeof(message));
    XrValidatedProgram *program = NULL;
    XrProgramDiagnostic diagnostic = {0};
    XrProgramVerifyStatus verified = xr_program_validate(
        artifact->bytes, artifact->size, NULL, &program, &diagnostic);
    bool ok = decoded == expected &&
        (expected == XR_PROGRAM_DECODE_OK
             ? verified == XR_PROGRAM_VERIFY_OK && program != NULL &&
                   view.format_major == 3u && view.format_minor == 12u
             : verified == XR_PROGRAM_VERIFY_STRUCTURAL_REJECTED && program == NULL &&
                   diagnostic.kind == XR_PROGRAM_DIAGNOSTIC_STRUCTURAL &&
                   diagnostic.decode_status == expected);
    if (expected != XR_PROGRAM_DECODE_OK) {
        char exact[128];
        snprintf(exact, sizeof(exact), "XrProgram decode failed: %s",
                 xr_program_decode_status_name(expected));
        ok = ok && strcmp(message, exact) == 0;
    }
    if (!ok)
        fprintf(stderr, "retired identity admission mismatch: decode=%u verify=%u kind=%u message=%s\n",
                (unsigned) decoded, (unsigned) verified, (unsigned) diagnostic.kind, message);
    xr_validated_program_free(program);
    return ok;
}

/* Every rejected artifact is the admitted ordinary-string artifact with one
 * byte changed. Unknown operation lookup is checked separately because the
 * structural decoder reports unsupported content before semantic arity. */
static bool xr_program_retired_builder_admission_controls(void) {
    XrProgramArtifact artifact = {0};
    size_t operation = 0u, result_type = 0u;
    char diagnostic[128] = {0};
    bool ok = xr_program_text_fixture_write(&artifact, diagnostic, sizeof(diagnostic)) ==
                  XR_PROGRAM_BUILD_OK &&
              xr_retired_builder_admission(&artifact, XR_PROGRAM_DECODE_OK) &&
              xr_retired_builder_concat_offsets(&artifact, &operation, &result_type);
    if (!ok)
        goto done;
    for (uint16_t retired = 173u; retired <= 177u; ++retired) {
        if (xr_core_spec_operation_by_id(retired) != NULL) {
            ok = false;
            goto done;
        }
        artifact.bytes[operation] = (uint8_t) ((retired & UINT16_C(0x7f)) | UINT16_C(0x80));
        ok = xr_retired_builder_admission(&artifact, XR_PROGRAM_DECODE_UNSUPPORTED_SECTION_CONTENT);
        artifact.bytes[operation] = UINT8_C(0x97);
        if (!ok || !xr_retired_builder_admission(&artifact, XR_PROGRAM_DECODE_OK)) {
            ok = false;
            goto done;
        }
    }
    if (xr_program_builtin_type_row(20u) != NULL ||
        artifact.bytes[result_type] != XR_CORE_TYPE_STRING) {
        ok = false;
        goto done;
    }
    artifact.bytes[result_type] = UINT8_C(20);
    ok = xr_retired_builder_admission(&artifact, XR_PROGRAM_DECODE_UNSUPPORTED_SECTION_CONTENT);
    artifact.bytes[result_type] = XR_CORE_TYPE_STRING;
    if (!ok || !xr_retired_builder_admission(&artifact, XR_PROGRAM_DECODE_OK)) {
        ok = false;
        goto done;
    }
    size_t minor = XR_PROGRAM_MAGIC_SIZE + 2u;
    if (minor + 1u >= artifact.size || artifact.bytes[minor] != 12u ||
        artifact.bytes[minor + 1u] != 0u) {
        ok = false;
        goto done;
    }
    artifact.bytes[minor] = UINT8_C(11);
    ok = xr_retired_builder_admission(&artifact, XR_PROGRAM_DECODE_UNSUPPORTED_VERSION);
    artifact.bytes[minor] = UINT8_C(12);
    ok = ok && xr_retired_builder_admission(&artifact, XR_PROGRAM_DECODE_OK);
done:
    xr_program_artifact_free(&artifact);
    return ok;
}

#endif // XR_PROGRAM_RETIRED_BUILDER_FIXTURE_H
