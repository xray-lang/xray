/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef XLSP_SOURCE_INLAY_H
#define XLSP_SOURCE_INLAY_H
#include "xlsp_source_query.h"
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
#endif
