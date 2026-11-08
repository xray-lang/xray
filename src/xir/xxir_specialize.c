/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_specialize.c - Bounded Checked instance closure before physical lowering
 *
 * KEY CONCEPT:
 *   Instances substitute verified types and calls without retaining or revisiting AST.
 */
#include "xxir_generic.h"
#include "xxir_specialization_bindings.h"
#include "xxir_compile_memory.h"
#include "xxir_receive_control.h"
#include "xxir_operand_roles.h"
#include "xxir_types.h"
#include "xxir_internal.h"
#include "xxir_source_replay_internal.h"
#include "xxir_defaults_internal.h"
#include "xxir_implementation_verify.h"
#include "xxir_constraint_proof_internal.h"
#include "xxir_type_match_internal.h"
#include "xxir_comptime_pending_internal.h"
#include "../base/xmalloc.h"
#include "../base/xhash.h"
#include <stdio.h>
typedef struct SpecMemory { struct SpecMemory *next; } SpecMemory;
typedef struct SpecInstance {
    uint32_t declaration;
    const XrXirType *arguments;
    uint32_t count;
    XrXirType *node_cache;
    uint32_t owner, capture_owner;
} SpecInstance;
typedef struct SpecIndexEntry { uint32_t ordinal, hash; } SpecIndexEntry;
typedef struct SpecTypeFrame { uint32_t index, next; XrXirTypeNode node; } SpecTypeFrame;

typedef struct SpecPendingPoint {
    XrXirComptimeSite site;
    const XrXirType *borrowed_arguments;
    XrXirType *arguments;
    XrXirTypes *types;
    uint32_t count;
    bool found, complete;
} SpecPendingPoint;
typedef struct SpecContext {
    const XrXirModule *source;
    const XrXirArtifact *source_artifact;
    XirSourceFacts *source_facts;
    const XrXirEffects *effects;
    XrXirEffects *verification_effects;
    XirControlBuilder *control_builder;
    EffectControlSummary *control_summary;
    XrXirCompileContext remaining;
    XrXirDiagnostic diagnostic;
    SpecMemory *memory;
    XrXirFunction *functions;
    XrXirFunctionIdentity *identities;
    bool has_cleanup;
    SpecInstance *instances;
    SpecIndexEntry *instance_index;
    uint32_t index_capacity;
    uint32_t *ordinary;
    uint32_t count, capacity, cleanup_owner, lexical_context;
    XrXirTypes types;
    uint32_t node_capacity;
    SpecInstance closed;
    SpecTypeFrame *type_stack;
    SpecPendingPoint *pending;
    XirEvaluationRoot *evaluation;
    bool evaluation_found;
    XrCompileRequestControl *control;
    uint32_t control_rows;
} SpecContext;
static bool spec_progress(SpecContext *c) {
    if (c->diagnostic.status!=XR_XIR_OK) return false;
    c->diagnostic.status=xir_receive_step(c->control,&c->control_rows);
    return c->diagnostic.status==XR_XIR_OK;
}
static bool spec_copy(SpecContext *c,void *output,const void *input,size_t bytes) {
    if (c->diagnostic.status!=XR_XIR_OK) return false;
    c->diagnostic.status=xir_receive_copy(c->control,output,input,bytes);
    return c->diagnostic.status==XR_XIR_OK;
}
static bool spec_hash(SpecContext *c,const void *input,size_t bytes,uint32_t *output) {
    uint32_t hash=XR_FNV_OFFSET_BASIS;
    for (size_t offset=0;offset<bytes;) {
        if (c->diagnostic.status!=XR_XIR_OK) return false;
        c->diagnostic.status=xir_receive_poll(c->control);
        if (c->diagnostic.status!=XR_XIR_OK) return false;
        size_t span=bytes-offset;if (span>4096) span=4096;
        hash=xr_hash_bytes_update(hash,(const unsigned char *)input+offset,span);offset+=span;
    }
    if (c->diagnostic.status==XR_XIR_OK) c->diagnostic.status=xir_receive_poll(c->control);
    if (c->diagnostic.status!=XR_XIR_OK) return false;
    *output=hash;return true;
}
static void *spec_alloc(SpecContext *c, uint64_t count, size_t size) {
    XrXirStatus allocation_status = XR_XIR_OK;
    if (c->diagnostic.status != XR_XIR_OK || !count) return NULL;
    if (count > (SIZE_MAX - sizeof(SpecMemory)) / size ||
(sizeof(SpecMemory) + count * size > SIZE_MAX)) {
        c->diagnostic.status = XR_XIR_BUDGET; return NULL;
    }
    size_t bytes = sizeof(SpecMemory) + (size_t) count * size;
    SpecMemory *memory = xir_type_copy_zero(&c->remaining, 1, bytes, &allocation_status,c->control);
    if (!memory) { c->diagnostic.status = allocation_status; return NULL; }

    memory->next = c->memory; c->memory = memory; return memory + 1;
}
static bool spec_function_reserve(SpecContext *c, uint32_t minimum) {
    if (minimum<=c->capacity) return true;
    if (minimum>c->remaining.limits.functions) {
        c->diagnostic.status=XR_XIR_BUDGET;
        return false;
    }
    uint64_t grown=c->capacity ? (uint64_t)c->capacity*2 : c->source->function_count;
    if (grown<minimum) grown=minimum;
    uint32_t capacity=grown>c->remaining.limits.functions ?
        c->remaining.limits.functions : (uint32_t)grown;
    XrXirFunction *functions=spec_alloc(c,capacity,sizeof(*functions));
    XrXirFunctionIdentity *identities=c->source->declarations ?
        spec_alloc(c,capacity,sizeof(*identities)) : NULL;
    SpecInstance *instances=spec_alloc(c,capacity,sizeof(*instances));
    if (c->diagnostic.status!=XR_XIR_OK) return false;
    if (c->count) {
        if (!spec_copy(c,functions,c->functions,(size_t)c->count*sizeof(*functions))) return false;
        if (!spec_copy(c,instances,c->instances,(size_t)c->count*sizeof(*instances))) return false;
    }
    if (c->identities &&
        !spec_copy(c,identities,c->identities,(size_t)c->count*sizeof(*identities))) return false;
    c->functions=functions;
    c->identities=identities;
    c->instances=instances;
    c->capacity=capacity;
    return true;
}
static XrXirType spec_node_raw(SpecContext *c, XrXirTypeNode signature) {
    for (uint32_t i = 0; i < c->types.count; ++i) {
        if (!spec_progress(c)) return XR_XIR_UNIT;
        const XrXirTypeNode *s = &c->types.nodes[i];
        if (s->kind != signature.kind || s->element != signature.element || s->flags != signature.flags ||
            s->parameter_count != signature.parameter_count || s->result != signature.result ||
            s->parameter_span != signature.parameter_span ||
            s->nominal.declaration != signature.nominal.declaration ||
            s->nominal.argument_count != signature.nominal.argument_count) continue;
        if (!spec_progress(c)) return XR_XIR_UNIT;
        bool same = true;
        if (!spec_progress(c)) return XR_XIR_UNIT;
        for (uint32_t a = 0; a < s->nominal.argument_count; ++a) {
            if (!spec_progress(c)) return XR_XIR_UNIT;
            if (s->nominal.arguments[a] != signature.nominal.arguments[a]) same = false;
        }
        for (uint32_t p = 0; p < xr_xir_type_parameter_slots(s); ++p) {
            if (!spec_progress(c)) return XR_XIR_UNIT;
            if (s->parameters[p].type != signature.parameters[p].type ||
                s->parameters[p].mode != signature.parameters[p].mode || s->parameters[p].escaping != signature.parameters[p].escaping) same = false;
        }
        if (same) return (XrXirType) (XR_XIR_CONSTRUCTED_TYPE_BASE + i);
    }
    if (c->types.count == XR_XIR_CONSTRUCTED_TYPE_LIMIT - XR_XIR_CONSTRUCTED_TYPE_BASE) {
        c->diagnostic.status = XR_XIR_BUDGET; return XR_XIR_UNIT;
    }
    if (c->types.count == c->node_capacity) {
        uint32_t capacity = c->node_capacity ? c->node_capacity * 2 : 8;
        uint32_t maximum = XR_XIR_CONSTRUCTED_TYPE_LIMIT - XR_XIR_CONSTRUCTED_TYPE_BASE;
        if (capacity > maximum) capacity = maximum;
        XrXirTypeNode *table = spec_alloc(c, capacity, sizeof(*table));
        if (!table) return XR_XIR_UNIT;
        if (c->types.count && !spec_copy(c,table,c->types.nodes,(size_t)c->types.count*sizeof(*table))) return XR_XIR_UNIT;
        c->types.nodes = table; c->node_capacity = capacity;
    }
    ((XrXirTypeNode *) c->types.nodes)[c->types.count] = signature;
    return (XrXirType) (XR_XIR_CONSTRUCTED_TYPE_BASE + c->types.count++);
}
static XrXirType spec_node(SpecContext *c,XrXirTypeNode node) {
    if (node.kind!=XR_XIR_TYPE_UNION) return spec_node_raw(c,node);
    XrXirType *members=spec_alloc(c,node.parameter_count,sizeof(*members));
    if (node.parameter_count && !members) return XR_XIR_UNIT;
    for (uint32_t p=0;p<node.parameter_count;++p) {
        if (!spec_progress(c)) return XR_XIR_UNIT;
        members[p]=node.parameters[p].type;
    }
    XrXirErrorUnion normalized={0};
    XrXirStatus status=xr_xir_compile_error_union(&c->remaining,&c->types,members,node.parameter_count,&normalized);
    if (status!=XR_XIR_OK) {c->diagnostic.status=status;return XR_XIR_UNIT;}
    if (!normalized.count) return normalized.scalar;
    XrXirCallableParameter *owned=spec_alloc(c,normalized.count,sizeof(*owned));
    bool copied=!owned || spec_copy(c,owned,normalized.members,(size_t)normalized.count*sizeof(*owned));
    xr_compile_resources_free(normalized.members);
    if (!owned || !copied) return XR_XIR_UNIT;
    return spec_node_raw(c,(XrXirTypeNode){.kind=XR_XIR_TYPE_UNION,
        .parameters=owned,.parameter_count=normalized.count});
}
/* Source nodes are immutable; destination interning may grow its own table. */
static XrXirType *spec_type_cache(SpecContext *c, const SpecInstance *instance, uint32_t index) {
    return (c->source->types->nodes[index].parameter_span ? instance->node_cache : c->closed.node_cache) + index;
}
static bool spec_type_ready(SpecContext *c, const SpecInstance *instance, XrXirType type, XrXirType *result) {
    if (!spec_progress(c)) return false;
    if ((uint32_t) type >= XR_XIR_TYPE_PARAMETER_BASE && (uint32_t) type < XR_XIR_TYPE_PARAMETER_LIMIT) {
        uint32_t index = (uint32_t) type - XR_XIR_TYPE_PARAMETER_BASE;
        if (index >= instance->count) { c->diagnostic.status = XR_XIR_BAD_TYPE; return false; }
        *result = instance->arguments[index]; return true;
    }
    if (!xr_xir_type_node(c->source->types, type)) { *result = type; return true; }
    *result = *spec_type_cache(c, instance, (uint32_t) type - XR_XIR_CONSTRUCTED_TYPE_BASE);
    return *result != XR_XIR_UNIT;
}
static bool spec_type_push(SpecContext *c, uint32_t index, uint32_t *count) {
    if (*count >= c->source->types->count) { c->diagnostic.status = XR_XIR_BAD_TYPE; return false; }
    SpecTypeFrame *frame = &c->type_stack[(*count)++];
    *frame = (SpecTypeFrame) {index, 0, c->source->types->nodes[index]};
    frame->node.parameter_span = 0;
    if (frame->node.kind == XR_XIR_TYPE_CALLABLE || frame->node.kind == XR_XIR_TYPE_TUPLE || frame->node.kind == XR_XIR_TYPE_UNION) {
        uint32_t n = frame->node.parameter_count;
        XrXirCallableParameter *parameters = spec_alloc(c, n, sizeof(*parameters));
        if (n && !parameters) return false;
        frame->node.parameters = parameters;
    } else if (frame->node.kind == XR_XIR_TYPE_NOMINAL) {
        uint32_t n = frame->node.nominal.argument_count;
        XrXirType *arguments = spec_alloc(c, n, sizeof(*arguments));
        if (n && !arguments) return false;
        frame->node.nominal.arguments = arguments;
        frame->node.nominal.fields = NULL; frame->node.nominal.field_count = 0;
    }
    return true;
}
static XrXirType spec_type(SpecContext *c, const SpecInstance *instance, XrXirType type) {
    XrXirType result = XR_XIR_UNIT;
    if (spec_type_ready(c, instance, type, &result)) return result;
    if (c->diagnostic.status != XR_XIR_OK) return XR_XIR_UNIT;
    if (!c->type_stack) {
        c->type_stack = spec_alloc(c, c->source->types->count, sizeof(*c->type_stack));
        if (!c->type_stack) return XR_XIR_UNIT;
    }
    uint32_t count = 0;
    if (!spec_type_push(c, (uint32_t) type - XR_XIR_CONSTRUCTED_TYPE_BASE, &count)) return XR_XIR_UNIT;
    while (count && c->diagnostic.status == XR_XIR_OK) {
        SpecTypeFrame *frame = &c->type_stack[count - 1];
        const XrXirTypeNode *source = &c->source->types->nodes[frame->index];
        uint32_t components = source->kind == XR_XIR_TYPE_NOMINAL ? source->nominal.argument_count :
            source->kind == XR_XIR_TYPE_CALLABLE ? source->parameter_count + 2 :
            (source->kind == XR_XIR_TYPE_TUPLE || source->kind == XR_XIR_TYPE_UNION) ? source->parameter_count :
            (source->kind == XR_XIR_TYPE_MAP || source->kind == XR_XIR_TYPE_TASK || source->kind == XR_XIR_TYPE_ITERATOR) ? 2 : 1;
        if (frame->next == components) {
            *spec_type_cache(c, instance, frame->index) = spec_node(c, frame->node);
            --count; continue;
        }
        XrXirType child = source->element;
        if ((source->kind == XR_XIR_TYPE_MAP || source->kind == XR_XIR_TYPE_TASK || source->kind == XR_XIR_TYPE_ITERATOR) && frame->next)
            child = source->result;
        if (source->kind == XR_XIR_TYPE_NOMINAL) child = source->nominal.arguments[frame->next];
        if (source->kind == XR_XIR_TYPE_TUPLE || source->kind == XR_XIR_TYPE_UNION) child = source->parameters[frame->next].type;
        if (source->kind == XR_XIR_TYPE_CALLABLE && frame->next <= source->parameter_count)
            child = frame->next < source->parameter_count ? source->parameters[frame->next].type : source->result;
        if (!spec_type_ready(c, instance, child, &result)) {
            if (c->diagnostic.status != XR_XIR_OK) break;
            uint32_t index = (uint32_t) child - XR_XIR_CONSTRUCTED_TYPE_BASE;
            if (index >= frame->index) { c->diagnostic.status = XR_XIR_BAD_TYPE; break; }
            if (!spec_type_push(c, index, &count)) break;
            continue;
        }
        if (source->kind == XR_XIR_TYPE_NOMINAL)
            ((XrXirType *) frame->node.nominal.arguments)[frame->next] = result;
        else if ((source->kind == XR_XIR_TYPE_MAP || source->kind == XR_XIR_TYPE_TASK || source->kind == XR_XIR_TYPE_ITERATOR) && frame->next)
            frame->node.result = result;
        else if (source->kind != XR_XIR_TYPE_CALLABLE && source->kind != XR_XIR_TYPE_TUPLE && source->kind != XR_XIR_TYPE_UNION) frame->node.element = result;
        else if (source->kind == XR_XIR_TYPE_CALLABLE && frame->next == source->parameter_count) frame->node.result = result;
        else if (source->kind == XR_XIR_TYPE_CALLABLE && frame->next > source->parameter_count) frame->node.element = result;
        else {
            XrXirCallableParameter *parameters = (XrXirCallableParameter *) frame->node.parameters;
            parameters[frame->next] = source->parameters[frame->next];
            XrXirType original = parameters[frame->next].type;
            parameters[frame->next].type = result;
            if (frame->node.kind == XR_XIR_TYPE_CALLABLE &&
                !xr_xir_callable_signature(c->source->types,original) &&
                xr_xir_callable_signature(&c->types,result)) parameters[frame->next].escaping = 1;
        }
        ++frame->next;
    }
    return c->diagnostic.status == XR_XIR_OK ?
        *spec_type_cache(c, instance, (uint32_t) type - XR_XIR_CONSTRUCTED_TYPE_BASE) : XR_XIR_UNIT;
}

/* Hash only selects candidates in this request's destination type pool.
 * Explicit scalar fields exclude padding; ordered vectors retain every element. */
static bool spec_instance_hash(SpecContext *c, const SpecInstance *key, uint32_t *output) {
    uint32_t header[] = {key->declaration, key->count, key->owner, key->capture_owner};
    if (!spec_progress(c)) return false;
    uint32_t hash = xr_hash_bytes(header, sizeof(header));
    if (key->count) {
        uint64_t bytes = (uint64_t)key->count * sizeof(*key->arguments);
        if (bytes > SIZE_MAX || !spec_progress(c)) return false;
        uint32_t arguments=0;if (!spec_hash(c,key->arguments,(size_t)bytes,&arguments)) return false;
        uint32_t pair[] = {hash, arguments};
        hash = xr_hash_bytes(pair, sizeof(pair));
    }
    if (!spec_progress(c)) return false;
    *output = hash; return true;
}
static bool spec_instance_equal(SpecContext *c, const SpecInstance *a, const SpecInstance *b) {
    if (!spec_progress(c)) return false;
    if (a->declaration != b->declaration || a->count != b->count ||
        a->owner != b->owner || a->capture_owner != b->capture_owner) return false;
    for (uint32_t i = 0; i < a->count; ++i) {
        if (!spec_progress(c)) return false;
        if (a->arguments[i] != b->arguments[i]) return false;
    }
    return true;
}
static uint32_t spec_index_find(SpecContext *c, const SpecInstance *key, uint32_t hash) {
    if (!c->index_capacity) return UINT32_MAX;
    if (!spec_progress(c)) return UINT32_MAX;
    uint32_t slot = hash & (c->index_capacity - 1);
    for (uint32_t probe = 0; probe < c->index_capacity; ++probe) {
        if (!spec_progress(c)) return UINT32_MAX;
        SpecIndexEntry entry = c->instance_index[slot];
        if (!entry.ordinal) return UINT32_MAX;
        if (entry.hash == hash && spec_instance_equal(c, key, &c->instances[entry.ordinal - 1]))
            return entry.ordinal - 1;
        if (c->diagnostic.status != XR_XIR_OK || !spec_progress(c)) return UINT32_MAX;
        slot = (slot + 1) & (c->index_capacity - 1);
    }
    c->diagnostic.status = XR_XIR_BAD_STRUCTURE; return UINT32_MAX;
}
/* Existing entries already have distinct complete keys. Rehashing moves only
 * cached hashes and ordinals, never element pointers or semantic identities. */
static bool spec_index_place(SpecContext *c, SpecIndexEntry *table, uint32_t capacity, SpecIndexEntry entry) {
    if (!spec_progress(c)) return false;
    uint32_t slot = entry.hash & (capacity - 1);
    for (uint32_t probe = 0; probe < capacity; ++probe) {
        if (!spec_progress(c)) return false;
        if (!table[slot].ordinal) {
            if (!spec_progress(c)) return false;
            table[slot] = entry; return true;
        }
        if (!spec_progress(c)) return false;
        slot = (slot + 1) & (capacity - 1);
    }
    c->diagnostic.status = XR_XIR_BAD_STRUCTURE; return false;
}
static bool spec_index_reserve(SpecContext *c) {
    if (c->index_capacity && c->count < c->index_capacity / 2) return true;
    if (c->index_capacity > UINT32_MAX / 2) { c->diagnostic.status = XR_XIR_BUDGET; return false; }
    uint32_t capacity = c->index_capacity ? c->index_capacity * 2 : 8;
    SpecIndexEntry *table = spec_alloc(c, capacity, sizeof(*table));
    if (!table) return false;
    /* Allocation clears the table through the same finite ledger. Prior tables
     * remain on the request's memory chain and count toward live/peak limits. */
    for (uint32_t slot = 0; slot < c->index_capacity; ++slot) {
        if (!spec_progress(c)) return false;
        SpecIndexEntry entry = c->instance_index[slot];
        if (entry.ordinal && !spec_index_place(c, table, capacity, entry)) return false;
    }
    if (!spec_progress(c)) return false;
    c->instance_index = table; c->index_capacity = capacity; return true;
}


static bool spec_name(SpecContext *c, XrXirFunction *function, const SpecInstance *instance) {
    if (!instance->count) return true;
    uint64_t capacity = (uint64_t)function->name_length + 32 + (uint64_t)instance->count * 12;
    if (capacity > UINT32_MAX) { c->diagnostic.status = XR_XIR_BUDGET; return false; }
    char *name = spec_alloc(c, capacity, 1); if (!name) return false;
    if (!spec_copy(c,name,function->name,function->name_length)) return false;
    size_t at = function->name_length;
    int written = snprintf(name + at, (size_t) capacity - at, "$%u", instance->declaration);
    if (written < 0 || (size_t) written >= capacity - at) { c->diagnostic.status = XR_XIR_BAD_STRUCTURE; return false; }
    at += (size_t) written;
    for (uint32_t i = 0; i < instance->count; ++i) {
        if (!spec_progress(c)) return false;
        written = snprintf(name + at, (size_t) capacity - at, ":%u", (unsigned) instance->arguments[i]);
        if (written < 0 || (size_t) written >= capacity - at) { c->diagnostic.status = XR_XIR_BAD_STRUCTURE; return false; }
        at += (size_t) written;
    }
    function->name = name; function->name_length = (uint32_t) at; return true;
}
static uint32_t spec_capture_owner(SpecContext *c, uint32_t declaration) {
    if (!c->source->declarations) return UINT32_MAX;
    uint32_t owner = c->source->declarations->functions[declaration].capture_owner;
    if (!owner) return UINT32_MAX;
    uint32_t current = c->lexical_context;
    while (current < c->count && spec_progress(c)) {
        if (c->instances[current].declaration == owner - 1) return current;
        uint32_t parent = c->identities[current].capture_owner;
        current = parent ? parent - 1 : UINT32_MAX;
    }
    if (c->diagnostic.status == XR_XIR_OK) c->diagnostic.status = XR_XIR_BAD_STRUCTURE;
    return UINT32_MAX;
}
static uint32_t spec_intern(SpecContext *c, uint32_t declaration, const XrXirType *types,
    uint32_t count) {
    uint32_t capture_owner = spec_capture_owner(c, declaration);
    if (c->diagnostic.status != XR_XIR_OK) return UINT32_MAX;
    SpecInstance key = {declaration, types, count, NULL, c->cleanup_owner, capture_owner};
    uint32_t hash = 0;
    if (!spec_instance_hash(c, &key, &hash)) return UINT32_MAX;
    uint32_t existing = spec_index_find(c, &key, hash);
    if (existing != UINT32_MAX || c->diagnostic.status != XR_XIR_OK) return existing;
    const XrXirFunction *source = &c->source->functions[declaration];
    if (c->count >= c->remaining.limits.functions || source->parameter_count > c->remaining.limits.parameters ||
        source->instruction_count > c->remaining.limits.instructions || source->block_count > c->remaining.limits.blocks) {
        c->diagnostic.status = XR_XIR_BUDGET; return UINT32_MAX;
    }
    if (!spec_function_reserve(c,c->count+1) || !spec_index_reserve(c)) return UINT32_MAX;
    c->remaining.limits.parameters -= source->parameter_count;
    c->remaining.limits.instructions -= source->instruction_count;
    c->remaining.limits.blocks -= source->block_count;
    if (!spec_progress(c)) return UINT32_MAX;
    uint32_t index = c->count++;
    XrXirType *owned_types = spec_alloc(c, count, sizeof(*types));
    if (count && !owned_types) return UINT32_MAX;
    if (count && !spec_copy(c,owned_types,types,(size_t)count*sizeof(*types))) return UINT32_MAX;
    XrXirType *type_cache = c->source->types ? spec_alloc(c, c->source->types->count, sizeof(*type_cache)) : NULL;
    if (c->source->types && c->source->types->count && !type_cache) return UINT32_MAX;
    c->instances[index] = (SpecInstance){declaration, owned_types, count, type_cache, c->cleanup_owner, capture_owner};
    XrXirFunction *to = &c->functions[index]; *to = *source;
    XrXirType *parameters = spec_alloc(c, source->parameter_count, sizeof(*parameters));
    uint32_t *consumes=spec_alloc(c,source->consume_parameter_count,sizeof(*consumes));
    uint32_t *escapes=spec_alloc(c,source->parameter_count,sizeof(*escapes));
    XrXirInstruction *ops = spec_alloc(c, source->instruction_count, sizeof(*ops));
    if (c->diagnostic.status != XR_XIR_OK) return UINT32_MAX;
    to->parameters = parameters; to->instructions = ops;
    to->consume_parameters=consumes;
    to->escaping_parameters=escapes;
    if(source->consume_parameter_count &&
        !spec_copy(c,consumes,source->consume_parameters,(size_t)source->consume_parameter_count*sizeof(*consumes)))return UINT32_MAX;

    const SpecInstance *instance = &c->instances[index];
    to->result = spec_type(c, instance, source->result);
    to->error_type = spec_type(c, instance, source->error_type);
    for (uint32_t p = 0; p < source->parameter_count; ++p) parameters[p] = spec_type(c, instance, source->parameters[p]);
    uint32_t escape_count=0;
    for (uint32_t p=0;p<source->parameter_count;++p)
        if (xr_xir_function_parameter_escaping(source,p) ||
            (!xr_xir_callable_signature(c->source->types,source->parameters[p]) &&
             xr_xir_callable_signature(&c->types,parameters[p]))) escapes[escape_count++]=p;
    to->escaping_parameter_count=escape_count;
    to->escaping_parameters=escape_count?escapes:NULL;
    for (uint32_t i = 0; i < source->instruction_count; ++i) {
        ops[i] = source->instructions[i]; ops[i].type = spec_type(c, instance, ops[i].type);
        if (ops[i].op == XR_XIR_ERROR_IS)
            ops[i].immediate = spec_type(c, instance, (XrXirType) ops[i].immediate);
    }
    if (c->diagnostic.status != XR_XIR_OK) return UINT32_MAX;
    if (!spec_name(c, to, instance)) return UINT32_MAX;
    if (c->source->declarations) {
        if (!spec_progress(c)) return UINT32_MAX;
        c->identities[index] = c->source->declarations->functions[declaration];
        if (c->identities[index].cleanup_owner) c->identities[index].cleanup_owner=c->cleanup_owner+1;
        if (c->identities[index].capture_owner) c->identities[index].capture_owner=capture_owner+1;
        if (instance->count) c->identities[index].exported=0;
    }
    if (!spec_index_place(c, c->instance_index, c->index_capacity, (SpecIndexEntry){index + 1, hash})) return UINT32_MAX;
    return index;
}
static bool spec_call(SpecContext *c, uint32_t index, uint32_t i) {
    const SpecInstance instance = c->instances[index];
    const XrXirGeneric *generic = c->source->generics ? &c->source->generics[instance.declaration] : NULL;
    XrXirFunction function = c->functions[index];
    XrXirInstruction *op = (XrXirInstruction *)&function.instructions[i];
    c->diagnostic.instruction = i;
    if (!spec_progress(c)) return false;
    if(op->op==XR_XIR_CLASS_METHOD_REF) {
        if(op->immediate<0 || (uint64_t)op->immediate>=c->source->function_count ||
            c->ordinary[op->immediate]==UINT32_MAX || op->type_arguments[0] || op->type_arguments[1]) {
            c->diagnostic.status=XR_XIR_BAD_STRUCTURE;return false;
        }
        /* Every nongeneric method is already interned in the ordinary
         * prefix. The complete closed table rederives dynamic candidates. */
        op->immediate=c->ordinary[op->immediate];return true;
    }
    if (!xr_xir_op_uses_type_arguments(op->op)) return true;
    uint32_t count = op->type_arguments[1];
    XrXirType *types = spec_alloc(c, count, sizeof(*types));
    if (count && !types) return false;
    if (!spec_progress(c)) return false;
    for (uint32_t a = 0; a < count; ++a) types[a] = spec_type(c, &instance, generic->arguments[op->type_arguments[0] + a]);
    if (c->diagnostic.status != XR_XIR_OK) return false;
    uint32_t declaration = (uint32_t)op->immediate;
    if (op->op == XR_XIR_CALL_DEFAULT || op->op == XR_XIR_INVOKE_DEFAULT) {
        const XrXirDefaultBinding *binding=NULL;
        const uint32_t *identity=xr_xir_default_identity(op);
        c->diagnostic.status=xr_xir_compile_default_lookup(&c->remaining, c->source, identity[0], identity[1], &binding);
        if(c->diagnostic.status!=XR_XIR_OK) return false;
        if(!binding) { c->diagnostic.status=XR_XIR_BAD_STRUCTURE; return false; }
        declaration=binding->function;
        if(op->op==XR_XIR_CALL_DEFAULT) { op->op=XR_XIR_CALL; op->targets[0]=op->targets[1]=0; }
        else { op->op=XR_XIR_INVOKE; op->args[0]=op->args[1]=0; }
    }
    if (op->op == XR_XIR_CALL_REQUIREMENT) {
        XrXirModule actual = {XR_XIR_CHECKED,c->functions,c->count,NULL,NULL,&c->types,NULL,XR_XIR_PROGRAM,NULL};
        XrXirProofContext context = {&actual,{XR_XIR_CONTEXT_CLOSED,0,0}};
        const XrXirInterfaceDeclaration *interface = &c->source->types->interfaces->declarations[op->targets[0]];
        const XrXirInterfaceMethod *method = &interface->methods[op->targets[1]];
        uint32_t parent = interface->parameter_count, own = method->own_parameter_count;
        if (parent > 65536 || own > 65536 - parent || count != parent + own) {
            c->diagnostic.status = XR_XIR_BAD_STRUCTURE; return false;
        }
        for (uint32_t a = 0; a < own; ++a) {
            XrXirConstraintUse use = {c->source,
                {XR_XIR_CONTEXT_INTERFACE_METHOD,op->targets[0],op->targets[1]},parent+a,types,count};
            c->diagnostic.status = xir_compile_constraints_prove_controlled(&c->remaining, &context, &use,c->control);
            if (c->diagnostic.status != XR_XIR_OK) return false;
        }
        XrXirWitnessRequest request = {c->source,
            xr_xir_operand_type(&function,function.operands[op->args[0]]),
            {op->targets[0],parent ? types : NULL,parent},op->targets[1]};
        XrXirWitness witness = {0};
        const XrXirEffects *source_effects = c->effects ? c->effects : c->verification_effects;
        if (source_effects && (request.receiver == XR_XIR_I64 || request.receiver == XR_XIR_STRING))
            c->diagnostic.status = xir_effects_scalar_witness(&c->remaining, source_effects,
                op->targets[0], op->targets[1], request.receiver, &witness.function);
        else c->diagnostic.status = xir_compile_witness_resolve_controlled(&c->remaining, &context,
            &request, &witness, c->control);
        if (c->diagnostic.status != XR_XIR_OK) return false;
        if (witness.argument_count > 65536 || own > 65536 - witness.argument_count) {
            c->diagnostic.status = XR_XIR_BAD_STRUCTURE; return false;
        }
        uint32_t complete_count = witness.argument_count + own;
        XrXirType *complete = spec_alloc(c,complete_count,sizeof(*complete));
        if ((complete_count && !complete) || !spec_progress(c)) return false;
        if (witness.argument_count && !spec_copy(c,complete,witness.arguments,(size_t)witness.argument_count*sizeof(*complete))) return false;
        if (own && !spec_copy(c,complete+witness.argument_count,types+parent,(size_t)own*sizeof(*complete))) return false;
        declaration = witness.function; types = complete; count = complete_count;
        op->op = XR_XIR_CALL; op->targets[0] = op->targets[1] = 0;
    }
    XirComptimeSelection selection={c->source,&c->types,instance.declaration,i,types,count};
    const XrXirComptimeBinding *constant=NULL;
    XrXirStatus selected=xir_comptime_select(&c->remaining,c->source_artifact->construction,&selection,&constant, c->control);
    bool evaluation_root=false;
    if (c->evaluation && selected==XR_XIR_BAD_TYPE) {
        const XrXirComptimeSite *site=xir_comptime_site(c->source_artifact->construction,instance.declaration,i);
        if (site && site->helper==declaration) {
            selected=XR_XIR_OK;evaluation_root=!c->evaluation_found;
        }
    }
    if (c->pending && selected==XR_XIR_BAD_TYPE) {
        const XrXirComptimeSite *site=xir_comptime_site(c->source_artifact->construction,instance.declaration,i);
        if (site && site->helper==declaration) {
            if (!spec_progress(c)) return false;
            c->pending->site=*site;c->pending->borrowed_arguments=types;c->pending->count=count;c->pending->found=true;
            return false;
        }
    }
    c->diagnostic.status=selected;
    if (c->diagnostic.status!=XR_XIR_OK) return false;
    if (constant) { declaration=constant->provider;types=NULL;count=0; }
    c->cleanup_owner=op->op==XR_XIR_CLEANUP_REGISTER ? index : UINT32_MAX;
    c->lexical_context=index;
    uint32_t target = spec_intern(c, declaration, types, count);
    c->cleanup_owner=UINT32_MAX;c->lexical_context=UINT32_MAX;
    if (target == UINT32_MAX) return false;
    if (evaluation_root) {
        if (!spec_progress(c)) return false;
        *c->evaluation=(XirEvaluationRoot){index,i,target};c->evaluation_found=true;
    }
    op->immediate = target; op->type_arguments[0] = op->type_arguments[1] = 0;
    return true;
}
static bool spec_calls(SpecContext *c, uint32_t index) {
    c->diagnostic.function = c->instances[index].declaration;
    XrXirDeclarations declarations = {.functions = c->identities};
    XrXirModule module = {.stage = XR_XIR_CHECKED, .functions = c->functions, .function_count = c->count,
        .declarations = c->identities ? &declarations : NULL, .types = &c->types, .linkage_kind = XR_XIR_PROGRAM};
    c->diagnostic.status = xir_effect_control_begin(c->control_builder, &module, index);
    if (c->diagnostic.status != XR_XIR_OK) return false;
    uint32_t instructions = c->functions[index].instruction_count;
    for (uint32_t i = 0; i < instructions; ++i) {
        if (!spec_call(c, index, i)) return false;
        /* Discovery may move descriptor arrays. Emit only after rebinding the
         * actual final function and identity tables, without another body walk. */
        declarations.functions = c->identities;
        module.functions = c->functions;
        module.function_count = c->count;
        c->diagnostic.status = xir_effect_control_instruction(c->control_builder, &module, index, i);
        if (c->diagnostic.status != XR_XIR_OK) return false;
    }
    c->diagnostic.status = xir_effect_control_end(c->control_builder, &module, index);
    return c->diagnostic.status == XR_XIR_OK;
}
static bool spec_declarations(SpecContext *c, XrXirDeclarations *result) {
    const XrXirDeclarations *source = c->source->declarations;
    if (!source) return true;
    *result = *source;
    result->implementations = NULL;
    XrXirSourceModule *modules = spec_alloc(c, source->module_count, sizeof(*modules));
    if (!modules || !spec_progress(c)) return false;
    if (!spec_copy(c,modules,source->modules,(size_t)source->module_count*sizeof(*modules))) return false;
    for (uint32_t m = 0; m < source->module_count; ++m) {
        if (!spec_progress(c)) return false;
        modules[m].initializer = c->ordinary[modules[m].initializer];
    }
    XrXirSlot *slots = spec_alloc(c, source->slot_count, sizeof(*slots));
    if (source->slot_count && !slots) return false;
    for (uint32_t i = 0; i < source->slot_count; ++i) {
        slots[i] = source->slots[i]; slots[i].type = spec_type(c, &c->closed, slots[i].type);
    }
    if (c->diagnostic.status != XR_XIR_OK) return false;
    result->slots = slots; result->modules = modules; result->functions = c->identities;
    result->entry_function = c->ordinary[source->entry_function]; return true;
}
static XrXirStatus spec_nominal_projection(SpecContext *c) {
    const XrXirNominalTable *source = c->source->types->nominals;
    c->types.nominals = source;
    if (!source) return XR_XIR_OK;
    bool instance = c->effects || c->source->defaults || c->source->generics ||
        c->source->types->interfaces ||
        (c->source->declarations && c->source->declarations->implementations);
    if (!instance) return XR_XIR_OK;
    XrXirNominalTable *table = spec_alloc(c,1,sizeof(*table));
    XrXirNominalDeclaration *declarations = spec_alloc(c,source->count,sizeof(*declarations));
    if (c->diagnostic.status != XR_XIR_OK) return c->diagnostic.status;
    *table = *source; table->declarations = declarations;
    for (uint32_t d = 0; d < source->count; ++d) {
        if (!spec_progress(c)) return c->diagnostic.status;
        declarations[d] = source->declarations[d];
        XrXirNominalField *fields = spec_alloc(c,declarations[d].field_count,sizeof(*fields));
        if (c->diagnostic.status != XR_XIR_OK) return c->diagnostic.status;
        declarations[d].fields = fields;
        for (uint32_t f = 0; f < declarations[d].field_count; ++f) {
            if (!spec_progress(c)) return c->diagnostic.status;
            fields[f] = source->declarations[d].fields[f];
        }
        XrXirConstraint *constraints = spec_alloc(c,declarations[d].parameter_count,sizeof(*constraints));
        if (c->diagnostic.status != XR_XIR_OK) return c->diagnostic.status;
        declarations[d].constraints = constraints;
        for (uint32_t p = 0; p < declarations[d].parameter_count; ++p) {
            if (!spec_progress(c)) return c->diagnostic.status;
            constraints[p].markers = source->declarations[d].constraints[p].markers;
        }
    }
    c->types.nominals = table;
    return XR_XIR_OK;
}
static XrXirStatus spec_nominal_seed(SpecContext *c) {
    const XrXirTypes *source = c->source->types;
    XrXirStatus status = spec_nominal_projection(c);
    if (status != XR_XIR_OK) return status;
    if (!source->count) return XR_XIR_OK;
    XrXirTypeNode *nodes = spec_alloc(c, source->count, sizeof(*nodes));
    c->closed.node_cache = spec_alloc(c, source->count, sizeof(*c->closed.node_cache));
    if (c->diagnostic.status != XR_XIR_OK) return c->diagnostic.status;
    if (!spec_progress(c)) return c->diagnostic.status;
    if (!spec_copy(c,nodes,source->nodes,(size_t)source->count*sizeof(*nodes))) return c->diagnostic.status;
    c->types.nodes = nodes; c->types.count = source->count; c->node_capacity = source->count;
    for (uint32_t i = 0; i < source->count; ++i) {
        if (!spec_progress(c)) return c->diagnostic.status;
        if (!source->nodes[i].parameter_span) c->closed.node_cache[i] = (XrXirType) (XR_XIR_CONSTRUCTED_TYPE_BASE + i);
    }
    return XR_XIR_OK;
}
static XrXirStatus spec_nominal_fields(SpecContext *c) {
    const XrXirTypes *source = c->source->types;
    if (!c->types.nominals) return XR_XIR_OK;
    for (uint32_t i = 0; i < c->types.count; ++i) {
        if (!spec_progress(c)) return c->diagnostic.status;
        /* Field substitution can grow the pool; keep only independently owned edges. */
        XrXirTypeNode node = c->types.nodes[i];
        if (node.kind != XR_XIR_TYPE_NOMINAL || node.parameter_span) continue;
        const XrXirNominalDeclaration *d = &source->nominals->declarations[node.nominal.declaration];
        if (d->field_count > c->remaining.limits.parameters) return XR_XIR_BUDGET;
        c->remaining.limits.parameters -= d->field_count;
        XrXirType *cache = spec_alloc(c, source->count, sizeof(*cache));
        XrXirType *fields = spec_alloc(c, d->field_count, sizeof(*fields));
        if (c->diagnostic.status != XR_XIR_OK) return c->diagnostic.status;
        SpecInstance instance = {node.nominal.declaration, node.nominal.arguments, node.nominal.argument_count, cache,UINT32_MAX,UINT32_MAX};
        for (uint32_t f = 0; f < d->field_count; ++f) fields[f] = spec_type(c, &instance, d->fields[f].type);
        if (c->diagnostic.status != XR_XIR_OK) return c->diagnostic.status;
        XrXirTypeNode *owned = (XrXirTypeNode *) c->types.nodes;
        owned[i].nominal.fields = fields; owned[i].nominal.field_count = d->field_count;
    }
    return xr_xir_compile_nominal_field_graph_verify(&c->remaining, &c->types);
}
static XrXirStatus spec_provenance(SpecContext *c, XrXirProvenance **output) {
    XrXirStatus allocation_status = XR_XIR_OK;
    uint64_t bytes = (uint64_t)c->count * sizeof(XrXirOrigin);
    if (bytes > SIZE_MAX) return XR_XIR_BUDGET;

    XrXirOrigin *origins = xir_compile_alloc(&c->remaining, (size_t)bytes, &allocation_status);
    if (!origins) return allocation_status;
    for (uint32_t f = 0; f < c->count; ++f) {
        if (!spec_progress(c)) {xr_compile_resources_free(origins);return c->diagnostic.status;}
        origins[f] = (XrXirOrigin) {c->instances[f].declaration, c->instances[f].arguments,
            c->instances[f].count,NULL,0};
    }
    XrXirProvenance evidence = {c->evaluation?XR_XIR_EVIDENCE_EVALUATION:XR_XIR_EVIDENCE_INSTANCE,
        (XrXirArtifact *)c->source_artifact,origins,c->count,NULL,0,NULL,0,{0}};
    if (c->evaluation) evidence.evaluation=*c->evaluation;
    const XrXirEffects *source_read=c->effects?c->effects:c->verification_effects;
    XrXirStatus status = source_read ? xr_xir_compile_provenance_copy_source_effects(
        &c->remaining,&evidence,source_read,output, c->control) :
        xr_xir_compile_provenance_copy(&c->remaining,&evidence,output, c->control);
    xr_compile_resources_free(origins);
    return status;
}
#include "xxir_specialize_materialization.inc.c"
#include "xxir_pending_prepare.inc.c"
static XrXirStatus spec_run(const XrXirArtifact *checked,XrXirArtifact **output,
    XrXirDiagnostic *diagnostic,SpecPendingPoint *pending,XirEvaluationRoot *evaluation,const XrXirSpecializationReceiver *receiver,
    XrCompileRequestControl *control) {
    if (!checked || !output || *output || (receiver && (!receiver->receive || pending || evaluation))) {
        if (diagnostic) *diagnostic = (XrXirDiagnostic){XR_XIR_BAD_STRUCTURE,
            UINT32_MAX, UINT32_MAX, UINT32_MAX, XR_XIR_DIAGNOSTIC_NONE};
        return XR_XIR_BAD_STRUCTURE;
    }
    XrXirCompileContext compile_state = checked->context;
    XrXirCompileContext *budget = &compile_state;
    SpecContext c = {0};
    XrXirArtifact *built = NULL;
    c.diagnostic = (XrXirDiagnostic) {XR_XIR_OK, UINT32_MAX, UINT32_MAX, UINT32_MAX, XR_XIR_DIAGNOSTIC_NONE};
    c.source = xr_xir_compile_artifact_module(checked);c.source_artifact=checked;c.cleanup_owner=UINT32_MAX;c.lexical_context=UINT32_MAX;
    c.remaining = *budget;
    c.pending=pending;c.evaluation=evaluation;c.control=control;
    XrXirCompileContext limits = c.remaining;

    if (!c.source || c.source->stage != XR_XIR_CHECKED || c.source->linkage_kind != XR_XIR_PROGRAM ||
        xir_artifact_is_evaluation(checked)) { c.diagnostic.status = XR_XIR_BAD_STAGE; goto done; }
    c.diagnostic.status=xir_receive_poll(control);
    if (c.diagnostic.status!=XR_XIR_OK) goto done;
    bool templated=c.source->provenance && c.source->provenance->kind==XR_XIR_EVIDENCE_TEMPLATE;
#if XR_XIR_PARANOID
    if(templated) c.diagnostic.status=xir_source_facts_acquire(checked,&c.source_facts,&c.diagnostic, control);
    else if(!c.source->provenance && c.source->declarations && c.source->declarations->implementations)
        c.diagnostic.status=xir_artifact_verify_raw_source_effects(checked,&c.verification_effects,&c.diagnostic,control);
    else c.diagnostic.status=xir_artifact_verify(checked,&c.diagnostic,control);
    if (c.diagnostic.status != XR_XIR_OK) goto done;
    c.effects=xir_source_facts_effects(c.source_facts);
#else
    if(templated) {
        c.effects=checked->checked_effects;
        if(!c.effects){c.diagnostic.status=XR_XIR_BAD_STRUCTURE;goto done;}
    }else if(!c.source->provenance&&c.source->declarations&&c.source->declarations->implementations) {
        c.diagnostic.status=xr_xir_compile_effects_infer_verified_input(&limits,
            (XirEffectInput){c.source,checked->construction,control},&c.verification_effects);
        if(c.diagnostic.status!=XR_XIR_OK)goto done;
    }
#endif
    if (xir_effect_evidence_is_instance(c.source)) {
        c.diagnostic.status=xr_xir_compile_recheck_v2(&limits, c.source, checked->construction, &built, &c.diagnostic, control); goto done;
    }
    if (pending && c.source->types) {
        c.diagnostic.status=spec_pending_seed(&c);
        if (c.diagnostic.status!=XR_XIR_OK) goto done;
    } else if (c.source->types && c.source->types->nominals) {
        c.diagnostic.status = spec_nominal_seed(&c);
        if (c.diagnostic.status != XR_XIR_OK) goto done;
    }
    if (!c.effects && !c.source->defaults && !c.source->generics && !(c.source->types && c.source->types->interfaces) &&
        !(c.source->declarations && c.source->declarations->implementations)) {
        c.diagnostic.status = spec_nominal_fields(&c);
        if (c.diagnostic.status != XR_XIR_OK) goto done;
        XrXirModule specialized = *c.source;
        if (c.types.nominals) specialized.types = &c.types;
        c.diagnostic.status = xr_xir_compile_recheck_v2(&limits, &specialized, checked->construction, &built, &c.diagnostic, control);
        goto done;
    }
    if (c.source->types && !c.types.nominals && !pending) {
        c.closed.node_cache = spec_alloc(&c, c.source->types->count, sizeof(*c.closed.node_cache));
        if (!c.closed.node_cache) goto done;
        for (uint32_t t = 0; t < c.source->types->count; ++t) {
            if (!spec_progress(&c)) goto done;
            if (!c.source->types->nodes[t].parameter_span)
                spec_type(&c, &c.closed, (XrXirType) (XR_XIR_CONSTRUCTED_TYPE_BASE + t));
        }
        if (c.diagnostic.status != XR_XIR_OK) goto done;
    }
    c.ordinary = spec_alloc(&c, c.source->function_count, sizeof(*c.ordinary));
    if (c.diagnostic.status != XR_XIR_OK) goto done;
    for (uint32_t f = 0; f < c.source->function_count; ++f) {
        if (!spec_progress(&c)) goto done;
        c.ordinary[f] = UINT32_MAX;
        if (c.source->declarations && c.source->declarations->functions[f].cleanup_owner) c.has_cleanup = true;
        bool cleanup=c.source->declarations && c.source->declarations->functions[f].cleanup_owner;
        bool captured=c.source->declarations && c.source->declarations->functions[f].capture_owner;
        if (!cleanup && !captured && (!c.source->generics || !c.source->generics[f].parameter_count)) {
            c.diagnostic.function = f;
            c.ordinary[f] = spec_intern(&c, f, NULL, 0);
            c.cleanup_owner=UINT32_MAX;
            if (c.diagnostic.status != XR_XIR_OK) goto done;
        }
    }
    if (!c.count) { c.diagnostic.status = XR_XIR_BAD_STRUCTURE; goto done; }
    XrXirDeclarations control_declarations = {.functions = c.identities};
    XrXirModule control_module = {.stage = XR_XIR_CHECKED, .functions = c.functions, .function_count = c.count,
        .declarations = c.identities ? &control_declarations : NULL, .types = &c.types, .linkage_kind = XR_XIR_PROGRAM};
    c.diagnostic.status = xir_effect_control_builder_new(&limits, &control_module, control, &c.control_builder);
    if (c.diagnostic.status != XR_XIR_OK) goto done;
    for (uint32_t f = 0; f < c.count; ++f) if (!spec_calls(&c, f)) goto done;
    if (pending) { pending->complete=true;goto done; }
    if (evaluation && !c.evaluation_found) { c.diagnostic.status=XR_XIR_BAD_TYPE;goto done; }
    XrXirDeclarations declarations = {0};
    if (!spec_declarations(&c, &declarations)) goto done;
    c.diagnostic.status = spec_nominal_fields(&c);
    if (c.diagnostic.status != XR_XIR_OK) goto done;
    XrXirModule specialized = {XR_XIR_CHECKED, c.functions, c.count, c.source->declarations ? &declarations : NULL, NULL, c.types.count || c.types.nominals ? &c.types : NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrXirProvenance *provenance = NULL;
    c.diagnostic.status = spec_provenance(&c,&provenance);
    if (c.diagnostic.status != XR_XIR_OK) goto done;
    specialized.provenance = provenance;
    c.diagnostic.status = xir_effect_control_finish(c.control_builder, &specialized, &c.control_summary);
    if (c.diagnostic.status != XR_XIR_OK) { xr_xir_compile_provenance_free(provenance); goto done; }
    XrXirConstruction *projected = NULL;
    c.diagnostic.status = spec_materialization_construction(&c,&specialized,&projected);
    if (c.diagnostic.status == XR_XIR_OK) {
        {
            XirProvenanceReceiving receiving={projected,c.effects?c.effects:c.verification_effects};
            c.diagnostic.status = xr_xir_compile_recheck_provenance_take(&limits, &specialized,
                &receiving, &provenance, &built, &c.diagnostic, control);
        }
    }
    if (c.diagnostic.status == XR_XIR_OK) {
        built->control_summary = c.control_summary;
        c.control_summary = NULL;
    }
    xr_xir_compile_construction_free(projected);
    xr_xir_compile_provenance_free(provenance);
done:
    if (pending && c.diagnostic.status==XR_XIR_OK) {
        if (pending->found) c.diagnostic.status=spec_pending_receive(&c);
        else if (!pending->complete) c.diagnostic.status=XR_XIR_BAD_STRUCTURE;
    }
    if (c.diagnostic.status==XR_XIR_OK) c.diagnostic.status=xir_receive_poll(control);
    if (c.diagnostic.status == XR_XIR_OK && receiver) {
        const XrXirModule *closed = xr_xir_compile_artifact_module(built);
        const XrXirDeclarations *a = c.source->declarations, *b = closed ? closed->declarations : NULL;
        if (!closed || (!!a != !!b) || (a && a->slot_count != b->slot_count)) c.diagnostic.status = XR_XIR_BAD_STRUCTURE;
        else {
            XrXirSpecializationMapView mapping = {checked,built,c.ordinary,c.source->function_count,
                a ? a->slot_count : 0,c.ordinary == NULL};
            c.diagnostic.status = receiver->receive(receiver->context,&mapping);
        }
    }
    if (c.diagnostic.status==XR_XIR_OK) c.diagnostic.status=xir_receive_poll(control);
    if (c.diagnostic.status == XR_XIR_OK) { *output = built; built = NULL; }
    xr_xir_compile_artifact_free(built);
    xir_source_facts_release(c.source_facts);
    xr_xir_compile_effects_free(c.verification_effects);
    xir_effect_control_builder_free(c.control_builder);
    xir_effect_control_free(c.control_summary);
    while (c.memory) { SpecMemory *next = c.memory->next; xr_compile_resources_free(c.memory); c.memory = next; }
    if (diagnostic) *diagnostic = c.diagnostic;
    return c.diagnostic.status;
}
XR_FUNC XrXirStatus xr_xir_compile_specialize(const XrXirArtifact *checked,
    XrXirArtifact **output, XrXirDiagnostic *diagnostic, XrCompileRequestControl *control) {
    return spec_run(checked,output,diagnostic,NULL,NULL,NULL,control);
}
XR_FUNC XrXirStatus xr_xir_compile_specialize_receive(const XrXirArtifact *checked,
    const XrXirSpecializationReceiver *receiver, XrXirArtifact **output, XrXirDiagnostic *diagnostic, XrCompileRequestControl *control) {
    if (!receiver || !receiver->receive) {
        if (diagnostic) *diagnostic=(XrXirDiagnostic){.status=XR_XIR_BAD_STRUCTURE};
        return XR_XIR_BAD_STRUCTURE;
    }
    return spec_run(checked,output,diagnostic,NULL,NULL,receiver,control);
}
XR_FUNC XrXirStatus xr_xir_compile_specialization_origin(const XrXirArtifact *artifact,
    uint32_t function, uint32_t *output) {
    if (!artifact || !output) return XR_XIR_BAD_STRUCTURE;
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    if (!module || module->stage != XR_XIR_CHECKED || xir_artifact_is_evaluation(artifact))
        return XR_XIR_BAD_STAGE;
    if (function >= module->function_count) return XR_XIR_BAD_VALUE;
    const XrXirProvenance *provenance = module->provenance;
    uint32_t origin = function;
    if (provenance && provenance->kind == XR_XIR_EVIDENCE_INSTANCE) {
        const XrXirModule *source = xr_xir_compile_artifact_module(provenance->source);
        if (!source || provenance->count != module->function_count || !provenance->origins)
            return XR_XIR_BAD_STRUCTURE;
        origin = provenance->origins[function].function;
        if (origin >= source->function_count) return XR_XIR_BAD_STRUCTURE;
    } else if (provenance && provenance->kind != XR_XIR_EVIDENCE_TEMPLATE) {
        return XR_XIR_BAD_STAGE;
    }
    *output = origin;
    return XR_XIR_OK;
}
#include "xxir_comptime_pending.inc.c"
