/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_terms.inc.c - Owned structural identities for contextual effects
 *
 * KEY CONCEPT:
 *   Verified declaration identities survive substitution without publishing a
 *   new executable descriptor or borrowing storage into the final summary.
 */
#include "xxir_nominal.h"
#include "xxir_type_match_internal.h"

typedef struct EffectTermMemory {
    struct EffectTermMemory *next;
    size_t capacity, used;
    uint64_t alignment;
} EffectTermMemory;
typedef struct EffectTermFrame {
    XrXirTypeNode node;
    uint32_t index, next;
    void *children;
    size_t child_capacity;
} EffectTermFrame;
typedef struct EffectTerms {
    XrXirTypes types;
    XrXirNominalTable *owned_nominals;
    XrXirInterfaceTable *owned_interfaces;
    EffectTermMemory *memory, *slice;
    const XrXirCompileContext *remaining;
    XrXirStatus status;
    uint32_t capacity;
    size_t slice_capacity;
    EffectTermFrame *stack;
    XrXirType *cache;
    uint32_t stack_capacity;
    bool stack_claimed;
    XrXirTypeMatchScratch match;
} EffectTerms;

_Static_assert(_Alignof(EffectTermMemory)>=_Alignof(XrXirTypeNode) &&
    _Alignof(EffectTermMemory)>=_Alignof(XrXirInstruction) &&
    _Alignof(EffectTermMemory)>=_Alignof(void *) &&
    sizeof(EffectTermMemory)%_Alignof(EffectTermMemory)==0,
    "Effect term slices preserve structural metadata alignment");

/* All slices have the same owner lifetime. Reserved tail bytes are charged
 * physical storage; only admitted slices are initialized or read. */
static void *effect_terms_alloc(EffectTerms *pool, uint64_t count, size_t size) {
    if (!count || pool->status!=XR_XIR_OK) return NULL;
    if (!size) { pool->status=XR_XIR_BAD_STRUCTURE;return NULL; }
    size_t alignment=_Alignof(EffectTermMemory);
    if (count>SIZE_MAX/size || count==UINT64_MAX ||
        !xir_compile_work(pool->remaining,count+1)) {
        pool->status=XR_XIR_BUDGET;return NULL;
    }
    size_t bytes=(size_t)count*size;
    if (bytes>SIZE_MAX-(alignment-1)) { pool->status=XR_XIR_BUDGET;return NULL; }
    size_t rounded=(bytes+alignment-1)/alignment*alignment;
    EffectTermMemory *memory=pool->slice;
    if (rounded>4096 || !memory || rounded>memory->capacity-memory->used) {
        size_t capacity=rounded;
        if (rounded<=4096) {
            capacity=pool->slice_capacity ? pool->slice_capacity : 256;
            while (capacity<rounded) capacity*=2;
            if (pool->slice && capacity<4096) capacity*=2;
            if (capacity>4096) capacity=4096;
        }
        if (capacity>SIZE_MAX-sizeof(*memory) ||
            !xir_compile_work(pool->remaining,sizeof(*memory))) {
            pool->status=XR_XIR_BUDGET;return NULL;
        }
        XrXirStatus status=XR_XIR_OK;
        memory=xir_compile_alloc(pool->remaining,sizeof(*memory)+capacity,&status);
        if (!memory) { pool->status=status;return NULL; }
        *memory=(EffectTermMemory){.next=pool->memory,.capacity=capacity};
        pool->memory=memory;
        if (rounded<=4096) { pool->slice=memory;pool->slice_capacity=capacity; }
    }
    if (!xir_compile_work(pool->remaining,bytes)) { pool->status=XR_XIR_BUDGET;return NULL; }
    void *output=(unsigned char *)(memory+1)+memory->used;
    memset(output,0,bytes);memory->used+=rounded;return output;
}

/* The structural owner retains reusable stack capacity, never a type proof.
 * Its original ledger admits every fresh comparison and owns all blocks. */
static XrXirTypeMatchScratch *effect_terms_type_scratch(EffectTerms *pool) {
    if (!pool->match.resources) pool->match.resources=pool->remaining->resources;
    return &pool->match;
}

static void effect_terms_free(EffectTerms *pool) {
    xr_xir_compile_nominal_free(pool->owned_nominals);pool->owned_nominals=NULL;
    xr_xir_compile_interfaces_free(pool->owned_interfaces);pool->owned_interfaces=NULL;
    xr_xir_type_match_scratch_free(&pool->match);
    for (uint32_t i=0;i<pool->stack_capacity;++i)
        xr_compile_resources_free(pool->stack[i].children);
    xr_compile_resources_free(pool->stack);pool->stack=NULL;pool->cache=NULL;
    pool->stack_capacity=0;pool->stack_claimed=false;
    while (pool->memory) {
        EffectTermMemory *memory=pool->memory;pool->memory=memory->next;
        xr_compile_resources_free(memory);
    }
    pool->slice=NULL;pool->slice_capacity=0;
}
/* Shape equality intentionally excludes executable field/layout projections. */
static bool effect_term_same(EffectTerms *pool, const XrXirTypeNode *a, const XrXirTypeNode *b) {
    if (!xir_compile_work(pool->remaining, (uint64_t)a->parameter_count + a->nominal.argument_count + 1)) {
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
        if (pool->types.count) { if (!xir_compile_work(pool->remaining, (size_t)pool->types.count * sizeof(*nodes))) { pool->status = XR_XIR_BUDGET; return XR_XIR_UNIT; } memcpy(nodes, pool->types.nodes, (size_t)pool->types.count * sizeof(*nodes)); }
        pool->types.nodes = nodes; pool->capacity = capacity;
    }
    if (node.nominal.argument_count) {
        XrXirType *arguments = effect_terms_alloc(pool, node.nominal.argument_count, sizeof(*arguments));
        if (!arguments) return XR_XIR_UNIT;
        { if (!xir_compile_work(pool->remaining, (size_t)node.nominal.argument_count * sizeof(*arguments))) { pool->status = XR_XIR_BUDGET; return XR_XIR_UNIT; } memcpy(arguments, node.nominal.arguments, (size_t)node.nominal.argument_count * sizeof(*arguments)); }
        node.nominal.arguments = arguments;
    }
    if (node.parameter_count) {
        XrXirCallableParameter *parameters = effect_terms_alloc(pool, node.parameter_count, sizeof(*parameters));
        if (!parameters) return XR_XIR_UNIT;
        { if (!xir_compile_work(pool->remaining, (size_t)node.parameter_count * sizeof(*parameters))) { pool->status = XR_XIR_BUDGET; return XR_XIR_UNIT; } memcpy(parameters, node.parameters, (size_t)node.parameter_count * sizeof(*parameters)); }
        node.parameters = parameters;
    }
    node.nominal.fields = NULL; node.nominal.field_count = 0;
    ((XrXirTypeNode *)pool->types.nodes)[pool->types.count] = node;
    return (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + pool->types.count++);
}
static bool effect_term_ready(EffectTerms *pool, XrXirType type, const XrXirGeneric *arguments,
    const XrXirType *cache, XrXirType *result) {
    if (!xir_compile_work(pool->remaining, 1)) { pool->status = XR_XIR_BUDGET; return false; }
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
/* Capacity survives sequential substitutions; frames and result cache do
 * not. Every active traversal owns its frames until it has completely unwound. */
static XrXirStatus effect_term_stack(EffectTerms *pool,uint32_t size) {
    if (pool->stack_claimed) return XR_XIR_BAD_STRUCTURE;
    uint64_t bytes=(uint64_t)size*(sizeof(EffectTermFrame)+sizeof(XrXirType));
    if (bytes>SIZE_MAX || !xir_compile_work(pool->remaining,(uint64_t)size*2+1))
        return XR_XIR_BUDGET;
    if (size>pool->stack_capacity) {
        XrXirStatus status=XR_XIR_OK;
        EffectTermFrame *stack=xir_compile_alloc(pool->remaining,(size_t)bytes,&status);
        if (!stack) return status;
        size_t frame_bytes=(size_t)size*sizeof(*stack);
        uint64_t copy=(uint64_t)pool->stack_capacity*sizeof(*stack);
        if (!xir_compile_work(pool->remaining,(uint64_t)frame_bytes+copy)) {
            xr_compile_resources_free(stack);return XR_XIR_BUDGET;
        }
        memset(stack,0,frame_bytes);
        if (copy) memcpy(stack,pool->stack,(size_t)copy);
        xr_compile_resources_free(pool->stack);
        pool->stack=stack;pool->cache=(XrXirType *)(stack+size);pool->stack_capacity=size;
    }
    size_t cache_bytes=(size_t)size*sizeof(*pool->cache);
    if (!xir_compile_work(pool->remaining,cache_bytes)) return XR_XIR_BUDGET;
    if (cache_bytes) memset(pool->cache,0,cache_bytes);
    pool->stack_claimed=true;return XR_XIR_OK;
}

static bool effect_term_push(EffectTerms *pool,EffectTermFrame *frame,uint32_t index) {
    void *children=frame->children;size_t capacity=frame->child_capacity;
    *frame=(EffectTermFrame){.node=pool->types.nodes[index],.index=index,
        .children=children,.child_capacity=capacity};
    frame->node.parameter_span=0;
    uint64_t bytes=(uint64_t)frame->node.nominal.argument_count*sizeof(XrXirType)+
        (uint64_t)frame->node.parameter_count*sizeof(XrXirCallableParameter);
    if (bytes>SIZE_MAX || !xir_compile_work(pool->remaining,1)) {
        pool->status=XR_XIR_BUDGET;return false;
    }
    if (bytes>capacity) {
        XrXirStatus status=XR_XIR_OK;
        children=xir_compile_alloc(pool->remaining,(size_t)bytes,&status);
        if (!children) { pool->status=status;return false; }
        xr_compile_resources_free(frame->children);
        frame->children=children;frame->child_capacity=(size_t)bytes;
    }
    if (!xir_compile_work(pool->remaining,bytes)) { pool->status=XR_XIR_BUDGET;return false; }
    if (bytes) memset(frame->children,0,(size_t)bytes);
    if (frame->node.kind==XR_XIR_TYPE_NOMINAL)
        frame->node.nominal.arguments=frame->node.nominal.argument_count ? frame->children : NULL;
    else frame->node.parameters=frame->node.parameter_count ? frame->children : NULL;
    return true;
}

static XrXirStatus effect_terms_substitute(EffectTerms *pool,XrXirType type,
    const XrXirGeneric *arguments,XrXirType *output) {
    *output=XR_XIR_UNIT;
    if (pool->status!=XR_XIR_OK) return pool->status;
    if (!xr_xir_type_span(&pool->types,type)) { *output=type;return XR_XIR_OK; }
    uint32_t size=pool->types.count;
    XrXirStatus status=effect_term_stack(pool,size);
    if (status!=XR_XIR_OK) return status;
    EffectTermFrame *stack=pool->stack;XrXirType *cache=pool->cache;
    uint32_t depth=0;XrXirType result=XR_XIR_UNIT;
    if (!effect_term_ready(pool,type,arguments,cache,&result) && pool->status==XR_XIR_OK) {
        if (!size) pool->status=XR_XIR_BAD_TYPE;
        else if (effect_term_push(pool,&stack[depth],(uint32_t)type-XR_XIR_CONSTRUCTED_TYPE_BASE)) ++depth;
    }
    while (depth && pool->status==XR_XIR_OK) {
        if (!xir_compile_work(pool->remaining,1)) { pool->status=XR_XIR_BUDGET;break; }
        EffectTermFrame *frame=&stack[depth-1];
        XrXirTypeNode source=pool->types.nodes[frame->index];
        uint32_t components=source.kind==XR_XIR_TYPE_NOMINAL ? source.nominal.argument_count :
            source.kind==XR_XIR_TYPE_CALLABLE ? source.parameter_count+1 :
            source.kind==XR_XIR_TYPE_TUPLE ? source.parameter_count : 1;
        if (frame->next==components) {
            cache[frame->index]=effect_term_intern(pool,frame->node);
            --depth;continue;
        }
        XrXirType child=source.element;
        if (source.kind==XR_XIR_TYPE_NOMINAL) child=source.nominal.arguments[frame->next];
        else if (source.kind==XR_XIR_TYPE_TUPLE) child=source.parameters[frame->next].type;
        else if (source.kind==XR_XIR_TYPE_CALLABLE)
            child=frame->next==source.parameter_count ? source.result : source.parameters[frame->next].type;
        if (!effect_term_ready(pool,child,arguments,cache,&result)) {
            if (pool->status!=XR_XIR_OK) break;
            uint32_t index=(uint32_t)child-XR_XIR_CONSTRUCTED_TYPE_BASE;
            if (index>=frame->index || depth>=size) { pool->status=XR_XIR_BAD_TYPE;break; }
            if (effect_term_push(pool,&stack[depth],index)) ++depth;
            continue;
        }
        if (source.kind==XR_XIR_TYPE_NOMINAL) ((XrXirType *)frame->node.nominal.arguments)[frame->next]=result;
        else if (source.kind!=XR_XIR_TYPE_CALLABLE && source.kind!=XR_XIR_TYPE_TUPLE) frame->node.element=result;
        else if (source.kind==XR_XIR_TYPE_CALLABLE && frame->next==source.parameter_count) frame->node.result=result;
        else ((XrXirCallableParameter *)frame->node.parameters)[frame->next]=
            (XrXirCallableParameter){result,source.parameters[frame->next].mode};
        uint32_t span=xr_xir_type_span(&pool->types,result);
        if (span>frame->node.parameter_span) frame->node.parameter_span=span;
        ++frame->next;
    }
    if (pool->status==XR_XIR_OK && !effect_term_ready(pool,type,arguments,cache,output) && pool->status==XR_XIR_OK)
        pool->status=XR_XIR_BAD_TYPE;
    pool->stack_claimed=false;return pool->status;
}
