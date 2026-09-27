/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_type_match.inc.c - Bounded equality under explicit type substitution
 *
 * KEY CONCEPT:
 *   Field proofs and call proofs share one traversal of immutable type nodes.
 */
typedef struct TypeMatchFrame {
    const XrXirTypeNode *from, *to;
    uint32_t next;
} TypeMatchFrame;
typedef struct TypeMatchContext {
    const XrXirTypes *types;
    const XrXirType *arguments;
    uint32_t count;
    XrXirBudget *remaining;
} TypeMatchContext;
static XrXirStatus type_match_pair(TypeMatchContext *c, XrXirType expected,
                                  XrXirType actual, TypeMatchFrame *frame) {
    frame->from = NULL;
    if (!c->remaining->work) return XR_XIR_BUDGET;
    --c->remaining->work;
    uint32_t id = (uint32_t) expected;
    if (id >= XR_XIR_TYPE_PARAMETER_BASE && id < XR_XIR_TYPE_PARAMETER_LIMIT) {
        uint32_t index = id - XR_XIR_TYPE_PARAMETER_BASE;
        return index < c->count && c->arguments[index] == actual ? XR_XIR_OK : XR_XIR_BAD_TYPE;
    }
    const XrXirTypeNode *from = xr_xir_type_node(c->types, expected);
    if (!from || !from->parameter_span) return expected == actual ? XR_XIR_OK : XR_XIR_BAD_TYPE;
    const XrXirTypeNode *to = xr_xir_type_node(c->types, actual);
    if (!to || from->kind != to->kind || from->parameter_count != to->parameter_count || from->flags != to->flags)
        return XR_XIR_BAD_TYPE;
    if (from->kind != XR_XIR_TYPE_ARRAY && from->kind != XR_XIR_TYPE_CELL && from->kind != XR_XIR_TYPE_CALLABLE)
        return XR_XIR_BAD_TYPE;
    *frame = (TypeMatchFrame) {from, to, 0}; return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_xir_type_substitution_matches(const XrXirTypes *types,
    const XrXirType *arguments, uint32_t count, XrXirType expected,
    XrXirType actual, XrXirBudget *remaining) {
    if (!remaining || (count && !arguments)) return XR_XIR_BAD_STRUCTURE;
    TypeMatchContext c = {types, arguments, count, remaining};
    TypeMatchFrame root = {0};
    XrXirStatus status = type_match_pair(&c, expected, actual, &root);
    if (status != XR_XIR_OK || !root.from) return status;
    uint64_t bytes = (uint64_t) types->count * sizeof(TypeMatchFrame);
    if (bytes > SIZE_MAX || bytes > remaining->metadata_bytes) return XR_XIR_BUDGET;
    remaining->metadata_bytes -= bytes;
    TypeMatchFrame *stack = xr_malloc((size_t) bytes);
    if (!stack) return XR_XIR_OUT_OF_MEMORY;
    uint32_t depth = 1; stack[0] = root;
    while (depth && status == XR_XIR_OK) {
        TypeMatchFrame *frame = &stack[depth - 1];
        const XrXirTypeNode *from = frame->from, *to = frame->to;
        uint32_t components = from->kind == XR_XIR_TYPE_CALLABLE ? from->parameter_count + 1 : 1;
        if (frame->next == components) { --depth; continue; }
        XrXirType left = from->element, right = to->element;
        if (from->kind == XR_XIR_TYPE_CALLABLE) {
            if (!frame->next) { left = from->result; right = to->result; }
            else {
                uint32_t p = frame->next - 1;
                if (from->parameters[p].mode != to->parameters[p].mode) { status = XR_XIR_BAD_TYPE; break; }
                left = from->parameters[p].type; right = to->parameters[p].type;
            }
        }
        ++frame->next;
        TypeMatchFrame child = {0}; status = type_match_pair(&c, left, right, &child);
        if (status != XR_XIR_OK || !child.from) continue;
        if (child.from >= from || depth >= types->count) { status = XR_XIR_BAD_TYPE; break; }
        stack[depth++] = child;
    }
    xr_free(stack); return status;
}
