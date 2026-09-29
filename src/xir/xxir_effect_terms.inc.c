/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_terms.inc.c - Temporary structural identities for error effects
 *
 * KEY CONCEPT:
 *   Verified declaration identities survive substitution without publishing a
 *   new executable descriptor or borrowing storage into the final summary.
 */
typedef struct EffectTermMemory {
    struct EffectTermMemory *next;
    uint64_t bytes;
} EffectTermMemory;
typedef struct EffectTerms {
    XrXirTypes types;
    EffectTermMemory *memory;
    XrXirBudget *remaining;
    XrXirStatus status;
    uint32_t capacity;
} EffectTerms;
typedef struct EffectTermFrame {
    XrXirTypeNode node;
    uint32_t index, next;
    void *children;
    uint64_t bytes;
} EffectTermFrame;
static void *effect_terms_alloc(EffectTerms *pool, uint64_t count, size_t size) {
    if (!count || pool->status != XR_XIR_OK) return NULL;
    if (count > (SIZE_MAX - sizeof(EffectTermMemory)) / size ||
        count * size + sizeof(EffectTermMemory) > pool->remaining->scratch_bytes ||
        !effect_spend(&pool->remaining->work, count + 1)) {
        pool->status = XR_XIR_BUDGET; return NULL;
    }
    uint64_t bytes = count * size + sizeof(EffectTermMemory);
    EffectTermMemory *memory = xr_calloc(1, (size_t)bytes);
    if (!memory) { pool->status = XR_XIR_OUT_OF_MEMORY; return NULL; }
    pool->remaining->scratch_bytes -= bytes;
    memory->bytes = bytes; memory->next = pool->memory; pool->memory = memory;
    return memory + 1;
}
static void effect_terms_free(EffectTerms *pool) {
    while (pool->memory) {
        EffectTermMemory *memory = pool->memory; pool->memory = memory->next;
        pool->remaining->scratch_bytes += memory->bytes; xr_free(memory);
    }
}
/* Shape equality intentionally excludes executable field/layout projections. */
static bool effect_term_same(EffectTerms *pool, const XrXirTypeNode *a, const XrXirTypeNode *b) {
    if (!effect_spend(&pool->remaining->work, (uint64_t)a->parameter_count + a->nominal.argument_count + 1)) {
        pool->status = XR_XIR_BUDGET; return false;
    }
    if (a->kind != b->kind || a->flags != b->flags || a->element != b->element ||
        a->result != b->result || a->parameter_count != b->parameter_count ||
        a->nominal.declaration != b->nominal.declaration ||
        a->nominal.argument_count != b->nominal.argument_count) return false;
    for (uint32_t i = 0; i < a->parameter_count; ++i)
        if (a->parameters[i].type != b->parameters[i].type || a->parameters[i].mode != b->parameters[i].mode)
            return false;
    for (uint32_t i = 0; i < a->nominal.argument_count; ++i)
        if (a->nominal.arguments[i] != b->nominal.arguments[i]) return false;
    return true;
}
static XrXirType effect_term_intern(EffectTerms *pool, XrXirTypeNode node) {
    for (uint32_t i = 0; i < pool->types.count && pool->status == XR_XIR_OK; ++i)
        if (effect_term_same(pool, &pool->types.nodes[i], &node))
            return (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + i);
    if (pool->status != XR_XIR_OK) return XR_XIR_UNIT;
    uint32_t maximum = XR_XIR_CONSTRUCTED_TYPE_LIMIT - XR_XIR_CONSTRUCTED_TYPE_BASE;
    if (pool->types.count == maximum) { pool->status = XR_XIR_BUDGET; return XR_XIR_UNIT; }
    if (pool->types.count >= pool->capacity) {
        uint32_t capacity = pool->types.count < 8 ? 8 : pool->types.count * 2;
        if (capacity > maximum) capacity = maximum;
        XrXirTypeNode *nodes = effect_terms_alloc(pool, capacity, sizeof(*nodes));
        if (!nodes) return XR_XIR_UNIT;
        if (pool->types.count) memcpy(nodes, pool->types.nodes, (size_t)pool->types.count * sizeof(*nodes));
        pool->types.nodes = nodes; pool->capacity = capacity;
    }
    if (node.nominal.argument_count) {
        XrXirType *arguments = effect_terms_alloc(pool, node.nominal.argument_count, sizeof(*arguments));
        if (!arguments) return XR_XIR_UNIT;
        memcpy(arguments, node.nominal.arguments, (size_t)node.nominal.argument_count * sizeof(*arguments));
        node.nominal.arguments = arguments;
    }
    if (node.parameter_count) {
        XrXirCallableParameter *parameters = effect_terms_alloc(pool, node.parameter_count, sizeof(*parameters));
        if (!parameters) return XR_XIR_UNIT;
        memcpy(parameters, node.parameters, (size_t)node.parameter_count * sizeof(*parameters));
        node.parameters = parameters;
    }
    node.nominal.fields = NULL; node.nominal.field_count = 0;
    ((XrXirTypeNode *)pool->types.nodes)[pool->types.count] = node;
    return (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + pool->types.count++);
}
static bool effect_term_ready(EffectTerms *pool, XrXirType type, const XrXirGeneric *arguments,
    const XrXirType *cache, XrXirType *result) {
    if (!effect_spend(&pool->remaining->work, 1)) { pool->status = XR_XIR_BUDGET; return false; }
    if ((uint32_t)type >= XR_XIR_TYPE_PARAMETER_BASE && (uint32_t)type < XR_XIR_TYPE_PARAMETER_LIMIT) {
        uint32_t p = (uint32_t)type - XR_XIR_TYPE_PARAMETER_BASE;
        if (p >= arguments->argument_count) { pool->status = XR_XIR_BAD_TYPE; return false; }
        *result = arguments->arguments[p]; return true;
    }
    const XrXirTypeNode *node = xr_xir_type_node(&pool->types, type);
    if (!node || !node->parameter_span) { *result = type; return true; }
    *result = cache[(uint32_t)type - XR_XIR_CONSTRUCTED_TYPE_BASE];
    return *result != XR_XIR_UNIT;
}
static bool effect_term_push(EffectTerms *pool, EffectTermFrame *frame, uint32_t index) {
    *frame = (EffectTermFrame){0}; frame->index = index; frame->node = pool->types.nodes[index];
    frame->node.parameter_span = 0;
    uint64_t bytes = (uint64_t)frame->node.nominal.argument_count * sizeof(XrXirType) +
        (uint64_t)frame->node.parameter_count * sizeof(XrXirCallableParameter);
    if (bytes > SIZE_MAX || bytes > pool->remaining->scratch_bytes ||
        !effect_spend(&pool->remaining->work, bytes + 1)) { pool->status = XR_XIR_BUDGET; return false; }
    if (bytes) {
        frame->children = xr_calloc(1, (size_t)bytes);
        if (!frame->children) { pool->status = XR_XIR_OUT_OF_MEMORY; return false; }
        frame->bytes = bytes; pool->remaining->scratch_bytes -= bytes;
        if (frame->node.kind == XR_XIR_TYPE_NOMINAL) frame->node.nominal.arguments = frame->children;
        else frame->node.parameters = frame->children;
    }
    return true;
}
static void effect_term_pop(EffectTerms *pool, EffectTermFrame *frame) {
    xr_free(frame->children); pool->remaining->scratch_bytes += frame->bytes;
}
static XrXirStatus effect_terms_substitute(EffectTerms *pool, XrXirType type,
    const XrXirGeneric *arguments, XrXirType *output) {
    *output = XR_XIR_UNIT;
    if (!xr_xir_type_span(&pool->types, type)) { *output = type; return XR_XIR_OK; }
    uint32_t size = pool->types.count;
    uint64_t bytes = (uint64_t)size * (sizeof(EffectTermFrame) + sizeof(XrXirType));
    if (bytes > SIZE_MAX || bytes > pool->remaining->scratch_bytes ||
        !effect_spend(&pool->remaining->work, (uint64_t)size * 2 + 1)) return XR_XIR_BUDGET;
    pool->remaining->scratch_bytes -= bytes;
    EffectTermFrame *stack = size ? xr_calloc(size, sizeof(*stack)) : NULL;
    XrXirType *cache = size ? xr_calloc(size, sizeof(*cache)) : NULL;
    uint32_t depth = 0;
    if (size && (!stack || !cache)) pool->status = XR_XIR_OUT_OF_MEMORY;
    XrXirType result = XR_XIR_UNIT;
    if (pool->status == XR_XIR_OK && !effect_term_ready(pool, type, arguments, cache, &result) &&
        pool->status == XR_XIR_OK) {
        if (!size) pool->status = XR_XIR_BAD_TYPE;
        else if (effect_term_push(pool, &stack[depth], (uint32_t)type - XR_XIR_CONSTRUCTED_TYPE_BASE)) ++depth;
    }
    while (depth && pool->status == XR_XIR_OK) {
        if (!effect_spend(&pool->remaining->work, 1)) { pool->status = XR_XIR_BUDGET; break; }
        EffectTermFrame *frame = &stack[depth - 1];
        XrXirTypeNode source = pool->types.nodes[frame->index];
        uint32_t components = source.kind == XR_XIR_TYPE_NOMINAL ? source.nominal.argument_count :
            source.kind == XR_XIR_TYPE_CALLABLE ? source.parameter_count + 1 : 1;
        if (frame->next == components) {
            cache[frame->index] = effect_term_intern(pool, frame->node);
            effect_term_pop(pool, frame); --depth; continue;
        }
        XrXirType child = source.element;
        if (source.kind == XR_XIR_TYPE_NOMINAL) child = source.nominal.arguments[frame->next];
        else if (source.kind == XR_XIR_TYPE_CALLABLE)
            child = frame->next == source.parameter_count ? source.result : source.parameters[frame->next].type;
        if (!effect_term_ready(pool, child, arguments, cache, &result)) {
            if (pool->status != XR_XIR_OK) break;
            uint32_t index = (uint32_t)child - XR_XIR_CONSTRUCTED_TYPE_BASE;
            if (index >= frame->index || depth >= size) { pool->status = XR_XIR_BAD_TYPE; break; }
            if (effect_term_push(pool, &stack[depth], index)) ++depth;
            continue;
        }
        if (source.kind == XR_XIR_TYPE_NOMINAL) ((XrXirType *)frame->node.nominal.arguments)[frame->next] = result;
        else if (source.kind != XR_XIR_TYPE_CALLABLE) frame->node.element = result;
        else if (frame->next == source.parameter_count) frame->node.result = result;
        else ((XrXirCallableParameter *)frame->node.parameters)[frame->next] =
            (XrXirCallableParameter){result, source.parameters[frame->next].mode};
        uint32_t span = xr_xir_type_span(&pool->types, result);
        if (span > frame->node.parameter_span) frame->node.parameter_span = span;
        ++frame->next;
    }
    if (pool->status == XR_XIR_OK) {
        if (!effect_term_ready(pool, type, arguments, cache, output)) pool->status = XR_XIR_BAD_TYPE;
    }
    while (depth) effect_term_pop(pool, &stack[--depth]);
    xr_free(cache); xr_free(stack); pool->remaining->scratch_bytes += bytes;
    return pool->status;
}
