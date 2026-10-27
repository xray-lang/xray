/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_scalar_core.h - One numeric and predecessor-aware instruction kernel
 *
 * KEY CONCEPT:
 *   Compiler and runtime use the same actual values and target frame offsets.
 */
#ifndef XXIR_SCALAR_CORE_H
#define XXIR_SCALAR_CORE_H
#include "xxir.h"
#include "xxir_scalar.h"
typedef struct XirScalarCoreRun {
    const XrXirModule *module;
    const XrXirFunction *function;
    const XrXirFunctionLayout *layout;
    void *frame;
    const XrXirTypeArena *arena;
    XrCompileResources *compile_resources;
} XirScalarCoreRun;
typedef enum XirScalarCoreActionKind { XIR_SCALAR_CONTINUE, XIR_SCALAR_RETURN } XirScalarCoreActionKind;
typedef struct XirScalarCoreAction { XirScalarCoreActionKind kind;XrXirValue value; } XirScalarCoreAction;
/* Dispatch classification grants no execution permission. Both consumers admit
 * the complete current Artifact and the real function/layout first. */
static inline bool xir_scalar_core_opcode(XrXirOp op) {
    switch (op) {
    case XR_XIR_CONST_RUNE:case XR_XIR_CONST_BOOL:case XR_XIR_CONST_FLOAT:case XR_XIR_CONST_INT:
    case XR_XIR_LOCAL_UNINIT:case XR_XIR_SCALAR_LOCAL_NEW:case XR_XIR_SCALAR_LOCAL_READ:
    case XR_XIR_SCALAR_COPY:case XR_XIR_SCALAR_LOCAL_WRITE:case XR_XIR_OWNED_LOCAL_WRITE:
    case XR_XIR_ADD_INT:case XR_XIR_SUB_INT:case XR_XIR_MUL_INT:case XR_XIR_DIV_INT:case XR_XIR_REM_INT:
    case XR_XIR_AND_INT:case XR_XIR_OR_INT:case XR_XIR_XOR_INT:case XR_XIR_SHL_INT:case XR_XIR_SHR_INT:
    case XR_XIR_EQ_INT:case XR_XIR_NE_INT:case XR_XIR_LT_INT:case XR_XIR_LE_INT:case XR_XIR_GT_INT:case XR_XIR_GE_INT:
    case XR_XIR_RUNE_TO_INTEGER:case XR_XIR_INTEGER_TO_RUNE:case XR_XIR_CONVERT_NUMBER:
    case XR_XIR_JUMP:case XR_XIR_BRANCH:case XR_XIR_PHI:case XR_XIR_RETURN:return true;
    default:return false;
    }
}
/* Generic runtime equality keeps its own arena/domain admission. Only its
 * canonical bool/integer/Rune leaf operation shares this numeric implementation. */
XR_FUNC XrXirRunStatus xir_scalar_core_equal(XrXirType type,int64_t left,int64_t right,bool *output);
XR_FUNC XrXirRunStatus xir_scalar_core_edge(XirScalarCoreRun *run,uint32_t instruction,uint32_t target);
XR_FUNC XrXirRunStatus xir_scalar_core_step(XirScalarCoreRun *run,uint32_t *instruction,XirScalarCoreAction *action);
#endif // XXIR_SCALAR_CORE_H
