/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhir_lexical.inc.c - Read the same owned scope and binding declarations
 */
XR_FUNC bool xr_hir_body_lexical_counts(const XrHirBody *body, uint32_t *scopes,
    uint32_t *bindings) {
    if (!body || !scopes || !bindings) return false;
    *scopes = body->scopes.count;
    *bindings = body->binding_count;
    return true;
}

XR_FUNC bool xr_hir_body_lexical_scope(const XrHirBody *body, uint32_t scope,
    XrHirBodyScope *parent, XrHirBodyOrigin *origin) {
    if (!body || scope >= body->scopes.count || !parent || !origin) return false;
    const HirBodyScope row = ((const HirBodyScope *)body->scopes.items)[scope];
    *parent = row.parent;
    *origin = row.origin;
    return true;
}

XR_FUNC bool xr_hir_body_lexical_binding(const XrHirBody *body, uint32_t binding,
    XrHirLexicalBinding *output) {
    if (!body || binding >= body->binding_count || !output) return false;
    const HirBodyBinding *row = &body->bindings[binding];
    *output = (XrHirLexicalBinding){row->declaration, row->lexical_value,
        row->lexical_role, row->materialized};
    return true;
}

XR_FUNC bool xr_hir_body_lexical_use(const XrHirBody *body, XrHirBodyUse use,
    XrHirLexicalUse *output) {
    if (!body || use.owner != body || use.index >= body->count || !output) return false;
    const HirBodyUse *row = &body->uses[use.index];
    *output = (XrHirLexicalUse){row->binding, row->binding_kind,
        row->action.operation, row->has_second_operand ? row->second_operand : UINT32_MAX,
        row->initializes};
    return true;
}
