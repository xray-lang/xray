/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_typed_program.h - Immutable analyzer publication consumed by Xi lowering
 */

#ifndef XA_TYPED_PROGRAM_H
#define XA_TYPED_PROGRAM_H

#include "../../base/xdefs.h"
#include <stdbool.h>
#include <stdint.h>

struct AstNode;
struct XaAnalyzer;
struct XgGlobalEvidence;
struct XaResolvedCall;
struct XaSymbol;
struct XaEffectSummary;
struct XaMemoryEffectSummary;

typedef struct XaTypedProgram XaTypedProgram;

typedef enum XaTypedProgramReason {
    XA_TYPED_PROGRAM_REASON_NONE = 0,
    XA_TYPED_PROGRAM_REASON_INVALID_INPUT,
    XA_TYPED_PROGRAM_REASON_ANALYZER_ERROR,
    XA_TYPED_PROGRAM_REASON_RECOVERY_POISON,
    XA_TYPED_PROGRAM_REASON_STALE_REVISION,
} XaTypedProgramReason;

typedef struct XaTypedProgramPublishResult {
    XaTypedProgram *program;
    XaTypedProgramReason reason;
    uint32_t source_line;
    const char *detail;
} XaTypedProgramPublishResult;

/* Publish one immutable semantic snapshot. The syntax and analyzer-owned
 * semantic sidecars must outlive the returned program. */
XR_FUNC XaTypedProgramPublishResult
xa_typed_program_publish(struct XaAnalyzer *analyzer, const struct AstNode *syntax,
                         const struct XgGlobalEvidence *global_evidence, uint32_t module_id);
XR_FUNC void xa_typed_program_free(XaTypedProgram *program);

XR_FUNC bool xa_typed_program_is_verified(const XaTypedProgram *program);
XR_FUNC bool xa_typed_program_is_current(const XaTypedProgram *program);
XR_FUNC uint64_t xa_typed_program_semantic_revision(const XaTypedProgram *program);
XR_FUNC const struct AstNode *xa_typed_program_syntax(const XaTypedProgram *program);
XR_FUNC struct XaAnalyzer *xa_typed_program_semantics(const XaTypedProgram *program);
XR_FUNC const struct XgGlobalEvidence *
xa_typed_program_global_evidence(const XaTypedProgram *program);
XR_FUNC uint32_t xa_typed_program_module_id(const XaTypedProgram *program);
XR_FUNC const struct XaResolvedCall *
xa_typed_program_resolved_call(const XaTypedProgram *program, const struct AstNode *call_node);
XR_FUNC const struct XaEffectSummary *
xa_typed_program_effect_summary(const XaTypedProgram *program, const struct XaSymbol *symbol);
XR_FUNC const struct XaMemoryEffectSummary *
xa_typed_program_memory_effect_summary(const XaTypedProgram *program,
                                       const struct XaSymbol *symbol);
XR_FUNC const char *xa_typed_program_reason_name(XaTypedProgramReason reason);

#endif  // XA_TYPED_PROGRAM_H
