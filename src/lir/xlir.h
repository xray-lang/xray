/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir.h - Owned scalar SSA and immutable backend queries
 *
 * KEY CONCEPT:
 *   Construction handles identify their live module and function. Published
 *   views borrow immutable storage and never prescribe an executor's frames.
 */
#ifndef XLIR_H
#define XLIR_H

#include "../base/xdefs.h"

typedef struct XrLirModule XrLirModule;
typedef enum XrLirStatus {
    XR_LIR_OK,
    XR_LIR_OUT_OF_MEMORY,
    XR_LIR_INVALID_ARGUMENT,
    XR_LIR_INVALID_STATE,
    XR_LIR_UNSUPPORTED,
    XR_LIR_VERIFY_ERROR,
    XR_LIR_OUTPUT_ERROR
} XrLirStatus;
typedef enum XrLirType {
    XR_LIR_TYPE_INVALID,
    XR_LIR_UNIT,
    XR_LIR_I1,
    XR_LIR_I8,
    XR_LIR_I16,
    XR_LIR_I32,
    XR_LIR_I64,
    XR_LIR_U8,
    XR_LIR_U16,
    XR_LIR_U32,
    XR_LIR_U64,
    XR_LIR_F32,
    XR_LIR_F64,
    XR_LIR_PTR,
    XR_LIR_I64_CELL,
    XR_LIR_TYPE_COUNT
} XrLirType;
typedef enum XrLirOp {
    XR_LIR_OP_INVALID,
#define XR_LIR_OP(name, text, arity) XR_LIR_##name,
#include "xlir_ops.def"
#undef XR_LIR_OP
    XR_LIR_OP_COUNT
} XrLirOp;
typedef enum XrLirPredicate {
    XR_LIR_PREDICATE_NONE,
    XR_LIR_EQ,
    XR_LIR_NE,
    XR_LIR_LT,
    XR_LIR_LE,
    XR_LIR_GT,
    XR_LIR_GE
} XrLirPredicate;
typedef enum XrLirFunctionKind {
    XR_LIR_FUNCTION_DEFINITION,
    XR_LIR_FUNCTION_EXTERNAL
} XrLirFunctionKind;
typedef enum XrLirExternKind {
    XR_LIR_EXTERN_NONE,
    XR_LIR_EXTERN_PRINT_I64
} XrLirExternKind;

/* Handles are valid only while their module is alive. Zero IDs are invalid. */
typedef struct XrLirFunction {
    const XrLirModule *module;
    uint32_t id;
} XrLirFunction;
typedef struct XrLirBlock {
    const XrLirModule *module;
    uint32_t function, id;
} XrLirBlock;
typedef struct XrLirValue {
    const XrLirModule *module;
    uint32_t function, id;
} XrLirValue;

typedef struct XrLirSource {
    const char *path;
    size_t path_length;
    uint32_t line, column, end_line, end_column;
} XrLirSource;
typedef struct XrLirDiagnostic {
    XrLirStatus status;
    uint32_t function, block, instruction, edge;
    XrLirSource source;
    char message[192];
} XrLirDiagnostic;

typedef struct XrLirIntrinsic {
    uint32_t id;
    const XrLirType *parameters;
    uint32_t parameter_count;
    XrLirType result;
    bool scalar_sync;
} XrLirIntrinsic;
typedef struct XrLirFunctionView {
    const char *name;
    size_t name_length;
    const XrLirType *parameter_types;
    const uint32_t *parameters;
    uint32_t parameter_count, block_count, value_count, entry;
    XrLirType result;
    XrLirFunctionKind kind;
    XrLirExternKind extern_kind;
} XrLirFunctionView;
typedef struct XrLirBlockView {
    const uint32_t *parameters;
    uint32_t parameter_count, instruction_count;
} XrLirBlockView;
typedef struct XrLirEdgeView {
    uint32_t target;
    const uint32_t *arguments;
    uint32_t argument_count;
} XrLirEdgeView;
typedef struct XrLirInstructionView {
    XrLirOp op;
    XrLirType type;
    XrLirPredicate predicate;
    uint32_t result, callee, intrinsic;
    uint64_t bits;
    const uint32_t *operands;
    uint32_t operand_count, edge_count;
    XrLirEdgeView edges[2];
    XrLirSource source;
} XrLirInstructionView;

/* Queries require a finished module. Out-of-range queries preserve no view.
 * Local IDs in views are one-based within the indicated function. All borrowed
 * arrays, names and paths remain valid until module destruction. */
/* A non-NULL module must be live. Readability permits views, not execution or
 * verified SSA; NULL, unfinished and failed modules are not readable. */
XR_FUNC bool xr_lir_module_is_readable(const XrLirModule *module);
XR_FUNC uint32_t xr_lir_function_count(const XrLirModule *module);
/* Intrinsic signatures include unused entries. Query parameter slices borrow
 * the finished module; a receiving builder must copy them before it dies. */
XR_FUNC uint32_t xr_lir_intrinsic_count(const XrLirModule *module);
XR_FUNC XrLirStatus xr_lir_intrinsic_at(const XrLirModule *, uint32_t ordinal, XrLirIntrinsic *);
XR_FUNC XrLirStatus xr_lir_intrinsic_view(const XrLirModule *, uint32_t id, XrLirIntrinsic *);
XR_FUNC XrLirStatus xr_lir_function_at(const XrLirModule *, uint32_t ordinal, XrLirFunction *);
XR_FUNC XrLirStatus xr_lir_function_view(const XrLirModule *, XrLirFunction, XrLirFunctionView *);
XR_FUNC XrLirStatus xr_lir_block_at(const XrLirModule *, XrLirFunction, uint32_t ordinal,
                                    XrLirBlock *);
XR_FUNC XrLirStatus xr_lir_block_view(const XrLirModule *, XrLirBlock, XrLirBlockView *);
XR_FUNC XrLirStatus xr_lir_instruction_view(const XrLirModule *, XrLirBlock, uint32_t ordinal,
                                            XrLirInstructionView *);
XR_FUNC XrLirStatus xr_lir_value_type(const XrLirModule *, XrLirValue, XrLirType *);
XR_FUNC const char *xr_lir_type_name(XrLirType type);
XR_FUNC const char *xr_lir_op_name(XrLirOp op);

#endif  // XLIR_H
