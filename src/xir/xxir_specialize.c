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
#include "xxir_compile_memory.h"
#include "xxir_operand_roles.h"
#include "xxir_types.h"
#include "xxir_internal.h"
#include "xxir_defaults_internal.h"
#include "xxir_implementation_verify.h"
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
    const XrXirEffectArgument *effects;
    uint32_t effect_count, owner;
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
    uint32_t count, capacity, cleanup_owner;
    XrXirTypes types;
    uint32_t node_capacity;
    SpecInstance closed;
    SpecTypeFrame *type_stack;
    SpecPendingPoint *pending;
    XirEvaluationRoot *evaluation;
    bool evaluation_found;
} SpecContext;
static void *spec_alloc(SpecContext *c, uint64_t count, size_t size) {
    XrXirStatus allocation_status = XR_XIR_OK;
    if (c->diagnostic.status != XR_XIR_OK || !count) return NULL;
    if (count > (SIZE_MAX - sizeof(SpecMemory)) / size ||
(sizeof(SpecMemory) + count * size > SIZE_MAX)) {
        c->diagnostic.status = XR_XIR_BUDGET; return NULL;
    }
    size_t bytes = sizeof(SpecMemory) + (size_t) count * size;
    SpecMemory *memory = xir_compile_calloc(&c->remaining, 1, bytes, &allocation_status);
    if (!memory) { c->diagnostic.status = allocation_status; return NULL; }

    memory->next = c->memory; c->memory = memory; return memory + 1;
}
static bool spec_work(SpecContext *c, uint64_t work) {
    if (c->diagnostic.status != XR_XIR_OK) return false;
    if (!xir_compile_work(&c->remaining, work)) { c->diagnostic.status = XR_XIR_BUDGET; return false; }
     return true;
}
static XrXirType spec_node(SpecContext *c, XrXirTypeNode signature) {
    for (uint32_t i = 0; i < c->types.count; ++i) {
        if (!spec_work(c, 1)) return XR_XIR_UNIT;
        const XrXirTypeNode *s = &c->types.nodes[i];
        if (s->kind != signature.kind || s->element != signature.element || s->flags != signature.flags ||
            s->parameter_count != signature.parameter_count || s->result != signature.result ||
            s->parameter_span != signature.parameter_span ||
            s->nominal.declaration != signature.nominal.declaration ||
            s->nominal.argument_count != signature.nominal.argument_count) continue;
        if (!spec_work(c, s->parameter_count)) return XR_XIR_UNIT;
        bool same = true;
        if (!spec_work(c, s->nominal.argument_count)) return XR_XIR_UNIT;
        for (uint32_t a = 0; a < s->nominal.argument_count; ++a)
            if (s->nominal.arguments[a] != signature.nominal.arguments[a]) same = false;
        for (uint32_t p = 0; p < s->parameter_count; ++p)
            if (s->parameters[p].type != signature.parameters[p].type ||
                s->parameters[p].mode != signature.parameters[p].mode) same = false;
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
        if (c->types.count) { if (!spec_work(c, c->types.count * sizeof(*table))) { return XR_XIR_UNIT; } memcpy(table, c->types.nodes, c->types.count * sizeof(*table)); }
        c->types.nodes = table; c->node_capacity = capacity;
    }
    ((XrXirTypeNode *) c->types.nodes)[c->types.count] = signature;
    return (XrXirType) (XR_XIR_CONSTRUCTED_TYPE_BASE + c->types.count++);
}
/* Source nodes are immutable; destination interning may grow its own table. */
static XrXirType *spec_type_cache(SpecContext *c, const SpecInstance *instance, uint32_t index) {
    return (c->source->types->nodes[index].parameter_span ? instance->node_cache : c->closed.node_cache) + index;
}
static bool spec_type_ready(SpecContext *c, const SpecInstance *instance, XrXirType type, XrXirType *result) {
    if (!spec_work(c, 1)) return false;
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
    if (frame->node.kind == XR_XIR_TYPE_CALLABLE || frame->node.kind == XR_XIR_TYPE_TUPLE) {
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
            source->kind == XR_XIR_TYPE_CALLABLE ? source->parameter_count + 1 :
            source->kind == XR_XIR_TYPE_TUPLE ? source->parameter_count :
            source->kind == XR_XIR_TYPE_MAP ? 2 : 1;
        if (frame->next == components) {
            *spec_type_cache(c, instance, frame->index) = spec_node(c, frame->node);
            --count; continue;
        }
        XrXirType child = source->element;
        if (source->kind == XR_XIR_TYPE_MAP && frame->next) child = source->result;
        if (source->kind == XR_XIR_TYPE_NOMINAL) child = source->nominal.arguments[frame->next];
        if (source->kind == XR_XIR_TYPE_TUPLE) child = source->parameters[frame->next].type;
        if (source->kind == XR_XIR_TYPE_CALLABLE)
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
        else if (source->kind == XR_XIR_TYPE_MAP && frame->next) frame->node.result = result;
        else if (source->kind != XR_XIR_TYPE_CALLABLE && source->kind != XR_XIR_TYPE_TUPLE) frame->node.element = result;
        else if (source->kind == XR_XIR_TYPE_CALLABLE && frame->next == source->parameter_count) frame->node.result = result;
        else {
            XrXirCallableParameter *parameters = (XrXirCallableParameter *) frame->node.parameters;
            parameters[frame->next] = source->parameters[frame->next];
            parameters[frame->next].type = result;
        }
        ++frame->next;
    }
    return c->diagnostic.status == XR_XIR_OK ?
        *spec_type_cache(c, instance, (uint32_t) type - XR_XIR_CONSTRUCTED_TYPE_BASE) : XR_XIR_UNIT;
}

#include "xxir_effect_instance_key.inc.c"
/* Hash only selects candidates in this request's destination type pool.
 * Explicit scalar fields exclude padding; ordered vectors retain every element. */
static bool spec_instance_hash(SpecContext *c, const SpecInstance *key, uint32_t *output) {
    uint32_t header[] = {key->declaration, key->count, key->effect_count, key->owner};
    if (!spec_work(c, 4 + 3 * sizeof(header))) return false;
    uint32_t hash = xr_hash_bytes(header, sizeof(header));
    if (key->count) {
        uint64_t bytes = (uint64_t)key->count * sizeof(*key->arguments);
        if (bytes > SIZE_MAX || !spec_work(c, key->count + 3 * bytes + 3 * sizeof(uint32_t) * 2)) return false;
        uint32_t pair[] = {hash, xr_hash_bytes(key->arguments, (size_t)bytes)};
        hash = xr_hash_bytes(pair, sizeof(pair));
    }
    for (uint32_t a = 0; a < key->effect_count; ++a) {
        uint32_t row[] = {hash, key->effects[a].parameter, (uint32_t)key->effects[a].type};
        if (!spec_work(c, 2 + 3 * sizeof(row))) return false;
        hash = xr_hash_bytes(row, sizeof(row));
    }
    if (!spec_work(c, sizeof(*output))) return false;
    *output = hash; return true;
}
static bool spec_instance_equal(SpecContext *c, const SpecInstance *a, const SpecInstance *b) {
    if (!spec_work(c, 4)) return false;
    if (a->declaration != b->declaration || a->count != b->count ||
        a->effect_count != b->effect_count || a->owner != b->owner) return false;
    for (uint32_t i = 0; i < a->count; ++i) {
        if (!spec_work(c, 1)) return false;
        if (a->arguments[i] != b->arguments[i]) return false;
    }
    return spec_effect_same(c, a, b->effects, b->effect_count);
}
static uint32_t spec_index_find(SpecContext *c, const SpecInstance *key, uint32_t hash) {
    if (!c->index_capacity) return UINT32_MAX;
    if (!spec_work(c, 2)) return UINT32_MAX;
    uint32_t slot = hash & (c->index_capacity - 1);
    for (uint32_t probe = 0; probe < c->index_capacity; ++probe) {
        if (!spec_work(c, 4)) return UINT32_MAX;
        SpecIndexEntry entry = c->instance_index[slot];
        if (!entry.ordinal) return UINT32_MAX;
        if (entry.hash == hash && spec_instance_equal(c, key, &c->instances[entry.ordinal - 1]))
            return entry.ordinal - 1;
        if (c->diagnostic.status != XR_XIR_OK || !spec_work(c, 3)) return UINT32_MAX;
        slot = (slot + 1) & (c->index_capacity - 1);
    }
    c->diagnostic.status = XR_XIR_BAD_STRUCTURE; return UINT32_MAX;
}
/* Existing entries already have distinct complete keys. Rehashing moves only
 * cached hashes and ordinals, never element pointers or semantic identities. */
static bool spec_index_place(SpecContext *c, SpecIndexEntry *table, uint32_t capacity, SpecIndexEntry entry) {
    if (!spec_work(c, 2)) return false;
    uint32_t slot = entry.hash & (capacity - 1);
    for (uint32_t probe = 0; probe < capacity; ++probe) {
        if (!spec_work(c, 2)) return false;
        if (!table[slot].ordinal) {
            if (!spec_work(c, sizeof(entry))) return false;
            table[slot] = entry; return true;
        }
        if (!spec_work(c, 3)) return false;
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
        if (!spec_work(c, 2)) return false;
        SpecIndexEntry entry = c->instance_index[slot];
        if (entry.ordinal && !spec_index_place(c, table, capacity, entry)) return false;
    }
    if (!spec_work(c, sizeof(c->instance_index) + sizeof(c->index_capacity))) return false;
    c->instance_index = table; c->index_capacity = capacity; return true;
}


static bool spec_name(SpecContext *c, XrXirFunction *function, const SpecInstance *instance) {
    if (!instance->count && !instance->effect_count) return true;
    if (c->effects && !instance->count) {
        bool is_default=false;
        if (!spec_effect_default(c,instance,&is_default)) return false;
        if (is_default) return true;
    }
    uint64_t capacity = (uint64_t) function->name_length + 32 + (uint64_t) instance->count * 12 +
        (uint64_t)instance->effect_count*24;
    if (capacity > UINT32_MAX) { c->diagnostic.status = XR_XIR_BUDGET; return false; }
    char *name = spec_alloc(c, capacity, 1); if (!name) return false;
    { if (!spec_work(c, function->name_length)) { return false; } memcpy(name, function->name, function->name_length); }
    size_t at = function->name_length;
    int written = snprintf(name + at, (size_t) capacity - at, "$%u", instance->declaration);
    if (written < 0 || (size_t) written >= capacity - at) { c->diagnostic.status = XR_XIR_BAD_STRUCTURE; return false; }
    at += (size_t) written;
    for (uint32_t i = 0; i < instance->count; ++i) {
        written = snprintf(name + at, (size_t) capacity - at, ":%u", (unsigned) instance->arguments[i]);
        if (written < 0 || (size_t) written >= capacity - at) { c->diagnostic.status = XR_XIR_BAD_STRUCTURE; return false; }
        at += (size_t) written;
    }
    for (uint32_t i=0;i<instance->effect_count;++i) {
        written=snprintf(name+at,(size_t)capacity-at,"@%u:%u",instance->effects[i].parameter,
            (unsigned)instance->effects[i].type);
        if (written<0 || (size_t)written>=capacity-at) { c->diagnostic.status=XR_XIR_BAD_STRUCTURE;return false; }
        if (!spec_work(c,(uint64_t)written)) return false;
        at+=(size_t)written;
    }
    function->name = name; function->name_length = (uint32_t) at; return true;
}
static uint32_t spec_intern(SpecContext *c, uint32_t declaration, const XrXirType *types,
    uint32_t count, const XrXirEffectArgument *effects, uint32_t effect_count) {
    SpecInstance key = {declaration, types, count, NULL, effects, effect_count, c->cleanup_owner};
    uint32_t hash = 0;
    if (!spec_instance_hash(c, &key, &hash)) return UINT32_MAX;
    uint32_t existing = spec_index_find(c, &key, hash);
    if (existing != UINT32_MAX || c->diagnostic.status != XR_XIR_OK) return existing;
    const XrXirFunction *source = &c->source->functions[declaration];
    if (c->count == c->capacity || source->parameter_count > c->remaining.limits.parameters ||
        source->instruction_count > c->remaining.limits.instructions || source->block_count > c->remaining.limits.blocks) {
        c->diagnostic.status = XR_XIR_BUDGET; return UINT32_MAX;
    }
    if (!spec_index_reserve(c)) return UINT32_MAX;
    c->remaining.limits.parameters -= source->parameter_count;
    c->remaining.limits.instructions -= source->instruction_count;
    c->remaining.limits.blocks -= source->block_count;
    if (!spec_work(c, (uint64_t) source->parameter_count + source->instruction_count + count + source->name_length)) return UINT32_MAX;
    uint32_t index = c->count++;
    XrXirType *owned_types = spec_alloc(c, count, sizeof(*types));
    if (count && !owned_types) return UINT32_MAX;
    if (count) { if (!spec_work(c, count * sizeof(*types))) { return UINT32_MAX; } memcpy(owned_types, types, count * sizeof(*types)); }
    XrXirType *type_cache = c->source->types ? spec_alloc(c, c->source->types->count, sizeof(*type_cache)) : NULL;
    if (c->source->types && c->source->types->count && !type_cache) return UINT32_MAX;
    XrXirEffectArgument *owned_effects=spec_alloc(c,effect_count,sizeof(*owned_effects));
    if (effect_count && !owned_effects) return UINT32_MAX;
    if (effect_count) {
        if (!spec_work(c,(uint64_t)effect_count*sizeof(*owned_effects))) return UINT32_MAX;
        memcpy(owned_effects,effects,(size_t)effect_count*sizeof(*owned_effects));
    }
    c->instances[index] = (SpecInstance) {declaration, owned_types, count, type_cache,owned_effects,effect_count,c->cleanup_owner};
    XrXirFunction *to = &c->functions[index]; *to = *source;
    XrXirType *parameters = spec_alloc(c, source->parameter_count, sizeof(*parameters));
    XrXirInstruction *ops = spec_alloc(c, source->instruction_count, sizeof(*ops));
    if (c->diagnostic.status != XR_XIR_OK) return UINT32_MAX;
    to->parameters = parameters; to->instructions = ops;
    const SpecInstance *instance = &c->instances[index];
    to->result = spec_type(c, instance, source->result);
    for (uint32_t p = 0; p < source->parameter_count; ++p) parameters[p] = spec_type(c, instance, source->parameters[p]);
    for (uint32_t a=0;a<effect_count;++a) {
        if (!spec_work(c,2)) return UINT32_MAX;
        if (owned_effects[a].parameter>=source->parameter_count) { c->diagnostic.status=XR_XIR_BAD_STRUCTURE;return UINT32_MAX; }
        parameters[owned_effects[a].parameter]=owned_effects[a].type;
    }
    for (uint32_t i = 0; i < source->instruction_count; ++i) {
        ops[i] = source->instructions[i]; ops[i].type = spec_type(c, instance, ops[i].type);
        if (ops[i].op == XR_XIR_ERROR_IS)
            ops[i].immediate = spec_type(c, instance, (XrXirType) ops[i].immediate);
    }
    if (c->diagnostic.status != XR_XIR_OK) return UINT32_MAX;
    if (c->source->declarations)
        c->identities[index]=c->source->declarations->functions[declaration];
    if (c->source->declarations && c->source->declarations->functions[declaration].cleanup_owner)
        c->identities[index].cleanup_owner=c->cleanup_owner+1;
    if (!spec_effect_flow(c,index)) return UINT32_MAX;
    if (!spec_name(c, to, instance)) return UINT32_MAX;
    if (c->source->declarations) {
        if (!spec_work(c, sizeof(*c->identities))) return UINT32_MAX;
        c->identities[index] = c->source->declarations->functions[declaration];
        if (c->identities[index].cleanup_owner) c->identities[index].cleanup_owner=c->cleanup_owner+1;
        if (c->effects) {
            bool is_default=false;
            if (!spec_effect_default(c,instance,&is_default)) return UINT32_MAX;
            if (!is_default) {
                if (!spec_work(c,1)) return UINT32_MAX;
                c->identities[index].exported=0;
            }
        }
    }
    if (!spec_index_place(c, c->instance_index, c->index_capacity, (SpecIndexEntry){index + 1, hash})) return UINT32_MAX;
    return index;
}
static bool spec_calls(SpecContext *c, uint32_t index) {
    const SpecInstance *instance = &c->instances[index];
    const XrXirGeneric *generic = c->source->generics ? &c->source->generics[instance->declaration] : NULL;
    XrXirFunction *function = &c->functions[index];
    XrXirInstruction *ops = (XrXirInstruction *) function->instructions;
    c->diagnostic.function = instance->declaration;
    for (uint32_t i = 0; i < function->instruction_count; ++i) {
        c->diagnostic.instruction = i;
        if (!spec_work(c, 1)) return false;
        XrXirInstruction *op = &ops[i];
        if (!xr_xir_op_uses_type_arguments(op->op)) continue;
        uint32_t count = op->type_arguments[1];
        XrXirType *types = spec_alloc(c, count, sizeof(*types));
        if (count && !types) return false;
        if (!spec_work(c, count)) return false;
        for (uint32_t a = 0; a < count; ++a) types[a] = spec_type(c, instance, generic->arguments[op->type_arguments[0] + a]);
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
                c->diagnostic.status = xr_xir_compile_constraints_prove(&c->remaining, &context, &use);
                if (c->diagnostic.status != XR_XIR_OK) return false;
            }
            XrXirWitnessRequest request = {c->source,
                xr_xir_operand_type(function,function->operands[op->args[0]]),
                {op->targets[0],parent ? types : NULL,parent},op->targets[1]};
            XrXirWitness witness = {0};
            c->diagnostic.status = xr_xir_compile_witness_resolve(&c->remaining, &context, &request, &witness);
            if (c->diagnostic.status != XR_XIR_OK) return false;
            if (witness.argument_count > 65536 || own > 65536 - witness.argument_count) {
                c->diagnostic.status = XR_XIR_BAD_STRUCTURE; return false;
            }
            uint32_t complete_count = witness.argument_count + own;
            XrXirType *complete = spec_alloc(c,complete_count,sizeof(*complete));
            if ((complete_count && !complete) || !spec_work(c,complete_count)) return false;
            if (witness.argument_count) { if (!spec_work(c, witness.argument_count*sizeof(*complete))) { return false; } memcpy(complete,witness.arguments,witness.argument_count*sizeof(*complete)); }
            if (own) { if (!spec_work(c, own*sizeof(*complete))) { return false; } memcpy(complete+witness.argument_count,types+parent,own*sizeof(*complete)); }
            declaration = witness.function; types = complete; count = complete_count;
            op->op = XR_XIR_CALL; op->targets[0] = op->targets[1] = 0;
        }
        XirComptimeSelection selection={c->source,&c->types,instance->declaration,i,types,count};
        const XrXirComptimeBinding *constant=NULL;
        XrXirStatus selected=xir_comptime_select(&c->remaining,c->source_artifact->construction,&selection,&constant);
        bool evaluation_root=false;
        if (c->evaluation && selected==XR_XIR_BAD_TYPE) {
            const XrXirComptimeSite *site=xir_comptime_site(c->source_artifact->construction,instance->declaration,i);
            if (site && site->helper==declaration) {
                selected=XR_XIR_OK;evaluation_root=!c->evaluation_found;
            }
        }
        if (c->pending && selected==XR_XIR_BAD_TYPE) {
            const XrXirComptimeSite *site=xir_comptime_site(c->source_artifact->construction,instance->declaration,i);
            if (site && site->helper==declaration) {
                if (!spec_work(c,sizeof(*c->pending))) return false;
                c->pending->site=*site;c->pending->borrowed_arguments=types;c->pending->count=count;c->pending->found=true;
                return false;
            }
        }
        c->diagnostic.status=selected;
        if (c->diagnostic.status!=XR_XIR_OK) return false;
        if (constant) { declaration=constant->provider;types=NULL;count=0; }
        SpecEffectInput input={function,op,index,i};SpecEffectVector effect={0};
        if (!spec_effect_vector(c,declaration,types,count,constant?NULL:&input,&effect)) return false;
        c->cleanup_owner=op->op==XR_XIR_CLEANUP_REGISTER ? index : UINT32_MAX;
        uint32_t target = spec_intern(c, declaration, types, count,effect.arguments,effect.count);
        c->cleanup_owner=UINT32_MAX;
        if (target == UINT32_MAX) return false;
        if (evaluation_root) {
            if (!spec_work(c,sizeof(*c->evaluation))) return false;
            *c->evaluation=(XirEvaluationRoot){index,i,target};c->evaluation_found=true;
        }
        op->immediate = target; op->type_arguments[0] = op->type_arguments[1] = 0;
    }
    return true;
}
static bool spec_declarations(SpecContext *c, XrXirDeclarations *result) {
    const XrXirDeclarations *source = c->source->declarations;
    if (!source) return true;
    *result = *source;
    result->implementations = NULL;
    XrXirSourceModule *modules = spec_alloc(c, source->module_count, sizeof(*modules));
    if (!modules || !spec_work(c, source->module_count)) return false;
    { if (!spec_work(c, source->module_count * sizeof(*modules))) { return false; } memcpy(modules, source->modules, source->module_count * sizeof(*modules)); }
    for (uint32_t m = 0; m < source->module_count; ++m) modules[m].initializer = c->ordinary[modules[m].initializer];
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
        if (!spec_work(c,1)) return c->diagnostic.status;
        declarations[d] = source->declarations[d];
        XrXirNominalField *fields = spec_alloc(c,declarations[d].field_count,sizeof(*fields));
        if (c->diagnostic.status != XR_XIR_OK) return c->diagnostic.status;
        declarations[d].fields = fields;
        for (uint32_t f = 0; f < declarations[d].field_count; ++f) {
            if (!spec_work(c,sizeof(*fields))) return c->diagnostic.status;
            fields[f] = source->declarations[d].fields[f];
        }
        XrXirConstraint *constraints = spec_alloc(c,declarations[d].parameter_count,sizeof(*constraints));
        if (c->diagnostic.status != XR_XIR_OK) return c->diagnostic.status;
        declarations[d].constraints = constraints;
        for (uint32_t p = 0; p < declarations[d].parameter_count; ++p) {
            if (!spec_work(c,1)) return c->diagnostic.status;
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
    if (!spec_work(c, source->count)) return c->diagnostic.status;
    { if (!spec_work(c, (size_t) source->count * sizeof(*nodes))) { return c->diagnostic.status; } memcpy(nodes, source->nodes, (size_t) source->count * sizeof(*nodes)); }
    c->types.nodes = nodes; c->types.count = source->count; c->node_capacity = source->count;
    for (uint32_t i = 0; i < source->count; ++i)
        if (!source->nodes[i].parameter_span) c->closed.node_cache[i] = (XrXirType) (XR_XIR_CONSTRUCTED_TYPE_BASE + i);
    return XR_XIR_OK;
}
static XrXirStatus spec_nominal_fields(SpecContext *c) {
    const XrXirTypes *source = c->source->types;
    if (!c->types.nominals) return XR_XIR_OK;
    for (uint32_t i = 0; i < c->types.count; ++i) {
        if (!spec_work(c, 1)) return c->diagnostic.status;
        /* Field substitution can grow the pool; keep only independently owned edges. */
        XrXirTypeNode node = c->types.nodes[i];
        if (node.kind != XR_XIR_TYPE_NOMINAL || node.parameter_span) continue;
        const XrXirNominalDeclaration *d = &source->nominals->declarations[node.nominal.declaration];
        if (d->field_count > c->remaining.limits.parameters) return XR_XIR_BUDGET;
        c->remaining.limits.parameters -= d->field_count;
        XrXirType *cache = spec_alloc(c, source->count, sizeof(*cache));
        XrXirType *fields = spec_alloc(c, d->field_count, sizeof(*fields));
        if (c->diagnostic.status != XR_XIR_OK) return c->diagnostic.status;
        SpecInstance instance = {node.nominal.declaration, node.nominal.arguments, node.nominal.argument_count, cache,NULL,0,UINT32_MAX};
        for (uint32_t f = 0; f < d->field_count; ++f) fields[f] = spec_type(c, &instance, d->fields[f].type);
        if (c->diagnostic.status != XR_XIR_OK) return c->diagnostic.status;
        XrXirTypeNode *owned = (XrXirTypeNode *) c->types.nodes;
        owned[i].nominal.fields = fields; owned[i].nominal.field_count = d->field_count;
    }
    return XR_XIR_OK;
}
static XrXirStatus spec_provenance(SpecContext *c, XrXirProvenance **output) {
    XrXirStatus allocation_status = XR_XIR_OK;
    uint64_t bytes = (uint64_t)c->count * sizeof(XrXirOrigin);
    if (bytes > SIZE_MAX) return XR_XIR_BUDGET;

    XrXirOrigin *origins = xir_compile_alloc(&c->remaining, (size_t)bytes, &allocation_status);
    if (!origins) return allocation_status;
    for (uint32_t f = 0; f < c->count; ++f)
        origins[f] = (XrXirOrigin) {c->instances[f].declaration, c->instances[f].arguments,
            c->instances[f].count,c->instances[f].effects,c->instances[f].effect_count};
    XrXirProvenance evidence = {c->evaluation?XR_XIR_EVIDENCE_EVALUATION:XR_XIR_EVIDENCE_INSTANCE,
        (XrXirArtifact *)c->source_artifact,origins,c->count,NULL,0,NULL,0,{0}};
    if (c->evaluation) evidence.evaluation=*c->evaluation;
    if (!spec_effect_proofs(c,&evidence.bindings,&evidence.binding_count)) {
        xr_compile_resources_free(origins);return c->diagnostic.status;
    }
    XrXirStatus status = c->effects ? xr_xir_compile_provenance_copy_source_effects(
        &c->remaining,&evidence,c->effects,output) :
        xr_xir_compile_provenance_copy(&c->remaining,&evidence,output);
    xr_compile_resources_free(origins);
    return status;
}
#include "xxir_specialize_materialization.inc.c"
#include "xxir_pending_prepare.inc.c"
static XrXirStatus spec_run(const XrXirArtifact *checked,XrXirArtifact **output,
    XrXirDiagnostic *diagnostic,SpecPendingPoint *pending,XirEvaluationRoot *evaluation) {
    if (!checked || !output || *output) {
        if (diagnostic) *diagnostic = (XrXirDiagnostic){XR_XIR_BAD_STRUCTURE,
            UINT32_MAX, UINT32_MAX, UINT32_MAX, XR_XIR_DIAGNOSTIC_NONE};
        return XR_XIR_BAD_STRUCTURE;
    }
    XrXirCompileContext compile_state = checked->context;
    XrXirCompileContext *budget = &compile_state;
    SpecContext c = {0};
    c.diagnostic = (XrXirDiagnostic) {XR_XIR_OK, UINT32_MAX, UINT32_MAX, UINT32_MAX, XR_XIR_DIAGNOSTIC_NONE};
    c.source = xr_xir_compile_artifact_module(checked);c.source_artifact=checked;c.cleanup_owner=UINT32_MAX;
    c.remaining = *budget;
    c.pending=pending;c.evaluation=evaluation;
    XrXirCompileContext limits = c.remaining;

    if (!c.source || c.source->stage != XR_XIR_CHECKED || c.source->linkage_kind != XR_XIR_PROGRAM ||
        xir_artifact_is_evaluation(checked)) { c.diagnostic.status = XR_XIR_BAD_STAGE; goto done; }
    bool templated=c.source->provenance && c.source->provenance->kind==XR_XIR_EVIDENCE_TEMPLATE;
    c.diagnostic.status = templated ?
        xir_source_facts_acquire(checked,&c.source_facts,&c.diagnostic) :
        xr_xir_compile_artifact_verify(checked,&c.diagnostic);
    if (c.diagnostic.status != XR_XIR_OK) goto done;
    c.effects=xir_source_facts_effects(c.source_facts);
    if (xir_effect_evidence_is_instance(c.source)) {
        c.diagnostic.status=xr_xir_compile_recheck_v2(&limits, c.source, checked->construction, output, &c.diagnostic); goto done;
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
        c.diagnostic.status = xr_xir_compile_recheck_v2(&limits, &specialized, checked->construction, output, &c.diagnostic);
        goto done;
    }
    if (c.source->types && !c.types.nominals && !pending) {
        c.closed.node_cache = spec_alloc(&c, c.source->types->count, sizeof(*c.closed.node_cache));
        if (!c.closed.node_cache) goto done;
        for (uint32_t t = 0; t < c.source->types->count; ++t)
            if (!c.source->types->nodes[t].parameter_span)
                spec_type(&c, &c.closed, (XrXirType) (XR_XIR_CONSTRUCTED_TYPE_BASE + t));
        if (c.diagnostic.status != XR_XIR_OK) goto done;
    }
    c.capacity = limits.limits.functions;
    c.functions = spec_alloc(&c, c.capacity, sizeof(*c.functions));
    c.identities = c.source->declarations ? spec_alloc(&c, c.capacity, sizeof(*c.identities)) : NULL;
    c.instances = spec_alloc(&c, c.capacity, sizeof(*c.instances));
    c.ordinary = spec_alloc(&c, c.source->function_count, sizeof(*c.ordinary));
    if (c.diagnostic.status != XR_XIR_OK) goto done;
    for (uint32_t f = 0; f < c.source->function_count; ++f) {
        c.ordinary[f] = UINT32_MAX;
        if (c.source->declarations && c.source->declarations->functions[f].cleanup_owner) c.has_cleanup = true;
        bool cleanup=c.source->declarations && c.source->declarations->functions[f].cleanup_owner;
        if (!cleanup && (!c.source->generics || !c.source->generics[f].parameter_count)) {
            c.diagnostic.function = f;
            SpecEffectVector effect={0};
            if (!spec_effect_vector(&c,f,NULL,0,NULL,&effect)) goto done;
            c.ordinary[f] = spec_intern(&c, f, NULL, 0,effect.arguments,effect.count);
            c.cleanup_owner=UINT32_MAX;
            if (c.diagnostic.status != XR_XIR_OK) goto done;
        }
    }
    if (!c.count) { c.diagnostic.status = XR_XIR_BAD_STRUCTURE; goto done; }
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
    XrXirConstruction *projected = NULL;
    c.diagnostic.status = spec_materialization_construction(&c,&specialized,&projected);
    if (c.diagnostic.status == XR_XIR_OK) {
        if (!xir_compile_work(&limits,sizeof(XirProvenanceReceiving))) c.diagnostic.status=XR_XIR_BUDGET;
        else {
            XirProvenanceReceiving receiving={projected,c.effects};
            c.diagnostic.status = xr_xir_compile_recheck_provenance_take(&limits, &specialized,
                &receiving, &provenance, output, &c.diagnostic);
        }
    }
    xr_xir_compile_construction_free(projected);
    xr_xir_compile_provenance_free(provenance);
done:
    if (pending && c.diagnostic.status==XR_XIR_OK) {
        if (pending->found) c.diagnostic.status=spec_pending_receive(&c);
        else if (!pending->complete) c.diagnostic.status=XR_XIR_BAD_STRUCTURE;
    }
    xir_source_facts_release(c.source_facts);
    while (c.memory) { SpecMemory *next = c.memory->next; xr_compile_resources_free(c.memory); c.memory = next; }
    if (diagnostic) *diagnostic = c.diagnostic;
    return c.diagnostic.status;
}
XR_FUNC XrXirStatus xr_xir_compile_specialize(const XrXirArtifact *checked,XrXirArtifact **output,XrXirDiagnostic *diagnostic) {
    return spec_run(checked,output,diagnostic,NULL,NULL);
}
#include "xxir_comptime_pending.inc.c"
