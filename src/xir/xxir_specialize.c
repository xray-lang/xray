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
#include "xxir_internal.h"
#include "../base/xmalloc.h"
#include <stdio.h>
typedef struct SpecMemory { struct SpecMemory *next; } SpecMemory;
typedef struct SpecInstance { uint32_t declaration; const XrXirType *types; uint32_t count; } SpecInstance;
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
static XrXirType spec_type(const SpecInstance *instance, XrXirType type) {
    return (uint32_t) type < XR_XIR_TYPE_PARAMETER_BASE ? type :
        instance->types[(uint32_t) type - XR_XIR_TYPE_PARAMETER_BASE];
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
        written = snprintf(name + at, (size_t) capacity - at, ":%u", (unsigned) instance->types[i]);
        if (written < 0 || (size_t) written >= capacity - at) { c->diagnostic.status = XR_XIR_BAD_STRUCTURE; return false; }
        at += (size_t) written;
    }
    function->name = name; function->name_length = (uint32_t) at; return true;
}
static uint32_t spec_intern(SpecContext *c, uint32_t declaration, const XrXirType *types, uint32_t count) {
    for (uint32_t i = 0; i < c->count; ++i) {
        if (!spec_work(c, (uint64_t) count + 1)) return UINT32_MAX;
        if (c->instances[i].declaration == declaration && c->instances[i].count == count &&
            (!count || !memcmp(c->instances[i].types, types, count * sizeof(*types)))) return i;
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
    c->instances[index] = (SpecInstance) {declaration, owned_types, count};
    XrXirFunction *to = &c->functions[index]; *to = *source;
    XrXirType *parameters = spec_alloc(c, source->parameter_count, sizeof(*parameters));
    XrXirInstruction *ops = spec_alloc(c, source->instruction_count, sizeof(*ops));
    if (c->diagnostic.status != XR_XIR_OK) return UINT32_MAX;
    to->parameters = parameters; to->instructions = ops;
    const SpecInstance *instance = &c->instances[index];
    to->result = spec_type(instance, source->result);
    for (uint32_t p = 0; p < source->parameter_count; ++p) parameters[p] = spec_type(instance, source->parameters[p]);
    for (uint32_t i = 0; i < source->instruction_count; ++i) {
        ops[i] = source->instructions[i]; ops[i].type = spec_type(instance, ops[i].type);
    }
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
        for (uint32_t a = 0; a < count; ++a) types[a] = spec_type(instance, generic->arguments[op->targets[0] + a]);
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
    result->modules = modules; result->functions = c->identities;
    result->entry_function = c->ordinary[source->entry_function]; return true;
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
    if (!c.source->generics) {
        c.diagnostic.status = xr_xir_recheck(c.source, &limits, output, &c.diagnostic); goto done;
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
    XrXirModule specialized = {XR_XIR_CHECKED, c.functions, c.count, c.source->declarations ? &declarations : NULL, NULL, c.source->callables};
    c.diagnostic.status = xr_xir_recheck(&specialized, &limits, output, &c.diagnostic);
done:
    while (c.memory) { SpecMemory *next = c.memory->next; xr_free(c.memory); c.memory = next; }
    if (diagnostic) *diagnostic = c.diagnostic;
    return c.diagnostic.status;
}
