/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhir_lexical.h - Numeric observations of the actual typed body owner
 */
#ifndef XHIR_LEXICAL_H
#define XHIR_LEXICAL_H
#include "xhir_body.h"
#include "../xir/xxir_lexical.h"

typedef struct XrHirLexicalBinding {
    XrHirBindingInput declaration;
    uint32_t value;
    XirLexicalBindingRole role;
    bool materialized;
} XrHirLexicalBinding;

typedef struct XrHirLexicalUse {
    XrHirBodyBinding binding;
    XrHirBindingUseKind kind;
    XrXirOp operation;
    uint32_t incoming;
    bool initializes;
} XrHirLexicalUse;

XR_FUNC bool xr_hir_body_lexical_counts(const XrHirBody *, uint32_t *scopes,
    uint32_t *bindings);
XR_FUNC bool xr_hir_body_lexical_scope(const XrHirBody *, uint32_t,
    XrHirBodyScope *parent, XrHirBodyOrigin *origin);
XR_FUNC bool xr_hir_body_lexical_binding(const XrHirBody *, uint32_t,
    XrHirLexicalBinding *output);
XR_FUNC bool xr_hir_body_lexical_use(const XrHirBody *, XrHirBodyUse,
    XrHirLexicalUse *output);
#endif // XHIR_LEXICAL_H
