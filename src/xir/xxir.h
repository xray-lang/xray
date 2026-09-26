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

typedef struct XrXirFunctionLayout {
    uint32_t slot_count;
    uint32_t frame_bytes;
    const uint32_t *offsets;
    const XrXirLayout *parameters;
    XrXirLayout result;
    uint32_t owned_count;
    const uint32_t *owned_offsets;
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
} XrXirFunction;

typedef struct XrXirModule {
    XrXirStage stage;
    const XrXirFunction *functions;
    uint32_t function_count;
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

#endif // XXIR_H
