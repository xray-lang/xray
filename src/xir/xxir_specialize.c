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
#include "xxir_types.h"
#include "xxir_internal.h"
#include "../base/xmalloc.h"
#include <stdio.h>
typedef struct SpecMemory { struct SpecMemory *next; } SpecMemory;
typedef struct SpecInstance { uint32_t declaration; const XrXirType *arguments; uint32_t count; XrXirType *node_cache; } SpecInstance;
typedef struct SpecTypeFrame { uint32_t index, next; XrXirTypeNode node; } SpecTypeFrame;
typedef struct SpecContext {
    const XrXirModule *source;
    XrXirBudget remaining;
    XrXirDiagnostic diagnostic;
    SpecMemory *memory;
    XrXirFunction *functions;
    XrXirFunctionIdentity *identities;
    SpecInstance *instances;
    uint32_t *ordinary;
    uint32_t count, capacity;
    XrXirTypes types;
    uint32_t node_capacity;
    SpecInstance closed;
    SpecTypeFrame *type_stack;
} SpecContext;
static void *spec_alloc(SpecContext *c, uint64_t count, size_t size) {
    if (c->diagnostic.status != XR_XIR_OK || !count) return NULL;
    if (count > (SIZE_MAX - sizeof(SpecMemory)) / size ||
        sizeof(SpecMemory) + count * size > c->remaining.metadata_bytes) {
        c->diagnostic.status = XR_XIR_BUDGET; return NULL;
    }
    size_t bytes = sizeof(SpecMemory) + (size_t) count * size;
    SpecMemory *memory = xr_calloc(1, bytes);
    if (!memory) { c->diagnostic.status = XR_XIR_OUT_OF_MEMORY; return NULL; }
    c->remaining.metadata_bytes -= bytes;
    memory->next = c->memory; c->memory = memory; return memory + 1;
}
static bool spec_work(SpecContext *c, uint64_t work) {
    if (c->diagnostic.status != XR_XIR_OK) return false;
    if (work > c->remaining.work) { c->diagnostic.status = XR_XIR_BUDGET; return false; }
    c->remaining.work -= work; return true;
}
static XrXirType spec_node(SpecContext *c, XrXirTypeNode signature) {
    for (uint32_t i = 0; i < c->types.count; ++i) {
        if (!spec_work(c, 1)) return XR_XIR_UNIT;
        const XrXirTypeNode *s = &c->types.nodes[i];
        if (s->kind != signature.kind || s->element != signature.element || s->flags != signature.flags ||
            s->parameter_count != signature.parameter_count || s->result != signature.result) continue;
        if (!spec_work(c, s->parameter_count)) return XR_XIR_UNIT;
        bool same = true;
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
        if (c->types.count) memcpy(table, c->types.nodes, c->types.count * sizeof(*table));
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
    if (frame->node.kind == XR_XIR_TYPE_CALLABLE) {
        uint32_t n = frame->node.parameter_count;
        XrXirCallableParameter *parameters = spec_alloc(c, n, sizeof(*parameters));
        if (n && !parameters) return false;
        frame->node.parameters = parameters;
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
        uint32_t components = source->kind == XR_XIR_TYPE_CALLABLE ? source->parameter_count + 1 : 1;
        if (frame->next == components) {
            *spec_type_cache(c, instance, frame->index) = spec_node(c, frame->node);
            --count; continue;
        }
        XrXirType child = source->element;
        if (source->kind == XR_XIR_TYPE_CALLABLE)
            child = frame->next < source->parameter_count ? source->parameters[frame->next].type : source->result;
        if (!spec_type_ready(c, instance, child, &result)) {
            if (c->diagnostic.status != XR_XIR_OK) break;
            uint32_t index = (uint32_t) child - XR_XIR_CONSTRUCTED_TYPE_BASE;
            if (index >= frame->index) { c->diagnostic.status = XR_XIR_BAD_TYPE; break; }
            if (!spec_type_push(c, index, &count)) break;
            continue;
        }
        if (source->kind != XR_XIR_TYPE_CALLABLE) frame->node.element = result;
        else if (frame->next == source->parameter_count) frame->node.result = result;
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

static bool spec_name(SpecContext *c, XrXirFunction *function, const SpecInstance *instance) {
    if (!instance->count) return true;
    uint64_t capacity = (uint64_t) function->name_length + 32 + (uint64_t) instance->count * 12;
    if (capacity > UINT32_MAX) { c->diagnostic.status = XR_XIR_BUDGET; return false; }
    char *name = spec_alloc(c, capacity, 1); if (!name) return false;
    memcpy(name, function->name, function->name_length);
    size_t at = function->name_length;
    int written = snprintf(name + at, (size_t) capacity - at, "$%u", instance->declaration);
    if (written < 0 || (size_t) written >= capacity - at) { c->diagnostic.status = XR_XIR_BAD_STRUCTURE; return false; }
    at += (size_t) written;
    for (uint32_t i = 0; i < instance->count; ++i) {
        written = snprintf(name + at, (size_t) capacity - at, ":%u", (unsigned) instance->arguments[i]);
        if (written < 0 || (size_t) written >= capacity - at) { c->diagnostic.status = XR_XIR_BAD_STRUCTURE; return false; }
        at += (size_t) written;
    }
    function->name = name; function->name_length = (uint32_t) at; return true;
}
static uint32_t spec_intern(SpecContext *c, uint32_t declaration, const XrXirType *types, uint32_t count) {
    for (uint32_t i = 0; i < c->count; ++i) {
        if (!spec_work(c, (uint64_t) count + 1)) return UINT32_MAX;
        if (c->instances[i].declaration == declaration && c->instances[i].count == count &&
            (!count || !memcmp(c->instances[i].arguments, types, count * sizeof(*types)))) return i;
    }
    const XrXirFunction *source = &c->source->functions[declaration];
    if (c->count == c->capacity || source->parameter_count > c->remaining.parameters ||
        source->instruction_count > c->remaining.instructions || source->block_count > c->remaining.blocks) {
        c->diagnostic.status = XR_XIR_BUDGET; return UINT32_MAX;
    }
    c->remaining.parameters -= source->parameter_count;
    c->remaining.instructions -= source->instruction_count;
    c->remaining.blocks -= source->block_count;
    if (!spec_work(c, (uint64_t) source->parameter_count + source->instruction_count + count + source->name_length)) return UINT32_MAX;
    uint32_t index = c->count++;
    XrXirType *owned_types = spec_alloc(c, count, sizeof(*types));
    if (count && !owned_types) return UINT32_MAX;
    if (count) memcpy(owned_types, types, count * sizeof(*types));
    XrXirType *type_cache = c->source->types ? spec_alloc(c, c->source->types->count, sizeof(*type_cache)) : NULL;
    if (c->source->types && c->source->types->count && !type_cache) return UINT32_MAX;
    c->instances[index] = (SpecInstance) {declaration, owned_types, count, type_cache};
    XrXirFunction *to = &c->functions[index]; *to = *source;
    XrXirType *parameters = spec_alloc(c, source->parameter_count, sizeof(*parameters));
    XrXirInstruction *ops = spec_alloc(c, source->instruction_count, sizeof(*ops));
    if (c->diagnostic.status != XR_XIR_OK) return UINT32_MAX;
    to->parameters = parameters; to->instructions = ops;
    const SpecInstance *instance = &c->instances[index];
    to->result = spec_type(c, instance, source->result);
    for (uint32_t p = 0; p < source->parameter_count; ++p) parameters[p] = spec_type(c, instance, source->parameters[p]);
    for (uint32_t i = 0; i < source->instruction_count; ++i) {
        ops[i] = source->instructions[i]; ops[i].type = spec_type(c, instance, ops[i].type);
    }
    if (c->diagnostic.status != XR_XIR_OK) return UINT32_MAX;
    if (!spec_name(c, to, instance)) return UINT32_MAX;
    if (c->source->declarations) c->identities[index] = c->source->declarations->functions[declaration];
    return index;
}
static bool spec_calls(SpecContext *c, uint32_t index) {
    const SpecInstance *instance = &c->instances[index];
    const XrXirGeneric *generic = &c->source->generics[instance->declaration];
    XrXirFunction *function = &c->functions[index];
    XrXirInstruction *ops = (XrXirInstruction *) function->instructions;
    c->diagnostic.function = instance->declaration;
    for (uint32_t i = 0; i < function->instruction_count; ++i) {
        c->diagnostic.instruction = i;
        if (!spec_work(c, 1)) return false;
        XrXirInstruction *op = &ops[i];
        if (op->op != XR_XIR_CALL && op->op != XR_XIR_FUNCTION_REF) continue;
        uint32_t count = op->targets[1];
        XrXirType *types = spec_alloc(c, count, sizeof(*types));
        if (count && !types) return false;
        if (!spec_work(c, count)) return false;
        for (uint32_t a = 0; a < count; ++a) types[a] = spec_type(c, instance, generic->arguments[op->targets[0] + a]);
        if (c->diagnostic.status != XR_XIR_OK) return false;
        uint32_t target = spec_intern(c, (uint32_t) op->immediate, types, count);
        if (target == UINT32_MAX) return false;
        op->immediate = target; op->targets[0] = op->targets[1] = 0;
    }
    return true;
}
static bool spec_declarations(SpecContext *c, XrXirDeclarations *result) {
    const XrXirDeclarations *source = c->source->declarations;
    if (!source) return true;
    *result = *source;
    XrXirSourceModule *modules = spec_alloc(c, source->module_count, sizeof(*modules));
    if (!modules || !spec_work(c, source->module_count)) return false;
    memcpy(modules, source->modules, source->module_count * sizeof(*modules));
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
static XrXirStatus spec_nominal_fields(SpecContext *c) {
    const XrXirTypes *source = c->source->types;
    c->types.nominals = source->nominals;
    if (!source->count) return XR_XIR_OK;
    XrXirTypeNode *nodes = spec_alloc(c, source->count, sizeof(*nodes));
    c->closed.node_cache = spec_alloc(c, source->count, sizeof(*c->closed.node_cache));
    if (c->diagnostic.status != XR_XIR_OK) return c->diagnostic.status;
    if (!spec_work(c, source->count)) return c->diagnostic.status;
    memcpy(nodes, source->nodes, (size_t) source->count * sizeof(*nodes));
    c->types = (XrXirTypes) {nodes, source->count, source->nominals}; c->node_capacity = source->count;
    for (uint32_t i = 0; i < source->count; ++i)
        if (!source->nodes[i].parameter_span) c->closed.node_cache[i] = (XrXirType) (XR_XIR_CONSTRUCTED_TYPE_BASE + i);
    for (uint32_t i = 0; i < source->count; ++i) {
        if (!spec_work(c, 1)) return c->diagnostic.status;
        const XrXirTypeNode *node = &source->nodes[i];
        if (node->kind != XR_XIR_TYPE_NOMINAL) continue;
        const XrXirNominalDeclaration *d = &source->nominals->declarations[node->nominal.declaration];
        if (d->field_count > c->remaining.parameters) return XR_XIR_BUDGET;
        c->remaining.parameters -= d->field_count;
        XrXirType *cache = spec_alloc(c, source->count, sizeof(*cache));
        XrXirType *fields = spec_alloc(c, d->field_count, sizeof(*fields));
        if (c->diagnostic.status != XR_XIR_OK) return c->diagnostic.status;
        SpecInstance instance = {node->nominal.declaration, node->nominal.arguments, node->nominal.argument_count, cache};
        for (uint32_t f = 0; f < d->field_count; ++f) fields[f] = spec_type(c, &instance, d->fields[f].type);
        if (c->diagnostic.status != XR_XIR_OK) return c->diagnostic.status;
        XrXirTypeNode *owned = (XrXirTypeNode *) c->types.nodes;
        owned[i].nominal.fields = fields; owned[i].nominal.field_count = d->field_count;
    }
    return XR_XIR_OK;
}
XrXirStatus xr_xir_specialize(const XrXirArtifact *checked, const XrXirBudget *budget,
    XrXirArtifact **output, XrXirDiagnostic *diagnostic) {
    SpecContext c = {0};
    c.diagnostic = (XrXirDiagnostic) {XR_XIR_OK, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    c.source = xr_xir_artifact_module(checked);
    c.remaining = budget ? *budget : xr_xir_default_budget();
    XrXirBudget limits = c.remaining;
    if (!output) { c.diagnostic.status = XR_XIR_BAD_STRUCTURE; goto done; }
    *output = NULL;
    if (!c.source || c.source->stage != XR_XIR_CHECKED) { c.diagnostic.status = XR_XIR_BAD_STAGE; goto done; }
    c.diagnostic.status = xr_xir_artifact_verify(checked, &limits, &c.diagnostic);
    if (c.diagnostic.status != XR_XIR_OK) goto done;
    if (c.source->types && c.source->types->nominals) {
        c.diagnostic.status = spec_nominal_fields(&c);
        if (c.diagnostic.status != XR_XIR_OK) goto done;
    }
    if (!c.source->generics) {
        XrXirModule specialized = *c.source;
        if (c.types.nominals) specialized.types = &c.types;
        c.diagnostic.status = xr_xir_recheck(&specialized, &limits, output, &c.diagnostic);
        goto done;
    }
    if (c.source->types && !c.types.nominals) {
        c.closed.node_cache = spec_alloc(&c, c.source->types->count, sizeof(*c.closed.node_cache));
        if (!c.closed.node_cache) goto done;
        for (uint32_t t = 0; t < c.source->types->count; ++t)
            if (!c.source->types->nodes[t].parameter_span)
                spec_type(&c, &c.closed, (XrXirType) (XR_XIR_CONSTRUCTED_TYPE_BASE + t));
        if (c.diagnostic.status != XR_XIR_OK) goto done;
    }
    c.capacity = limits.functions;
    c.functions = spec_alloc(&c, c.capacity, sizeof(*c.functions));
    c.identities = c.source->declarations ? spec_alloc(&c, c.capacity, sizeof(*c.identities)) : NULL;
    c.instances = spec_alloc(&c, c.capacity, sizeof(*c.instances));
    c.ordinary = spec_alloc(&c, c.source->function_count, sizeof(*c.ordinary));
    if (c.diagnostic.status != XR_XIR_OK) goto done;
    for (uint32_t f = 0; f < c.source->function_count; ++f) {
        c.ordinary[f] = UINT32_MAX;
        if (!c.source->generics[f].parameter_count) {
            c.diagnostic.function = f;
            c.ordinary[f] = spec_intern(&c, f, NULL, 0);
            if (c.diagnostic.status != XR_XIR_OK) goto done;
        }
    }
    if (!c.count) { c.diagnostic.status = XR_XIR_BAD_STRUCTURE; goto done; }
    for (uint32_t f = 0; f < c.count; ++f) if (!spec_calls(&c, f)) goto done;
    XrXirDeclarations declarations = {0};
    if (!spec_declarations(&c, &declarations)) goto done;
    XrXirModule specialized = {XR_XIR_CHECKED, c.functions, c.count, c.source->declarations ? &declarations : NULL, NULL, c.types.count || c.types.nominals ? &c.types : NULL};
    c.diagnostic.status = xr_xir_recheck(&specialized, &limits, output, &c.diagnostic);
done:
    while (c.memory) { SpecMemory *next = c.memory->next; xr_free(c.memory); c.memory = next; }
    if (diagnostic) *diagnostic = c.diagnostic;
    return c.diagnostic.status;
}
