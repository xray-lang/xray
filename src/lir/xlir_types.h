/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_types.h - Module-owned physical aggregate identities
 *
 * KEY CONCEPT:
 *   Constructed IDs belong to their receiving module. Their complete shape
 *   is the type of values and signatures, including module-state payloads.
 */
#ifndef XLIR_TYPES_H
#define XLIR_TYPES_H
#include "xlir.h"

typedef enum XrLirTypeKind {
    XR_LIR_TYPE_BUILTIN,
    XR_LIR_TYPE_FIXED_ARRAY,
    XR_LIR_TYPE_NULLABLE,
    XR_LIR_TYPE_CALLABLE,
    XR_LIR_TYPE_CELL,
    XR_LIR_TYPE_TUPLE,
    XR_LIR_TYPE_RECORD
} XrLirTypeKind;

typedef struct XrLirTypeView {
    XrLirTypeKind kind;
    XrLirType element;
    uint32_t count;
} XrLirTypeView;

typedef struct XrLirTypeLayout {
    uint64_t size;
    uint32_t alignment, scalar_count;
    bool managed;
} XrLirTypeLayout;

typedef struct XrLirScalarPart {
    XrLirType type;
    uint64_t offset;
} XrLirScalarPart;

typedef struct XrLirRecordField {
    const char *name;
    uint32_t name_length;
    XrLirType type;
} XrLirRecordField;

/* Finished-module inventory. An unreadable module returns zero. Constructed
 * ordinals start at XR_LIR_TYPE_COUNT and follow their owned interning order. */
XR_FUNC uint32_t xr_lir_constructed_type_count(const XrLirModule *);

/* Interning copies the complete shape and preserves output on failure.
 * The admitted fixed-byte shape is U8 with the Checked count 1..65535.
 * Builtin ordinals remain global. Other IDs are local to the given module.
 * No descriptor borrows a frontend type pool or allocates an execution value. */
XR_FUNC XrLirStatus xr_lir_fixed_array_type(XrLirModule *, XrLirType element,
    uint32_t count, XrLirType *output);
/* Nullable stores an inline tag and exact POD payload. Scalar data, fixed
 * bytes and earlier Nullable types are admitted; managed values and authority
 * capabilities have no nullable physical projection through this operation. */
XR_FUNC XrLirStatus xr_lir_nullable_type(XrLirModule *, XrLirType element, XrLirType *output);
/* This physical Tuple family contains count I64 fields in logical order.
 * Other Tuple payload families require their own admitted exact layout. */
XR_FUNC XrLirStatus xr_lir_tuple_i64_type(XrLirModule *, uint32_t count, XrLirType *output);
/* Structural fields are in canonical key order. The receiving module owns
 * every field name and exact dependency; no frontend metadata is borrowed. */
XR_FUNC XrLirStatus xr_lir_record_type(XrLirModule *, const XrLirRecordField *,
    uint32_t count, XrLirType *output);
XR_FUNC XrLirStatus xr_lir_record_fields(const XrLirModule *, XrLirType,
    const XrLirRecordField **, uint32_t *count);
XR_FUNC bool xr_lir_boxed_type(const XrLirModule *, XrLirType);
/* POD queries accept a live, unfailed construction or a finished module.
 * They preserve output on failure. Capabilities retain their dedicated ABI
 * and have no ordinary storage layout or scalar-leaf projection here. */
XR_FUNC XrLirStatus xr_lir_type_view(const XrLirModule *, XrLirType, XrLirTypeView *);
XR_FUNC XrLirStatus xr_lir_type_layout(const XrLirModule *, XrLirType, XrLirTypeLayout *);
XR_FUNC XrLirStatus xr_lir_type_scalar(const XrLirModule *, XrLirType, uint32_t ordinal,
    XrLirScalarPart *);
/* Cross-owner transport imports structure into the receiving pool. A numeric
 * ordinal alone does not establish correspondence between two modules. */
XR_FUNC XrLirStatus xr_lir_import_type(XrLirModule *target, const XrLirModule *source,
    XrLirType type, XrLirType *output);
/* Copy a finished module's complete pool into an empty receiving pool before
 * copying registry entries or signatures. The copied ordinals remain exact. */
XR_FUNC XrLirStatus xr_lir_copy_types(XrLirModule *target, const XrLirModule *source);

/* FIXED_ARRAY_NEW has its exact constructed result type and exactly count
 * U8 operands. FIXED_ARRAY_GET returns U8 from a fixed value and signed I64
 * index. Negative and out-of-range indices fail at that instruction's source,
 * including when its result is unused. Both ops have zero bits/callee/intrinsic.
 * Length is an I64 constant derived from this type, after receiver evaluation.
 * Such a fold preserves the receiver's calls, state reads and possible traps. */

#endif // XLIR_TYPES_H
