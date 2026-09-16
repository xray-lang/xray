/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_identity.c - Runtime-side canonical XrProgram identity
 */

#include "xr_program_internal.h"
#include "xr_program_schema_gen.h"

#include "../base/xsha256.h"
#include "../core/xr_core_spec_gen.h"
#include "../runtime/core/xr_text_kernel.h"

#include <string.h>

/* Ascending by type id.  `string` is the only affine builtin with an explicit
 * copy; `panic-info` is affine and never copied; everything else is a trivial
 * scalar. */
static const XrProgramBuiltinTypeRow builtin_type_rows[XR_CORE_PROGRAM_BUILTIN_TYPE_COUNT] = {
    {XR_CORE_TYPE_VOID, XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL, XR_CORE_IR_COPY_FORBIDDEN},
    {XR_CORE_TYPE_BOOL, XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL, XR_CORE_IR_COPY_TRIVIAL},
    {XR_CORE_TYPE_I64, XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL, XR_CORE_IR_COPY_TRIVIAL},
    {XR_CORE_TYPE_U32, XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL, XR_CORE_IR_COPY_TRIVIAL},
    {XR_CORE_TYPE_ERROR, XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL, XR_CORE_IR_COPY_TRIVIAL},
    {XR_CORE_TYPE_PANIC_INFO, XR_CORE_IR_TYPE_OWNERSHIP_AFFINE, XR_CORE_IR_COPY_FORBIDDEN},
    {XR_CORE_TYPE_U16, XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL, XR_CORE_IR_COPY_TRIVIAL},
    {XR_CORE_TYPE_TARGET_OS, XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL, XR_CORE_IR_COPY_TRIVIAL},
    {XR_CORE_TYPE_TARGET_ARCH, XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL, XR_CORE_IR_COPY_TRIVIAL},
    {XR_CORE_TYPE_TARGET_ABI, XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL, XR_CORE_IR_COPY_TRIVIAL},
    {XR_CORE_TYPE_TARGET_ENDIAN, XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL, XR_CORE_IR_COPY_TRIVIAL},
    {XR_CORE_TYPE_STRING, XR_CORE_IR_TYPE_OWNERSHIP_AFFINE, XR_CORE_IR_COPY_EXPLICIT},
    {XR_CORE_TYPE_RUNE, XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL, XR_CORE_IR_COPY_TRIVIAL},
};

const XrProgramBuiltinTypeRow *xr_program_builtin_type_row(uint16_t type_id) {
    for (uint32_t index = 0; index < XR_CORE_PROGRAM_BUILTIN_TYPE_COUNT; ++index) {
        if (builtin_type_rows[index].type_id == type_id)
            return &builtin_type_rows[index];
    }
    return NULL;
}

const XrProgramBuiltinTypeRow *xr_program_builtin_type_row_at(uint32_t index) {
    return index < XR_CORE_PROGRAM_BUILTIN_TYPE_COUNT ? &builtin_type_rows[index] : NULL;
}

void xr_program_compute_id(const uint8_t *bytes, size_t size, XrProgramId *id_out) {
    static const uint8_t domain[] = XR_PROGRAM_ID_DOMAIN;
    XrSHA256Context context;
    if (!id_out)
        return;
    memset(id_out, 0, sizeof(*id_out));
    if (!bytes && size != 0)
        return;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain));
    xr_sha256_update(&context, bytes, size);
    xr_sha256_final(&context, id_out->bytes);
}

bool xr_program_id_equal(XrProgramId left, XrProgramId right) {
    return memcmp(left.bytes, right.bytes, XR_PROGRAM_DIGEST_SIZE) == 0;
}

/* A constant row is canonical when its kind names exactly its builtin type and
 * its payload is admissible: any i64, any bool, strict UTF-8 text within the
 * artifact-wide byte ceiling, or one Unicode scalar value. */
bool xr_program_constant_payload_is_canonical(uint16_t type_id, XrCoreIrConstantKind kind,
                                              const uint8_t *string_bytes, uint32_t string_size,
                                              uint32_t rune) {
    switch (kind) {
        case XR_CORE_IR_CONSTANT_I64:
            return type_id == XR_CORE_TYPE_I64;
        case XR_CORE_IR_CONSTANT_BOOL:
            return type_id == XR_CORE_TYPE_BOOL;
        case XR_CORE_IR_CONSTANT_STRING:
            return type_id == XR_CORE_TYPE_STRING &&
                   string_size <= XR_PROGRAM_CONSTANT_STRING_MAX_BYTES &&
                   (string_bytes || string_size == 0u) &&
                   xr_text_utf8_is_valid(string_bytes, string_size);
        case XR_CORE_IR_CONSTANT_RUNE:
            return type_id == XR_CORE_TYPE_RUNE && xr_text_rune_is_scalar(rune);
        default:
            return false;
    }
}

bool xr_program_constant_is_canonical(const XrCoreIrConstantInput *constant) {
    if (!constant)
        return false;
    return xr_program_constant_payload_is_canonical(
        constant->type_id, constant->kind,
        constant->kind == XR_CORE_IR_CONSTANT_STRING ? constant->value.string.bytes : NULL,
        constant->kind == XR_CORE_IR_CONSTANT_STRING ? constant->value.string.size : 0u,
        constant->kind == XR_CORE_IR_CONSTANT_RUNE ? constant->value.rune : 0u);
}
