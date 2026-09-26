/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir.h - Stage-aware typed control flow and owned compiler artifacts
 *
 * KEY CONCEPT:
 *   Borrowed construction views never become executable by changing a tag.
 *   Checked and lowered artifacts own their metadata and validate transitions.
 */

#ifndef XXIR_H
#define XXIR_H

#include "xxir_scalar.h"
#include "xxir_declarations.h"

typedef enum XrXirStage {
    XR_XIR_BUILT = 1,
    XR_XIR_CHECKED = 2,
    XR_XIR_LOWERED = 4
} XrXirStage;

typedef enum XrXirOp {
    XR_XIR_INVALID,
#define XR_XIR_OP(name, stages, rule, operands, edges, terminal) XR_XIR_##name,
#include "xxir_ops.def"
#undef XR_XIR_OP
    XR_XIR_OP_COUNT
} XrXirOp;

typedef enum XrXirStatus {
    XR_XIR_OK,
    XR_XIR_BAD_STRUCTURE,
    XR_XIR_BAD_STAGE,
    XR_XIR_BAD_TYPE,
    XR_XIR_BAD_VALUE,
    XR_XIR_BAD_DOMINANCE,
    XR_XIR_BUDGET,
    XR_XIR_OUT_OF_MEMORY,
    XR_XIR_BAD_LAYOUT
} XrXirStatus;

typedef struct XrXirTarget {
    uint32_t architecture;
    uint32_t abi_version;
} XrXirTarget;

typedef enum XrXirLayoutContext {
    XR_XIR_LAYOUT_STORAGE = 1,
    XR_XIR_LAYOUT_SSA,
    XR_XIR_LAYOUT_PARAMETER,
    XR_XIR_LAYOUT_RESULT,
    XR_XIR_LAYOUT_BOXED,
    XR_XIR_LAYOUT_FRAME
} XrXirLayoutContext;

typedef struct XrXirLayout {
    uint32_t size;
    uint32_t alignment;
} XrXirLayout;

/* PHI offsets name the result followed by a private eight-byte snapshot slot.
 * Owned snapshot offsets participate in the same exhaustive cleanup list. */
typedef struct XrXirFunctionLayout {
    uint32_t slot_count;
    uint32_t frame_bytes;
    const uint32_t *offsets;
    const XrXirLayout *parameters;
    XrXirLayout result;
    uint32_t owned_count;
    const uint32_t *owned_offsets;
    uint32_t outgoing_count;
} XrXirFunctionLayout;

typedef struct XrXirInstruction {
    XrXirOp op;
    XrXirType type;
    uint32_t args[2];
    uint32_t targets[2];
    int64_t immediate;
} XrXirInstruction;

typedef struct XrXirBlock {
    uint32_t first;
    uint32_t count;
} XrXirBlock;

/* Value IDs are parameters followed by instruction indices. Terminators occupy
 * indices but never define usable values. All unused instruction fields are zero. */
typedef struct XrXirFunction {
    const char *name;
    uint32_t name_length;
    const XrXirType *parameters;
    uint32_t parameter_count;
    XrXirType result;
    const XrXirBlock *blocks;
    uint32_t block_count;
    const XrXirInstruction *instructions;
    uint32_t instruction_count;
    const uint32_t *operands;
    uint32_t operand_count;
} XrXirFunction;

#define XR_XIR_TYPE_PARAMETER_BASE 65536u
#define XR_XIR_CONSTRAINT_SENDABLE 1u
typedef struct XrXirGeneric {
    const uint32_t *constraints;
    uint32_t parameter_count;
    const XrXirType *arguments;
    uint32_t argument_count;
} XrXirGeneric;

#define XR_XIR_CALLABLE_TYPE_BASE 256u
typedef struct XrXirCallableParameter { XrXirType type; uint32_t mode; } XrXirCallableParameter;
typedef struct XrXirCallableSignature {
    const XrXirCallableParameter *parameters;
    uint32_t parameter_count;
    XrXirType result;
    uint32_t flags;
} XrXirCallableSignature;
typedef struct XrXirCallableTypes {
    const XrXirCallableSignature *signatures;
    uint32_t count;
} XrXirCallableTypes;

typedef struct XrXirModule {
    XrXirStage stage;
    const XrXirFunction *functions;
    uint32_t function_count;
    const XrXirDeclarations *declarations;
    const XrXirGeneric *generics;
    const XrXirCallableTypes *callables;
} XrXirModule;

typedef struct XrXirBudget {
    uint32_t functions;
    uint32_t parameters;
    uint32_t blocks;
    uint32_t instructions;
    uint64_t metadata_bytes;
    uint64_t scratch_bytes;
    uint64_t work;
    uint64_t frame_bytes;
} XrXirBudget;

typedef struct XrXirDiagnostic {
    XrXirStatus status;
    uint32_t function;
    uint32_t block;
    uint32_t instruction;
} XrXirDiagnostic;

typedef struct XrXirArtifact XrXirArtifact;

XR_FUNC XrXirBudget xr_xir_default_budget(void);
XR_FUNC const char *xr_xir_op_name(XrXirOp op);
XR_FUNC XrXirStatus xr_xir_verify(const XrXirModule *module,
                                const XrXirBudget *budget, XrXirDiagnostic *diagnostic);
XR_FUNC XrXirStatus xr_xir_check(const XrXirModule *built, const XrXirBudget *budget,
                               XrXirArtifact **output, XrXirDiagnostic *diagnostic);
XR_FUNC XrXirStatus xr_xir_lower(const XrXirArtifact *checked, const XrXirTarget *target,
                               const XrXirBudget *budget,
                               XrXirArtifact **output, XrXirDiagnostic *diagnostic);
XR_FUNC const XrXirModule *xr_xir_artifact_module(const XrXirArtifact *artifact);
XR_FUNC const XrXirTarget *xr_xir_artifact_target(const XrXirArtifact *artifact);
XR_FUNC const XrXirFunctionLayout *xr_xir_artifact_layout(const XrXirArtifact *artifact,
                                                       uint32_t function);
XR_FUNC XrXirStatus xr_xir_layout(XrXirType type, const XrXirTarget *target,
                                XrXirLayoutContext context, XrXirLayout *layout);
XR_FUNC XrXirStatus xr_xir_artifact_verify(const XrXirArtifact *artifact,
                                        const XrXirBudget *budget, XrXirDiagnostic *diagnostic);
XR_FUNC void xr_xir_artifact_free(XrXirArtifact *artifact);
XR_FUNC XrXirStatus xr_xir_declarations_verify(const XrXirDeclarations *declarations,
    uint32_t functions, uint64_t *bytes, uint64_t *work);
/* Internal snapshot helpers require successful declaration verification first. */
XR_FUNC XrXirStatus xr_xir_declarations_order(const XrXirDeclarations *declarations,
    uint32_t *order, uint64_t *work);
XR_FUNC XrXirStatus xr_xir_declarations_clone(const XrXirDeclarations *source,
    uint32_t functions, XrXirDeclarations **output);
XR_FUNC void xr_xir_declarations_free(XrXirDeclarations *declarations);
XR_FUNC bool xr_xir_module_imports(const XrXirDeclarations *declarations, uint32_t from, uint32_t target);

#endif // XXIR_H
