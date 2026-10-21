/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_builder.h - Copying construction of scalar SSA modules
 *
 * KEY CONCEPT:
 *   Every borrowed input is copied before return. The first construction
 *   failure prevents publication, so callers cannot execute partial modules.
 */
#ifndef XLIR_BUILDER_H
#define XLIR_BUILDER_H
#include "xlir.h"

typedef struct XrLirRegistry {
    const XrLirIntrinsic *entries;
    uint32_t count;
} XrLirRegistry;
typedef struct XrLirFunctionSpec {
    const char *name;
    size_t name_length;
    const XrLirType *parameters;
    uint32_t parameter_count;
    XrLirType result;
    XrLirFunctionKind kind;
    XrLirExternKind extern_kind;
} XrLirFunctionSpec;
typedef struct XrLirEdge {
    XrLirBlock target;
    const XrLirValue *arguments;
    uint32_t argument_count;
} XrLirEdge;
typedef struct XrLirInstruction {
    XrLirOp op;
    XrLirType type;
    XrLirPredicate predicate;
    const XrLirValue *operands;
    uint32_t operand_count;
    uint64_t bits;
    XrLirFunction callee;
    uint32_t intrinsic;
    XrLirEdge edges[2];
    uint32_t edge_count;
    XrLirSource source;
} XrLirInstruction;

/* Creation requires an empty output. NULL registry means no intrinsics.
 * The module owns a snapshot of every supplied intrinsic signature. */
XR_FUNC XrLirStatus xr_lir_module_create(const XrLirRegistry *, XrLirModule **output);
XR_FUNC void xr_lir_module_destroy(XrLirModule *module);
XR_FUNC XrLirStatus xr_lir_declare_function(XrLirModule *, const XrLirFunctionSpec *,
                                            XrLirFunction *);
XR_FUNC XrLirStatus xr_lir_define_function(XrLirModule *, XrLirFunction);
XR_FUNC XrLirStatus xr_lir_function_parameter(XrLirModule *, XrLirFunction, uint32_t ordinal,
                                              XrLirValue *);
XR_FUNC XrLirStatus xr_lir_add_block(XrLirModule *, XrLirFunction, const XrLirType *,
                                     uint32_t count, XrLirBlock *);
XR_FUNC XrLirStatus xr_lir_block_parameter(XrLirModule *, XrLirBlock, uint32_t ordinal,
                                           XrLirValue *);
XR_FUNC XrLirStatus xr_lir_set_entry(XrLirModule *, XrLirFunction, XrLirBlock);
XR_FUNC XrLirStatus xr_lir_emit_value(XrLirModule *, XrLirBlock, const XrLirInstruction *,
                                      XrLirValue *);
XR_FUNC XrLirStatus xr_lir_emit_effect(XrLirModule *, XrLirBlock, const XrLirInstruction *);
XR_FUNC XrLirStatus xr_lir_set_terminator(XrLirModule *, XrLirBlock, const XrLirInstruction *);
/* Every return initializes the optional diagnostic. Failed modules preserve
 * their first error; diagnostic source paths borrow the module until destroy. */
XR_FUNC XrLirStatus xr_lir_module_finish(XrLirModule *, XrLirDiagnostic *);

#endif  // XLIR_BUILDER_H
