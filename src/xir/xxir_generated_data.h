/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_generated_data.h - Ordinary value tables into unchecked function data
 *
 * KEY CONCEPT:
 *   Logical references resolve only in a receiving compiler scope. Decoding
 *   publishes owned data, never Checked code or declaration authority.
 */
#ifndef XXIR_GENERATED_DATA_H
#define XXIR_GENERATED_DATA_H
#include "xxir.h"
#include "xxir_value.h"
typedef struct XrXirGeneratedData XrXirGeneratedData;
typedef struct XrXirGeneratedDataRequest {
    /* Authentic ordinary Value and receiving canonical scope, borrowed only
     * during decoding. Callees are selected by the receiver, not by names in
     * the output. Every decoded type ID still borrows this scope until the
     * ordinary generated Built owner independently copies the whole module. */
    const XrXirValue *value;
    const XrXirTypes *types;
    const XrXirType *references;
    uint32_t reference_count;
    const uint32_t *callees;
    uint32_t callee_count;
    uint32_t first_function,first_literal,field_count,method_count;
} XrXirGeneratedDataRequest;
typedef struct XrXirGeneratedDataView {
    const XrXirFunction *functions;
    const XrXirGeneric *generics;
    const uint32_t *members,*roots;
    const int32_t *instruction_fields;
    uint32_t function_count,instruction_count;
    const XrXirLiteral *literals;
    uint32_t literal_count;
} XrXirGeneratedDataView;
/* Version1 is a Tuple of version, functions, parameters, blocks, instructions,
 * operands, literals and type arguments. Every table is a flat ordinary Array.
 * Nonnegative type references index the supplied canonical map; negative ones
 * name builtin (-reference-1). Calls name new functions or supplied callees by
 * the same sign convention; CONST_STRING names a new literal ordinal.
 * Names are labels, not source exports or owner claims. member/root and field
 * selectors must still bind to an authentic definition policy in the receiver.
 * The complete ordinary Built checker remains mandatory before publication. */
XR_FUNC XrXirStatus xr_xir_compile_generated_data_decode(const XrXirCompileContext *,
    const XrXirGeneratedDataRequest *,XrXirGeneratedData **);
XR_FUNC const XrXirGeneratedDataView *xr_xir_compile_generated_data_view(const XrXirGeneratedData *);
XR_FUNC void xr_xir_compile_generated_data_free(XrXirGeneratedData *);
#endif // XXIR_GENERATED_DATA_H
