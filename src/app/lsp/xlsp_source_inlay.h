/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef XLSP_SOURCE_INLAY_H
#define XLSP_SOURCE_INLAY_H
#include "xlsp_source_query.h"
#include "xlsp_source_syntax.h"
typedef struct XlspSourceTypeHint {
    XrLspPosition position;
    char *label;
    size_t label_length;
} XlspSourceTypeHint;
typedef struct XlspSourceTypeHints {XlspSourceTypeHint *items;size_t count;} XlspSourceTypeHints;
/* Checked inferred bindings only. Labels and positions use owned Source facts;
 * no AST inference, synthesized generic names or effect proof participates.
 * Range endpoints are strict UTF-16; hints lie in [start,end). An empty range
 * yields no hints. Failure preserves output. Each allocation pins the original
 * compiler ledger and survives snapshot, Session and editor-input death. */
XR_FUNC XrXirStatus xlsp_source_type_hints(const XlspSourceSnapshot *,const char *,size_t,
    XrLspRange,XlspSourceTypeHints *);
XR_FUNC void xlsp_source_type_hints_free(XlspSourceTypeHints *);
typedef struct XlspSourceParameterHint {
    XrLspPosition position;
    XlspSourceHover label,target_identity,target_path;
    XrFingerprint target_fingerprint;
    XrXirSourceRange target_range;
    uint32_t target_native_identity,mode;
    XrXirSourceDeclarationKind target_kind;
} XlspSourceParameterHint;
typedef struct XlspSourceParameterHints {XlspSourceParameterHint *items;size_t count;} XlspSourceParameterHints;
/* Actual checked call identities and declared parameters supply labels/modes.
 * Syntax facts must belong to the same original ledger and exact document,
 * content and version. Unknown callable identities remain explicit failures.
 * Identical variable argument/parameter spellings omit a redundant label.
 * Owned provenance distinguishes same-named targets across root snapshots;
 * memory-only targets have no invented path or editor URI. Output survives
 * every producer and failure preserves the caller's empty output. */
XR_FUNC XrXirStatus xlsp_source_parameter_hints(const XlspSourceSnapshot *,const XlspSourceCallArguments *,
    XrLspRange,XlspSourceParameterHints *);
XR_FUNC void xlsp_source_parameter_hints_free(XlspSourceParameterHints *);
#endif
