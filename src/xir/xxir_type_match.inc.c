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
static void type_match_scratch_free(XrXirTypeMatchScratch *scratch,XirReadScratch *read) {
    if (!scratch) return;
    TypeMatchMemory *memory = scratch->memory;
    while (memory) {
        TypeMatchMemory *next = memory->next;
        xir_read_scratch_release(read,memory);
        memory = next;
    }
    *scratch = (XrXirTypeMatchScratch) {0};
}
XR_FUNC void xr_xir_type_match_scratch_free(XrXirTypeMatchScratch *scratch) {
    type_match_scratch_free(scratch,NULL);
}
static XrXirStatus type_match_stack_claim(const XrXirCompileContext *context,
    XrXirTypeMatchScratch *scratch, uint32_t count, TypeMatchMemory **output,XirReadScratch *read) {
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
    TypeMatchMemory *memory = xir_read_scratch_alloc(context,read,(size_t)bytes,false,&status);
    if (!memory) return status;
    *memory = (TypeMatchMemory) {scratch->memory, (TypeMatchFrame *) (memory + 1), count, true};
    scratch->memory = memory;
    *output = memory;
    return XR_XIR_OK;
}
/* Reserve only the depth actually traversed. Nested substitutions claim
 * separate blocks; no live frame may alias a reused scratch reservation. */
static XrXirStatus type_match_stack_grow(const XrXirCompileContext *context,
    XrXirTypeMatchScratch *scratch, TypeMatchMemory **current, uint32_t capacity,
    uint32_t used,XirReadScratch *read) {
    TypeMatchMemory *next = NULL;
    XrXirStatus status = type_match_stack_claim(context, scratch, capacity, &next,read);
    if (status != XR_XIR_OK) return status;
    uint64_t bytes = (uint64_t)used * sizeof(*next->frames);
    if (!xir_compile_work(context, bytes)) {
        next->claimed = false; return XR_XIR_BUDGET;
    }
    memcpy(next->frames, (*current)->frames, (size_t)bytes);
    (*current)->claimed = false; *current = next;
    return XR_XIR_OK;
}

typedef struct TypeMatchContext {
    const XrXirTypes *source_types;
    const XrXirTypes *types;
    const XrXirType *arguments;
    uint32_t count;
    XrXirCompileContext *remaining;
    XrXirTypeMatchScratch *scratch;
    XirReadScratch *read;
} TypeMatchContext;
static XrXirStatus type_match_run(const XrXirCompileContext *context,
    const XirTypeMatchRead *request,XrXirTypeMatchScratch *scratch);

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
        return type_match_run(c->remaining,&(XirTypeMatchRead){c->types,c->types,NULL,0,c->arguments[index],actual,c->read},c->scratch);
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
               from->kind != XR_XIR_TYPE_CALLABLE && from->kind != XR_XIR_TYPE_NULLABLE && from->kind != XR_XIR_TYPE_TUPLE &&
               from->kind != XR_XIR_TYPE_ATOMIC && from->kind != XR_XIR_TYPE_TASK &&
               from->kind != XR_XIR_TYPE_ITERATOR && from->kind != XR_XIR_TYPE_CHANNEL)
        return XR_XIR_BAD_TYPE;
    *frame = (TypeMatchFrame) {from, to, 0}; return XR_XIR_OK;
}
static XrXirStatus type_match_run(const XrXirCompileContext *compile_context,
    const XirTypeMatchRead *request,XrXirTypeMatchScratch *scratch) {
    if(!request) return XR_XIR_BAD_STRUCTURE;
    const XrXirTypes *source_types=request->source_types,*types=request->actual_types;
    const XrXirType *arguments=request->arguments;uint32_t count=request->argument_count;
    XrXirType expected=request->expected,actual=request->actual;
    XirReadScratch *read=request->memory;
    if (!xir_compile_context_valid(compile_context) || !scratch ||
        scratch->resources != compile_context->resources) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *remaining = &compile_state;
    if (!remaining || (count && !arguments)) return XR_XIR_BAD_STRUCTURE;
    TypeMatchContext c = {source_types, types, arguments, count, remaining, scratch,read};
    TypeMatchFrame root = {0};
    XrXirStatus status = type_match_pair(&c, expected, actual, &root);
    if (status != XR_XIR_OK || !root.from) return status;
    TypeMatchMemory *claimed = NULL;
    uint32_t initial = source_types->count < 8 ? source_types->count : 8;
    status = type_match_stack_claim(remaining, scratch, initial, &claimed,read);
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
        if (depth == claimed->capacity) {
            uint32_t capacity = claimed->capacity > source_types->count / 2 ?
                source_types->count : claimed->capacity * 2;
            status = type_match_stack_grow(remaining, scratch, &claimed, capacity, depth,read);
            if (status != XR_XIR_OK) break;
            stack = claimed->frames;
        }
        stack[depth++] = child;
    }
    claimed->claimed = false; return status;
}
XR_FUNC XrXirStatus xr_xir_compile_type_substitution_matches_between_scratch(const XrXirCompileContext *context,const XrXirTypes *source,const XrXirTypes *actual,const XrXirType *arguments,uint32_t count,XrXirType expected,XrXirType value,XrXirTypeMatchScratch *scratch) {
    XirTypeMatchRead request={source,actual,arguments,count,expected,value,NULL};
    return type_match_run(context,&request,scratch);
}
XR_FUNC XrXirStatus xr_xir_compile_type_substitution_matches_between(const XrXirCompileContext *compile_context, const XrXirTypes *source_types, const XrXirTypes *types, const XrXirType *arguments, uint32_t count, XrXirType expected, XrXirType actual) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirTypeMatchScratch scratch = {compile_context->resources, NULL};
    XrXirStatus status = xr_xir_compile_type_substitution_matches_between_scratch(
        compile_context, source_types, types, arguments, count, expected, actual, &scratch);
    xr_xir_type_match_scratch_free(&scratch);
    return status;
}
XR_FUNC XrXirStatus xir_type_match_read(const XrXirCompileContext *context,const XirTypeMatchRead *request) {
    if(!xir_compile_context_valid(context) || !request) return XR_XIR_BAD_STRUCTURE;
    XrXirTypeMatchScratch scratch={context->resources,NULL};
    XrXirStatus status=type_match_run(context,request,&scratch);
    type_match_scratch_free(&scratch,request->memory);return status;
}
XR_FUNC XrXirStatus xr_xir_compile_type_substitution_matches(const XrXirCompileContext *compile_context, const XrXirTypes *types, const XrXirType *arguments, uint32_t count, XrXirType expected, XrXirType actual) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *remaining = &compile_state;
    return xr_xir_compile_type_substitution_matches_between(remaining, types, types, arguments, count, expected, actual);
}
