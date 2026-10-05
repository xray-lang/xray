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
#include "xxir_compile_context.h"
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
    XR_XIR_BAD_LAYOUT,
    XR_XIR_IO,
    XR_XIR_UNRESOLVED
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
    uint32_t path_count;
} XrXirFunctionLayout;

typedef struct XrXirInstruction {
    XrXirOp op;
    XrXirType type;
    uint32_t args[2];
    uint32_t targets[2];
    int64_t immediate;
    uint32_t type_arguments[2];
} XrXirInstruction;

/* A nonzero panic names the handler block entered when an instruction of this
 * block, or a callee of one of its calls, raises a panic-channel fault. */
typedef struct XrXirBlock {
    uint32_t first;
    uint32_t count;
    uint32_t panic;
    /* Zero or one plus the active CLEANUP_REGISTER instruction index. */
    uint32_t frontier;
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

#define XR_XIR_CONSTRAINT_SENDABLE 1u
#define XR_XIR_CONSTRAINT_ERROR 2u
#define XR_XIR_CONSTRAINT_EQUAL 4u
#define XR_XIR_CONSTRAINT_MASK (XR_XIR_CONSTRAINT_SENDABLE | XR_XIR_CONSTRAINT_ERROR | XR_XIR_CONSTRAINT_EQUAL)
typedef struct XrXirInterfaceApplication XrXirInterfaceApplication;
typedef struct XrXirConstraint {
    uint32_t markers;
    const XrXirInterfaceApplication *interfaces;
    uint32_t interface_count;
} XrXirConstraint;

typedef enum XrXirTypeBinderKind {
    XR_XIR_BINDER_TYPE = 0,
    XR_XIR_BINDER_RESULT_VARIABLE = 1
} XrXirTypeBinderKind;
typedef struct XrXirGeneric {
    const XrXirConstraint *constraints;
    uint32_t parameter_count;
    const XrXirType *arguments;
    uint32_t argument_count;
    const uint32_t *parameter_kinds;
} XrXirGeneric;
static inline uint32_t xr_xir_binder_kind(const XrXirGeneric *record, uint32_t parameter) {
    return record && record->parameter_kinds && parameter < record->parameter_count ?
        record->parameter_kinds[parameter] : XR_XIR_BINDER_TYPE;
}

typedef struct XrXirCallableParameter { XrXirType type; uint32_t mode; } XrXirCallableParameter;

#define XR_XIR_CALLABLE_NO_SUSPEND 1u
typedef enum XrXirTypeKind {
    XR_XIR_TYPE_CALLABLE = 1,
    XR_XIR_TYPE_ARRAY = 2,
    XR_XIR_TYPE_CELL = 3,
    XR_XIR_TYPE_NOMINAL = 4,
    XR_XIR_TYPE_NULLABLE = 5,
    XR_XIR_TYPE_TUPLE = 6
} XrXirTypeKind;
typedef struct XrXirNominalType {
    uint32_t declaration;
    const XrXirType *arguments;
    uint32_t argument_count;
    const XrXirType *fields;
    uint32_t field_count;
} XrXirNominalType;
typedef struct XrXirTypeNode {
    uint32_t kind;
    XrXirType element;
    const XrXirCallableParameter *parameters;
    uint32_t parameter_count;
    XrXirType result;
    uint32_t flags;
    uint32_t parameter_span;
    XrXirNominalType nominal;
} XrXirTypeNode;
typedef struct XrXirNominalTable XrXirNominalTable;
typedef struct XrXirInterfaceTable XrXirInterfaceTable;

typedef struct XrXirTypes {
    const XrXirTypeNode *nodes;
    uint32_t count;
    const XrXirNominalTable *nominals;
    const XrXirInterfaceTable *interfaces;
} XrXirTypes;

typedef enum XrXirLinkageKind {
    XR_XIR_PROGRAM, XR_XIR_LIBRARY
} XrXirLinkageKind;

typedef enum XrXirDefaultOwnerKind {
    XR_XIR_DEFAULT_PARAMETER = 0
} XrXirDefaultOwnerKind;

typedef struct XrXirDefaultBinding {
    uint32_t owner_kind;
    uint32_t owner;
    uint32_t ordinal;
    uint32_t function;
} XrXirDefaultBinding;

typedef struct XrXirDefaultTable {
    const XrXirDefaultBinding *records;
    uint32_t count;
} XrXirDefaultTable;

typedef struct XrXirProvenance XrXirProvenance;
typedef struct XrXirModule {
    XrXirStage stage;
    const XrXirFunction *functions;
    uint32_t function_count;
    const XrXirDeclarations *declarations;
    const XrXirGeneric *generics;
    const XrXirTypes *types;
    const XrXirProvenance *provenance;
    XrXirLinkageKind linkage_kind;
    const XrXirDefaultTable *defaults;
} XrXirModule;


typedef enum XrXirDiagnosticReason {
    XR_XIR_DIAGNOSTIC_NONE, XR_XIR_DIAGNOSTIC_CLEANUP_THROW, XR_XIR_DIAGNOSTIC_CLEANUP_SUSPEND,
    XR_XIR_DIAGNOSTIC_UNINITIALIZED_READ, XR_XIR_DIAGNOSTIC_READONLY_WRITE,
    XR_XIR_DIAGNOSTIC_NO_SUSPEND
} XrXirDiagnosticReason;

typedef struct XrXirDiagnostic {
    XrXirStatus status;
    uint32_t function;
    uint32_t block;
    uint32_t instruction;
    XrXirDiagnosticReason reason;
} XrXirDiagnostic;

typedef struct XrXirArtifact XrXirArtifact;


XR_FUNC const char *xr_xir_op_name(XrXirOp op);
XR_FUNC XrXirStatus xr_xir_compile_defaults_verify(const XrXirCompileContext *compile_context, const XrXirModule *module);
XR_FUNC XrXirStatus xr_xir_compile_default_lookup(const XrXirCompileContext *compile_context, const XrXirModule *module, uint32_t owner, uint32_t ordinal, const XrXirDefaultBinding **binding);
XR_FUNC XrXirStatus xr_xir_compile_verify(const XrXirCompileContext *compile_context, const XrXirModule *module, XrXirDiagnostic *diagnostic);
XR_FUNC XrXirStatus xr_xir_compile_check(const XrXirCompileContext *compile_context, const XrXirModule *built, XrXirArtifact **output, XrXirDiagnostic *diagnostic);
XR_FUNC XrXirStatus xr_xir_compile_lower(const XrXirArtifact *checked, const XrXirTarget *target, XrXirArtifact **output, XrXirDiagnostic *diagnostic);
XR_FUNC const XrXirCompileContext *xr_xir_compile_artifact_context(const XrXirArtifact *artifact);
XR_FUNC const XrXirModule *xr_xir_compile_artifact_module(const XrXirArtifact *artifact);
XR_FUNC const XrXirTarget *xr_xir_compile_artifact_target(const XrXirArtifact *artifact);
XR_FUNC const XrXirFunctionLayout *xr_xir_compile_artifact_layout(const XrXirArtifact *artifact, uint32_t function);
XR_FUNC XrXirStatus xr_xir_builtin_layout(XrXirType type, const XrXirTarget *target,
    XrXirLayoutContext context, XrXirLayout *layout);
XR_FUNC XrXirStatus xr_xir_compile_layout(const XrXirCompileContext *compile_context, const XrXirTypes *types, XrXirType type, const XrXirTarget *target,
                                XrXirLayoutContext context, XrXirLayout *layout);
XR_FUNC XrXirStatus xr_xir_compile_artifact_verify(const XrXirArtifact *artifact, XrXirDiagnostic *diagnostic);
XR_FUNC void xr_xir_compile_artifact_free(XrXirArtifact *artifact);
XR_FUNC XrXirStatus xr_xir_compile_declarations_verify(const XrXirCompileContext *compile_context, const XrXirDeclarations *declarations, const XrXirTypes *types, uint32_t functions, XrXirLinkageKind kind);
/* Module-owned signatures follow descriptor/identity structure admission. */
XR_FUNC XrXirStatus xr_xir_compile_method_signature_verify(const XrXirCompileContext *compile_context, const XrXirModule *module, uint32_t function);
/* Internal snapshot helpers require successful declaration verification first. */
XR_FUNC XrXirStatus xr_xir_compile_declarations_order(const XrXirCompileContext *compile_context, const XrXirDeclarations *declarations, uint32_t *order);
XR_FUNC XrXirStatus xr_xir_compile_declarations_clone(const XrXirCompileContext *compile_context, const XrXirDeclarations *source, uint32_t functions, XrXirDeclarations **output);
XR_FUNC void xr_xir_compile_declarations_free(XrXirDeclarations *declarations);
XR_FUNC bool xr_xir_module_imports(const XrXirDeclarations *declarations, uint32_t from, uint32_t target);

#endif // XXIR_H
