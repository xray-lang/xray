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
typedef struct TypeMatchMemory {
    struct TypeMatchMemory *next;
    TypeMatchFrame *frames;
    uint32_t capacity;
    bool claimed;
} TypeMatchMemory;
XR_FUNC void xr_xir_type_match_scratch_free(XrXirTypeMatchScratch *scratch) {
    if (!scratch) return;
    TypeMatchMemory *memory = scratch->memory;
    while (memory) {
        TypeMatchMemory *next = memory->next;
        xr_compile_resources_free(memory);
        memory = next;
    }
    *scratch = (XrXirTypeMatchScratch) {0};
}
static XrXirStatus type_match_stack_claim(const XrXirCompileContext *context,
    XrXirTypeMatchScratch *scratch, uint32_t count, TypeMatchMemory **output) {
    for (TypeMatchMemory *memory = scratch->memory; memory; memory = memory->next) {
        if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
        if (!memory->claimed && memory->capacity >= count) {
            memory->claimed = true;
            *output = memory;
            return XR_XIR_OK;
        }
    }
    uint64_t bytes = sizeof(TypeMatchMemory) + (uint64_t) count * sizeof(TypeMatchFrame);
    if (!count || bytes > SIZE_MAX) return XR_XIR_BUDGET;
    XrXirStatus status = XR_XIR_OK;
    TypeMatchMemory *memory = xir_compile_alloc(context, (size_t) bytes, &status);
    if (!memory) return status;
    *memory = (TypeMatchMemory) {scratch->memory, (TypeMatchFrame *) (memory + 1), count, true};
    scratch->memory = memory;
    *output = memory;
    return XR_XIR_OK;
}
typedef struct TypeMatchContext {
    const XrXirTypes *source_types;
    const XrXirTypes *types;
    const XrXirType *arguments;
    uint32_t count;
    XrXirCompileContext *remaining;
    XrXirTypeMatchScratch *scratch;
} TypeMatchContext;
static XrXirStatus type_match_pair(TypeMatchContext *c, XrXirType expected,
                                  XrXirType actual, TypeMatchFrame *frame) {
    frame->from = NULL;
    if (!xir_compile_work(c->remaining, 1)) return XR_XIR_BUDGET;

    uint32_t id = (uint32_t) expected;
    if (!c->count && c->source_types == c->types && expected == actual) return XR_XIR_OK;
    if (id >= XR_XIR_TYPE_PARAMETER_BASE && id < XR_XIR_TYPE_PARAMETER_LIMIT) {
        uint32_t index = id - XR_XIR_TYPE_PARAMETER_BASE;
        if (index >= c->count) return XR_XIR_BAD_TYPE;
        if (c->arguments[index] == actual) return XR_XIR_OK;
        /* Substituted expressions belong to the destination environment. The
         * nested matcher has no substitution, so this branch cannot recur. */
        return xr_xir_compile_type_substitution_matches_between_scratch(c->remaining, c->types, c->types, NULL, 0, c->arguments[index], actual, c->scratch);
    }
    const XrXirTypeNode *from = xr_xir_type_node(c->source_types, expected);
    if (!from) return expected == actual ? XR_XIR_OK : XR_XIR_BAD_TYPE;
    if (c->source_types == c->types && !from->parameter_span && expected == actual) return XR_XIR_OK;
    const XrXirTypeNode *to = xr_xir_type_node(c->types, actual);
    if (!to || from->kind != to->kind || from->parameter_count != to->parameter_count || from->flags != to->flags)
        return XR_XIR_BAD_TYPE;
    if (from->kind == XR_XIR_TYPE_NOMINAL) {
        if (from->nominal.declaration != to->nominal.declaration ||
            from->nominal.argument_count != to->nominal.argument_count) return XR_XIR_BAD_TYPE;
    } else if (from->kind != XR_XIR_TYPE_ARRAY && from->kind != XR_XIR_TYPE_CELL &&
               from->kind != XR_XIR_TYPE_CALLABLE && from->kind != XR_XIR_TYPE_NULLABLE && from->kind != XR_XIR_TYPE_TUPLE)
        return XR_XIR_BAD_TYPE;
    *frame = (TypeMatchFrame) {from, to, 0}; return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_xir_compile_type_substitution_matches_between_scratch(const XrXirCompileContext *compile_context, const XrXirTypes *source_types, const XrXirTypes *types, const XrXirType *arguments, uint32_t count, XrXirType expected, XrXirType actual, XrXirTypeMatchScratch *scratch) {
    if (!xir_compile_context_valid(compile_context) || !scratch ||
        scratch->resources != compile_context->resources) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *remaining = &compile_state;
    if (!remaining || (count && !arguments)) return XR_XIR_BAD_STRUCTURE;
    TypeMatchContext c = {source_types, types, arguments, count, remaining, scratch};
    TypeMatchFrame root = {0};
    XrXirStatus status = type_match_pair(&c, expected, actual, &root);
    if (status != XR_XIR_OK || !root.from) return status;
    TypeMatchMemory *claimed = NULL;
    status = type_match_stack_claim(remaining, scratch, source_types->count, &claimed);
    if (status != XR_XIR_OK) return status;
    TypeMatchFrame *stack = claimed->frames;
    uint32_t depth = 1; stack[0] = root;
    while (depth && status == XR_XIR_OK) {
        TypeMatchFrame *frame = &stack[depth - 1];
        const XrXirTypeNode *from = frame->from, *to = frame->to;
        uint32_t components = from->kind == XR_XIR_TYPE_NOMINAL ? from->nominal.argument_count :
            from->kind == XR_XIR_TYPE_CALLABLE ? from->parameter_count + 1 :
            from->kind == XR_XIR_TYPE_TUPLE ? from->parameter_count : 1;
        if (frame->next == components) { --depth; continue; }
        XrXirType left = from->element, right = to->element;
        if (from->kind == XR_XIR_TYPE_NOMINAL) {
            left = from->nominal.arguments[frame->next]; right = to->nominal.arguments[frame->next];
        } else if (from->kind == XR_XIR_TYPE_TUPLE) {
            uint32_t p=frame->next;
            if (from->parameters[p].mode || to->parameters[p].mode) {status=XR_XIR_BAD_TYPE;break;}
            left=from->parameters[p].type;right=to->parameters[p].type;
        } else if (from->kind == XR_XIR_TYPE_CALLABLE) {
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
        if (child.from >= from || depth >= source_types->count) { status = XR_XIR_BAD_TYPE; break; }
        stack[depth++] = child;
    }
    claimed->claimed = false; return status;
}
XR_FUNC XrXirStatus xr_xir_compile_type_substitution_matches_between(const XrXirCompileContext *compile_context, const XrXirTypes *source_types, const XrXirTypes *types, const XrXirType *arguments, uint32_t count, XrXirType expected, XrXirType actual) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirTypeMatchScratch scratch = {compile_context->resources, NULL};
    XrXirStatus status = xr_xir_compile_type_substitution_matches_between_scratch(
        compile_context, source_types, types, arguments, count, expected, actual, &scratch);
    xr_xir_type_match_scratch_free(&scratch);
    return status;
}
XR_FUNC XrXirStatus xr_xir_compile_type_substitution_matches(const XrXirCompileContext *compile_context, const XrXirTypes *types, const XrXirType *arguments, uint32_t count, XrXirType expected, XrXirType actual) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *remaining = &compile_state;
    return xr_xir_compile_type_substitution_matches_between(remaining, types, types, arguments, count, expected, actual);
}
