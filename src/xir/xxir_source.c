/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source.c - Declaration checking directly into typed Built XIR
 *
 * KEY CONCEPT:
 *   Source names are resolved before lowering and never rediscovered at runtime.
 */
#include "xxir_source.h"
#include "xxir_initialization.h"
#include "xxir_source_query_internal.h"
#include "xxir_generic.h"
#include "xxir_types.h"
#include "xxir_nominal.h"
#include "xxir_float.h"
#include "../shared/xr_decimal_float.h"
#include "../shared/xnative_declaration.h"
#include "../shared/xr_core_intrinsic.h"
#include "../module/xmodule_graph.h"
#include "../frontend/parser/xast.h"
#include "../frontend/parser/xast_walk.h"
#include "../frontend/parser/xtype_ref.h"
#include "../base/xmalloc.h"
#include <stdio.h>
#include <limits.h>

typedef struct SourceMemory { struct SourceMemory *next; } SourceMemory;
typedef enum SourceKind { SOURCE_SLOT, SOURCE_FUNCTION, SOURCE_MODULE, SOURCE_IMPORT, SOURCE_LOCAL, SOURCE_NOMINAL } SourceKind;
typedef struct SourceName {
    struct SourceName *next;
    const char *name, *imported;
    AstNode *node;
    SourceKind kind;
    uint32_t index, module;
    XrXirType type;
    bool mutable, construction;
    uint32_t declaration;
} SourceName;
typedef struct SourceErrorContext SourceErrorContext;
typedef struct SourceFunction {
    AstNode *node, *type_owner;
    XrGenericParam **type_parameters;
    uint32_t type_parameter_count;
    bool infer_result, saw_return, constructor_captured, constructor_shared;
    uint32_t module, count, capacity;
    XrXirType *parameters;
    uint32_t *operands, operand_count, operand_capacity;
    uint32_t type_capacity;
    XrXirBlock *blocks;
    uint32_t block_count, block_capacity, frontier, lexical_depth;
    XrXirInstruction *ops;
    uint32_t declaration, generic_owner;
    uint32_t *constructor_places, *argument_defaults;
    AstNode *default_expression;
    SourceErrorContext *error_context;
    XrXirInitializationRegion *initialization_regions, *initialization_parent;
} SourceFunction;
typedef struct SourcePatch { struct SourcePatch *next; uint32_t instruction; } SourcePatch;
typedef struct SourceLoop { struct SourceLoop *parent; SourcePatch *breaks, *continues; uint32_t frontier; } SourceLoop;
typedef struct SourceValue { uint32_t id; XrXirType type; } SourceValue;
typedef struct SourceErrorEdge {
    struct SourceErrorEdge *next;
    uint32_t block, jump;
    SourceValue value;
} SourceErrorEdge;
struct SourceErrorContext { SourceErrorEdge *edges; uint32_t count, frontier; };

typedef struct SourceContext {
    XrModuleGraph *graph;
    XrXirBudget budget;
    XrXirSourceDiagnostic diagnostic;
    SourceMemory *memory;
    uint64_t allocated;
    XrXirFunction *functions;
    SourceFunction *bodies;
    XrXirSourceModule *modules;
    XrXirFunctionIdentity *identities;
    XrXirGeneric *generics;
    XrXirTypes types;
    XrXirNominalTable nominals;
    XrGenericParam **nominal_type_parameters;
    int nominal_type_parameter_count;
    bool nominal_type_context;
    uint32_t nominal_generic_owner;
    uint32_t **nominal_members, **nominal_variants;
    uint32_t **nominal_defaults;
    SourceName **nominal_sources, **nominal_methods;
    bool *nominal_defaultable;
    uint32_t *nominal_constructors;
    uint32_t type_capacity;
    XrXirSlot *slots;
    XrXirLiteral *literals;
    SourceName **names, *locals, *scope;
    uint32_t function_count, slot_count, literal_count, literal_capacity;
    uint32_t first_closure, next_closure;
    uint32_t function, module, depth;
    SourceLoop *loop;
    bool returned, has_generics, query_ready;
    XrXirSourceView query;
    uint32_t declaration_capacity, reference_capacity, expression_capacity;
    uint32_t query_module_capacity, array_module, array_declaration, array_members[4], length_declaration;
    uint32_t string_module, string_declaration, string_members[5];
} SourceContext;

static bool source_fail(SourceContext *ctx, AstNode *node, XrXirStatus status, const char *message) {
    if (ctx->diagnostic.status == XR_XIR_OK) {
        ctx->diagnostic.status = status;
        ctx->diagnostic.module = ctx->module;
        ctx->diagnostic.line = node ? node->line : 0;
        ctx->diagnostic.column = node ? node->column : 0;
        snprintf(ctx->diagnostic.message, sizeof(ctx->diagnostic.message), "%s", message);
    }
    return false;
}
static void *source_alloc(SourceContext *ctx, size_t count, size_t size) {
    if (ctx->diagnostic.status != XR_XIR_OK) return NULL;
    if (count > (SIZE_MAX - sizeof(SourceMemory)) / size ||
        sizeof(SourceMemory) + count * size > ctx->budget.metadata_bytes - ctx->allocated) {
        source_fail(ctx, NULL, XR_XIR_BUDGET, "source metadata budget exhausted"); return NULL;
    }
    size_t bytes = sizeof(SourceMemory) + count * size;
    SourceMemory *memory = xr_calloc(1, bytes);
    if (!memory) { source_fail(ctx, NULL, XR_XIR_OUT_OF_MEMORY, "source allocation failed"); return NULL; }
    memory->next = ctx->memory; ctx->memory = memory; ctx->allocated += bytes;
    return memory + 1;
}
static bool source_work(SourceContext *ctx, AstNode *node) {
    if (!ctx->budget.work) return source_fail(ctx, node, XR_XIR_BUDGET, "source work budget exhausted");
    --ctx->budget.work;
    return ctx->diagnostic.status == XR_XIR_OK;
}
static void *source_query_append(SourceContext *ctx, const void *array, uint32_t *count,
    uint32_t *capacity, size_t size) {
    if (!source_work(ctx, NULL)) return NULL;
    if (*count == *capacity) {
        if (*capacity > UINT32_MAX / 2) { source_fail(ctx, NULL, XR_XIR_BUDGET, "source query identity exhausted"); return NULL; }
        uint32_t next = *capacity ? *capacity * 2 : 16;
        void *grown = source_alloc(ctx, next, size);
        if (!grown) return NULL;
        if (*count) memcpy(grown, array, (size_t) *count * size);
        array = grown; *capacity = next;
    }
    ++*count;
    return (void *) array;
}
static XrXirSourceRange source_query_range(SourceContext *ctx, AstNode *node, const char *name) {
    XrXirSourceRange range = {ctx->module, 0, 0, 0, 0};
    if (!node) return range;
    range.line = node->line; range.column = node->column;
    range.end_line = node->end_line; range.end_column = node->end_column;
    if (name && range.line > 0 && range.column > 0 && strlen(name) <= (size_t) (INT_MAX - range.column)) {
        range.end_line = range.line; range.end_column = range.column + (int) strlen(name);
    }
    return range;
}
static XrXirSourceType source_query_type(SourceContext *ctx, XrXirType type) {
    uint32_t owner = 0;
    if (xr_xir_type_span(&ctx->types, type))
        owner = ctx->nominal_generic_owner ? ctx->nominal_generic_owner : ctx->bodies[ctx->function].generic_owner;
    return (XrXirSourceType) {type, owner, true};
}
static bool source_query_declare(SourceContext *ctx, SourceName *symbol,
    XrXirSourceDeclarationKind kind, uint32_t parent, XrXirSourceRange range) {
    XrXirSourceDeclaration *records = source_query_append(ctx, ctx->query.declarations,
        &ctx->query.declaration_count, &ctx->declaration_capacity, sizeof(*records));
    if (!records) return false;
    ctx->query.declarations = records;
    XrXirSourceDeclaration *record = &records[ctx->query.declaration_count - 1];
    symbol->declaration = ctx->query.declaration_count;
    *record = (XrXirSourceDeclaration) {symbol->declaration, parent, 0, kind, symbol->name, range,
        {0}, NULL, 0, symbol->mutable,
        (kind == XR_XIR_SOURCE_FUNCTION || kind == XR_XIR_SOURCE_TYPE) && !parent && symbol->node && symbol->node->is_exported, 0, NULL, 0, 0, 0, NULL};
    return true;
}
static void source_query_binding_type(SourceContext *ctx, SourceName *symbol) {
    XrXirSourceDeclaration *records = (XrXirSourceDeclaration *) ctx->query.declarations;
    if (symbol->declaration) records[symbol->declaration - 1].type = source_query_type(ctx, symbol->type);
}
static bool source_query_reference(SourceContext *ctx, AstNode *node, SourceName *binding,
    SourceName *target, XrXirSourceAccess access) {
    if (!binding || !target || !binding->declaration || !target->declaration) return true;
    XrXirSourceReference *records = source_query_append(ctx, ctx->query.references,
        &ctx->query.reference_count, &ctx->reference_capacity, sizeof(*records));
    if (!records) return false;
    ctx->query.references = records;
    XrXirSourceReference *record = &records[ctx->query.reference_count - 1];
    *record = (XrXirSourceReference) {source_query_range(ctx, node, NULL), binding->declaration, target->declaration, access};
    return true;
}
static bool source_query_expression(SourceContext *ctx, AstNode *node, XrXirType type) {
    XrXirSourceExpression *records = source_query_append(ctx, ctx->query.expressions,
        &ctx->query.expression_count, &ctx->expression_capacity, sizeof(*records));
    if (!records) return false;
    ctx->query.expressions = records;
    XrXirSourceExpression *record = &records[ctx->query.expression_count - 1];
    *record = (XrXirSourceExpression) {node->node_id, source_query_range(ctx, node, NULL), source_query_type(ctx, type)};
    return true;
}
static bool source_query_parameters(SourceContext *ctx, uint32_t declaration) {
    const XrXirFunction *function = &ctx->functions[ctx->function];
    XrXirSourceType *parameters = function->parameter_count ? source_alloc(ctx, function->parameter_count, sizeof(*parameters)) : NULL;
    if (function->parameter_count && !parameters) return false;
    for (uint32_t p = 0; p < function->parameter_count; ++p) {
        if (!source_work(ctx, NULL)) return false;
        parameters[p] = source_query_type(ctx, function->parameters[p]);
    }
    XrXirSourceDeclaration *record = (XrXirSourceDeclaration *) &ctx->query.declarations[declaration - 1];
    record->type = source_query_type(ctx, function->result);
    record->parameters = parameters; record->parameter_count = function->parameter_count;
    record->generic_parameter_count = ctx->generics[ctx->function].parameter_count;
    record->generic_constraints = ctx->generics[ctx->function].constraints;
    uint32_t owner = ctx->bodies[ctx->function].generic_owner;
    if (owner && owner != declaration) {
        record->generic_parent = owner;
        record->generic_parent_count = record->generic_parameter_count;
    }
    return true;
}
static SourceName *find_name(SourceContext *ctx, SourceName *names, const char *name) {
    for (SourceName *p = names; p; p = p->next) {
        if (!source_work(ctx, p->node)) return NULL;
        if (!strcmp(p->name, name)) return p;
    }
    return NULL;
}
static SourceName *add_name(SourceContext *ctx, SourceName **head, const char *name, AstNode *node) {
    if (!name || !*name || find_name(ctx, *head, name)) {
        source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "duplicate or empty declaration name"); return NULL;
    }
    SourceName *symbol = source_alloc(ctx, 1, sizeof(*symbol));
    if (symbol) { symbol->name = name; symbol->node = node; symbol->next = *head; *head = symbol; }
    return symbol;
}
static bool source_type(SourceContext *ctx, XrTypeRef *ref, XrXirType *type);
static bool source_nominal_type(SourceContext *ctx, const char *name, XrXirType *type);
static SourceName *source_nominal_name(SourceContext *ctx, const char *name);
static bool source_nominal_arguments(SourceContext *ctx, const char *name, XrTypeRef **arguments,
    uint32_t count, XrXirType *type);
static bool source_intern_type(SourceContext *ctx, XrXirTypeNode node, XrXirType *type) {
    uint32_t span = xr_xir_type_span(&ctx->types,
        node.kind == XR_XIR_TYPE_CALLABLE ? node.result : node.element);
    for (uint32_t p = 0; p < node.parameter_count; ++p) {
        if (!source_work(ctx, NULL)) return false;
        if (!node.parameters[p].type) return source_fail(ctx, NULL, XR_XIR_BAD_TYPE, "callable parameter must be a value type");
        uint32_t component = xr_xir_type_span(&ctx->types, node.parameters[p].type);
        if (component > span) span = component;
    }
    node.parameter_span = span;
    if (node.kind == XR_XIR_TYPE_NOMINAL) {
        for (uint32_t a = 0; a < node.nominal.argument_count; ++a) {
            if (!source_work(ctx, NULL)) return false;
            uint32_t component = xr_xir_type_span(&ctx->types, node.nominal.arguments[a]);
            if (component > node.parameter_span) node.parameter_span = component;
        }
    }
    for (uint32_t i = 0; i < ctx->types.count; ++i) {
        const XrXirTypeNode *s = &ctx->types.nodes[i];
        if (!source_work(ctx, NULL)) return false;
        if (s->kind != node.kind || s->element != node.element || s->parameter_count != node.parameter_count ||
            s->result != node.result || s->flags != node.flags) continue;
        if (node.kind == XR_XIR_TYPE_NOMINAL && (s->nominal.declaration != node.nominal.declaration ||
            s->nominal.argument_count != node.nominal.argument_count)) continue;
        bool same = true;
        for (uint32_t a = 0; a < node.nominal.argument_count; ++a) {
            if (!source_work(ctx, NULL)) return false;
            if (s->nominal.arguments[a] != node.nominal.arguments[a]) same = false;
        }
        for (uint32_t p = 0; p < node.parameter_count; ++p) {
            if (!source_work(ctx, NULL)) return false;
            if (s->parameters[p].type != node.parameters[p].type ||
                s->parameters[p].mode != node.parameters[p].mode) same = false;
        }
        if (same) { *type = (XrXirType) (XR_XIR_CONSTRUCTED_TYPE_BASE + i); return true; }
    }
    const uint32_t limit = XR_XIR_CONSTRUCTED_TYPE_LIMIT - XR_XIR_CONSTRUCTED_TYPE_BASE;
    if (ctx->types.count == limit)
        return source_fail(ctx, NULL, XR_XIR_BUDGET, "constructed type identity budget exhausted");
    if (ctx->types.count == ctx->type_capacity) {
        uint32_t capacity = ctx->type_capacity ? ctx->type_capacity * 2 : 8;
        if (capacity > limit) capacity = limit;
        XrXirTypeNode *table = source_alloc(ctx, capacity, sizeof(*table));
        if (!table) return false;
        if (ctx->types.count) memcpy(table, ctx->types.nodes, ctx->types.count * sizeof(*table));
        ctx->types.nodes = table; ctx->type_capacity = capacity;
    }
    XrXirTypeNode *table = (XrXirTypeNode *) ctx->types.nodes;
    table[ctx->types.count] = node;
    *type = (XrXirType) (XR_XIR_CONSTRUCTED_TYPE_BASE + ctx->types.count++); return true;
}
static bool source_signature(SourceContext *ctx, const XrXirCallableParameter *parameters,
    uint32_t count, XrXirType result, XrXirType *type) {
    return source_intern_type(ctx, (XrXirTypeNode) {XR_XIR_TYPE_CALLABLE, XR_XIR_UNIT,
        parameters, count, result, 0, 0, {0}}, type);
}
static bool source_reference_promise(SourceContext *ctx, AstNode *node,
    uint32_t function, XrXirType expected, XrXirType *type) {
    const XrXirTypeNode *context = xr_xir_callable_signature(&ctx->types, expected);
    if (!context || context->flags != XR_XIR_CALLABLE_NO_SUSPEND) return true;
    if (!(ctx->identities[function].promises & XR_XIR_FUNCTION_NO_SUSPEND))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "qualified reference requires an explicit target promise");
    XrXirTypeNode signature = *xr_xir_callable_signature(&ctx->types, *type);
    signature.flags = XR_XIR_CALLABLE_NO_SUSPEND;
    return source_intern_type(ctx, signature, type);
}
static bool source_cell_type(SourceContext *ctx, XrXirType element, XrXirType *type) {
    return source_intern_type(ctx, (XrXirTypeNode) {XR_XIR_TYPE_CELL, element,
        NULL, 0, XR_XIR_UNIT, 0, 0, {0}}, type);
}
static XrGenericParam **source_type_parameters(SourceContext *ctx, int *count) {
    if (ctx->nominal_type_context) {
        *count = ctx->nominal_type_parameter_count;
        return ctx->nominal_type_parameters;
    }
    SourceFunction *body = &ctx->bodies[ctx->function];
    *count = (int)body->type_parameter_count;
    return body->type_parameters;
}
#include "xxir_source_native.inc.c"
typedef struct SourceSubstitution { const XrXirType *types; uint32_t count; } SourceSubstitution;
static bool source_substitute(SourceContext *ctx, const SourceSubstitution *sub,
    XrXirType type, uint32_t depth, XrXirType *output) {
    if (!source_work(ctx, NULL)) return false;
    if (depth == 128) return source_fail(ctx, NULL, XR_XIR_BUDGET, "constructed type substitution depth exhausted");
    if ((uint32_t) type >= XR_XIR_TYPE_PARAMETER_BASE && (uint32_t) type < XR_XIR_TYPE_PARAMETER_LIMIT) {
        uint32_t index = (uint32_t) type - XR_XIR_TYPE_PARAMETER_BASE;
        if (index >= sub->count) return source_fail(ctx, NULL, XR_XIR_BAD_TYPE, "type parameter escapes declaration");
        *output = sub->types[index]; return true;
    }
    const XrXirTypeNode *found = xr_xir_type_node(&ctx->types, type);
    if (!found || !found->parameter_span) { *output = type; return true; }
    XrXirTypeNode node = *found;
    if (node.kind == XR_XIR_TYPE_NOMINAL) {
        XrXirType *arguments = source_alloc(ctx, node.nominal.argument_count, sizeof(*arguments));
        if (!arguments) return false;
        for (uint32_t a = 0; a < node.nominal.argument_count; ++a)
            if (!source_substitute(ctx, sub, node.nominal.arguments[a], depth + 1, &arguments[a])) return false;
        node.nominal.arguments = arguments;
        /* Derived fields belong to the substituted identity, never its template. */
        node.nominal.fields = NULL; node.nominal.field_count = 0;
        return source_intern_type(ctx, node, output);
    }
    if (node.kind != XR_XIR_TYPE_CALLABLE) {
        if (!source_substitute(ctx, sub, node.element, depth + 1, &node.element)) return false;
        return source_intern_type(ctx, node, output);
    }
    XrXirCallableParameter *parameters = node.parameter_count ? source_alloc(ctx, node.parameter_count, sizeof(*parameters)) : NULL;
    if (node.parameter_count && !parameters) return false;
    for (uint32_t p = 0; p < node.parameter_count; ++p) {
        parameters[p].mode = node.parameters[p].mode;
        if (!source_substitute(ctx, sub, node.parameters[p].type, depth + 1, &parameters[p].type)) return false;
    }
    node.parameters = parameters;
    if (!source_substitute(ctx, sub, node.result, depth + 1, &node.result)) return false;
    return source_intern_type(ctx, node, output);
}
static bool source_callable_type(SourceContext *ctx, XrTypeRef *ref, XrXirType *type) {
    if (!ref->nchildren || !ref->children || ref->requires_nothrow || ref->borrow_origin_syntax || ref->borrow_origin_count)
        return source_fail(ctx, NULL, XR_XIR_BAD_TYPE, "callable promises require an implemented contract");
    if (ctx->depth >= 128) return source_fail(ctx, NULL, XR_XIR_BUDGET, "source type depth exhausted");
    ++ctx->depth;
    uint32_t count = ref->nchildren - 1;
    XrXirCallableParameter *parameters = count ? source_alloc(ctx, count, sizeof(*parameters)) : NULL;
    if (count && !parameters) return false;
    for (uint32_t p = 0; p < count; ++p) {
        if (ref->function_param_modes && ref->function_param_modes[p] != XR_PARAM_READ)
            return source_fail(ctx, NULL, XR_XIR_BAD_TYPE, "callable mode requires an implemented contract");
        if (!source_work(ctx, NULL) || !source_type(ctx, ref->children[p], &parameters[p].type)) return false;
    }
    XrXirType result;
    if (!source_type(ctx, ref->children[count], &result)) return false;
    --ctx->depth; return source_signature(ctx, parameters, count, result, type);
}
static bool source_parameter_markers(SourceContext *ctx, AstNode *node,
    const XrGenericParam *parameter, uint32_t *markers) {
    *markers = 0;
    if (!parameter || !parameter->name || !strcmp(parameter->name, "Sendable") ||
        !strcmp(parameter->name, "Error") || parameter->constraint_count < 0 || parameter->constraint_count > 65536)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "generic parameter contract is not admitted");
    for (int i = 0; i < parameter->constraint_count; ++i) {
        if (!source_work(ctx, node)) return false;
        XrTypeRef *constraint = parameter->constraints[i];
        if (!constraint || constraint->kind != XR_TREF_NAMED || !constraint->name || constraint->nchildren)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "generic marker must be an unparameterized builtin");
        uint32_t bit = !strcmp(constraint->name, "Sendable") ? XR_XIR_CONSTRAINT_SENDABLE :
            !strcmp(constraint->name, "Error") ? XR_XIR_CONSTRAINT_ERROR : 0;
        if (!bit)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "unknown generic marker");
        *markers |= bit;
    }
    return true;
}
static bool source_type(SourceContext *ctx, XrTypeRef *ref, XrXirType *type) {
    if (!ref) { *type = XR_XIR_UNIT; return true; }
    switch (ref->kind) {
    case XR_TREF_FUNCTION: return source_callable_type(ctx, ref, type);
    case XR_TREF_UNIT: *type = XR_XIR_UNIT; return true;
    case XR_TREF_BOOL: *type = XR_XIR_BOOL; return true;
    case XR_TREF_STRING: *type = XR_XIR_STRING; return true;
    case XR_TREF_NAMED: case XR_TREF_TYPE_PARAM: {
        AstNode *node = ctx->bodies[ctx->function].type_owner;
        if (!ref->name) break;
        int count;
        XrGenericParam **parameters = source_type_parameters(ctx, &count);
        for (int i = 0; i < count; ++i) {
            if (!source_work(ctx, node)) return false;
            if (!strcmp(ref->name, parameters[i]->name)) {
                *type = (XrXirType) (XR_XIR_TYPE_PARAMETER_BASE + (uint32_t) i); return true;
            }
        }
        if (!strcmp(ref->name, "Error") && !source_nominal_name(ctx, ref->name)) {
            *type = XR_XIR_ERROR; return true;
        }
        if (!strcmp(ref->name, "PanicInfo") && !source_nominal_name(ctx, ref->name)) {
            *type = XR_XIR_PANIC_INFO; return true;
        }
        return source_nominal_type(ctx, ref->name, type);
    }
    case XR_TREF_SCALAR:
        switch (ref->scalar_rep) {
        case XR_NATIVE_I8: *type = XR_XIR_I8; return true;
        case XR_NATIVE_I16: *type = XR_XIR_I16; return true;
        case XR_NATIVE_I32: *type = XR_XIR_I32; return true;
        case XR_NATIVE_I64: *type = XR_XIR_I64; return true;
        case XR_NATIVE_U8: *type = XR_XIR_U8; return true;
        case XR_NATIVE_U16: *type = XR_XIR_U16; return true;
        case XR_NATIVE_U32: *type = XR_XIR_U32; return true;
        case XR_NATIVE_F32: *type = XR_XIR_F32; return true;
        case XR_NATIVE_F64: *type = XR_XIR_F64; return true;
        case XR_NATIVE_U64: *type = XR_XIR_U64; return true;
        default: break;
        }
        break;
    case XR_TREF_GENERIC:
        if (source_nominal_name(ctx, ref->name))
            return source_nominal_arguments(ctx, ref->name, ref->children, ref->nchildren, type);
        if (ref->name && xr_native_declaration_by_name(ref->name))
            return source_native_array_type(ctx, ref, type);
        if (ref->name && !strcmp(ref->name, "Atomic") && ref->nchildren == 1 &&
            ref->children[0]->kind == XR_TREF_SCALAR && ref->children[0]->scalar_rep == XR_NATIVE_I64) {
            *type = XR_XIR_ATOMIC_I64; return true;
        }
        return source_nominal_arguments(ctx, ref->name, ref->children, ref->nchildren, type);
    default: break;
    }
    return source_fail(ctx, NULL, XR_XIR_BAD_TYPE, "type declaration is not admitted by XIR");
}
static bool begin_block(SourceContext *ctx) {
    SourceFunction *body = &ctx->bodies[ctx->function];
    if (!ctx->budget.blocks) return source_fail(ctx, body->node, XR_XIR_BUDGET, "block budget exhausted");
    if (body->block_count == body->block_capacity) {
        uint32_t capacity = body->block_capacity ? body->block_capacity * 2 : 4;
        if (capacity < body->block_capacity) return source_fail(ctx, NULL, XR_XIR_BUDGET, "block capacity overflow");
        XrXirBlock *blocks = source_alloc(ctx, capacity, sizeof(*blocks));
        if (!blocks) return false;
        if (body->block_count) memcpy(blocks, body->blocks, body->block_count * sizeof(*blocks));
        body->blocks = blocks; body->block_capacity = capacity;
    }
    body->blocks[body->block_count++] = (XrXirBlock) {body->count, 0, 0, body->frontier};
    --ctx->budget.blocks; ctx->returned = false; return true;
}
static bool emit_raw(SourceContext *ctx, XrXirInstruction op, SourceValue *result) {
    SourceFunction *body = &ctx->bodies[ctx->function];
    if (!body->block_count) {
        bool terminated = ctx->returned;
        if (!begin_block(ctx)) return false;
        ctx->returned = terminated;
    }
    if (!ctx->budget.instructions) return source_fail(ctx, body->node, XR_XIR_BUDGET, "instruction budget exhausted");
    if (body->count == body->capacity) {
        uint32_t capacity = body->capacity ? body->capacity * 2 : 16;
        if (capacity < body->capacity) return source_fail(ctx, NULL, XR_XIR_BUDGET, "instruction capacity overflow");
        XrXirInstruction *ops = source_alloc(ctx, capacity, sizeof(*ops));
        if (!ops) return false;
        if (body->count) memcpy(ops, body->ops, body->count * sizeof(*ops));
        body->ops = ops; body->capacity = capacity;
    }
    if (result) *result = (SourceValue) {ctx->functions[ctx->function].parameter_count + body->count, op.type};
    body->ops[body->count++] = op; ++body->blocks[body->block_count - 1].count; --ctx->budget.instructions;
    return true;
}
#include "xxir_source_invoke.inc.c"
static bool expression(SourceContext *ctx, AstNode *node, SourceValue *value);
static bool expression_in(SourceContext *ctx, AstNode *node, XrXirType expected, SourceValue *value);
static bool source_expect(SourceContext *ctx, AstNode *node, XrXirType expected, SourceValue *value) {
    if (!expected || value->type == expected) return true;
    if (xr_xir_type_is_callable(&ctx->types, expected) && xr_xir_type_is_callable(&ctx->types, value->type)) {
        XrXirStatus status = xr_xir_callable_weakening(&ctx->types, value->type, expected, &ctx->budget.work);
        if (status != XR_XIR_OK) return source_fail(ctx, node, status, "callable conversion may only discard its top-level promise");
        return emit(ctx, (XrXirInstruction){XR_XIR_FUNCTION_WEAKEN, expected, {value->id, 0}, {0}, 0, {0}}, value);
    }
    if (expected == XR_XIR_ERROR) {
        const XrXirGeneric *generic = &ctx->generics[ctx->function];
        XrXirStatus status = xr_xir_type_markers(&ctx->types, value->type, XR_XIR_CONSTRAINT_ERROR,
            generic->constraints, generic->parameter_count, &ctx->budget.work);
        if (status != XR_XIR_OK) return source_fail(ctx, node, status, "Error conversion requires an enum proof");
        return emit(ctx, (XrXirInstruction) {XR_XIR_ERROR_ERASE, XR_XIR_ERROR, {value->id, 0}, {0}, 0, {0}}, value);
    }
    if (!(value->type == XR_XIR_F32 && expected == XR_XIR_F64) &&
        (!xr_xir_type_is_integer(value->type) || !xr_xir_type_is_integer(expected) ||
         xr_xir_integer_signed(value->type) != xr_xir_integer_signed(expected) ||
         xr_xir_integer_bits(value->type) > xr_xir_integer_bits(expected)))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "expression cannot satisfy its declared type");
    return emit(ctx, (XrXirInstruction) {XR_XIR_CONVERT_NUMBER, expected, {value->id, 0}, {0}, 0, {0}}, value);
}
typedef struct SourceInteger { bool present, negative; uint64_t magnitude; } SourceInteger;
static bool source_direct_integer(SourceContext *ctx, AstNode *node, SourceInteger *literal) {
    *literal = (SourceInteger) {0};
    bool negative = false;
    for (uint32_t depth = ctx->depth; node; ++depth) {
        if (!source_work(ctx, node)) return false;
        if (depth >= 128) return source_fail(ctx, node, XR_XIR_BUDGET, "integer literal depth exhausted");
        if (node->type == AST_GROUPING) { node = node->as.grouping; continue; }
        if (node->type == AST_UNARY_NEG && !negative) { negative = true; node = node->as.unary.operand; continue; }
        if (node->type != AST_LITERAL_INT) return true;
        uint64_t magnitude = node->as.literal.int_bits;
        if (!node->as.literal.int_overflows_i64 && magnitude > INT64_MAX) {
            magnitude = UINT64_C(0) - magnitude; negative = !negative;
        }
        *literal = (SourceInteger) {true, negative, magnitude}; return true;
    }
    return true;
}
typedef struct SourceDecimal { AstNode *node; bool negative; } SourceDecimal;
static bool source_direct_decimal(SourceContext *ctx, AstNode *node, SourceDecimal *literal) {
    *literal = (SourceDecimal) {0}; bool negative = false;
    for (unsigned depth = 0; node; ++depth) {
        if (!source_work(ctx, node)) return false;
        if (depth >= 128) return source_fail(ctx, node, XR_XIR_BUDGET, "decimal literal depth exhausted");
        if (node->type == AST_GROUPING) { node = node->as.grouping; continue; }
        if (node->type == AST_UNARY_NEG && !negative) { negative = true; node = node->as.unary.operand; continue; }
        if (node->type == AST_LITERAL_FLOAT) *literal = (SourceDecimal) {node, negative};
        return true;
    }
    return true;
}
static bool source_decimal_payload(SourceContext *ctx, const SourceDecimal *literal, XrXirType type, uint64_t *bits) {
    AstNode *node = literal->node;
    size_t length = node->as.literal.decimal_length;
    if (!node->as.literal.decimal_text || !length) return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "decimal spelling is missing");
    if (length > ctx->budget.work) return source_fail(ctx, node, XR_XIR_BUDGET, "decimal literal work budget exhausted");
    ctx->budget.work -= length;
    *bits = 0;
    if (!xr_decimal_float_parse(node->as.literal.decimal_text, length, xr_xir_float_bits(type), bits))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "invalid decimal literal");
    if (literal->negative) *bits ^= UINT64_C(1) << (xr_xir_float_bits(type) - 1);
    return true;
}
static bool source_decimal(SourceContext *ctx, const SourceDecimal *literal, XrXirType expected, SourceValue *value) {
    XrXirType type = xr_xir_float_bits(expected) ? expected : XR_XIR_F64;
    uint64_t bits;
    if (!source_decimal_payload(ctx,literal,type,&bits)) return false;
    int64_t payload; memcpy(&payload, &bits, sizeof(payload));
    return emit(ctx, (XrXirInstruction) {XR_XIR_CONST_FLOAT, type, {0}, {0}, payload, {0}}, value);
}
static bool source_integer_float_payload(SourceContext *ctx, AstNode *node, const SourceInteger *literal,
    XrXirType type, uint64_t *bits) {
    int64_t magnitude; memcpy(&magnitude, &literal->magnitude, sizeof(magnitude));
    *bits = 0; int64_t exact = 0;
    XrXirIntegerFormat format = {64, false};
    if (xr_xir_integer_to_float(format, xr_xir_float_bits(type), magnitude, bits) != XR_XIR_NUMERIC_OK ||
        xr_xir_float_to_integer(xr_xir_float_bits(type), format, *bits, &exact) != XR_XIR_NUMERIC_OK ||
        (uint64_t) exact != literal->magnitude)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "integer literal is not exactly representable in its floating context");
    if (literal->negative) *bits ^= UINT64_C(1) << (xr_xir_float_bits(type) - 1);
    return true;
}
static bool source_integer_float(SourceContext *ctx, AstNode *node, const SourceInteger *literal,
    XrXirType type, SourceValue *value) {
    uint64_t bits;
    if (!source_integer_float_payload(ctx,node,literal,type,&bits)) return false;
    int64_t payload; memcpy(&payload, &bits, sizeof(payload));
    return emit(ctx, (XrXirInstruction) {XR_XIR_CONST_FLOAT, type, {0}, {0}, payload, {0}}, value);
}
static bool source_integer_payload(SourceContext *ctx, AstNode *node, const SourceInteger *literal,
    XrXirType type, uint64_t *bits) {
    uint32_t width = xr_xir_integer_bits(type);
    bool sign = xr_xir_integer_signed(type);
    uint64_t limit = sign ? (UINT64_C(1) << (width - 1)) - (literal->negative ? 0 : 1) :
        (width == 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1);
    if (literal->magnitude > limit || (!sign && literal->negative && literal->magnitude))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "integer literal is outside its contextual type");
    *bits = literal->negative ? UINT64_C(0) - literal->magnitude : literal->magnitude;
    return true;
}
static bool source_integer(SourceContext *ctx, AstNode *node, const SourceInteger *literal,
    XrXirType expected, SourceValue *value) {
    if (xr_xir_float_bits(expected)) return source_integer_float(ctx, node, literal, expected, value);
    XrXirType type = xr_xir_type_is_integer(expected) ? expected : XR_XIR_I64;
    uint64_t bits;
    if (!source_integer_payload(ctx,node,literal,type,&bits)) return false;
    int64_t payload = bits <= INT64_MAX ? (int64_t) bits : -1 - (int64_t) (UINT64_MAX - bits);
    return emit(ctx, (XrXirInstruction) {XR_XIR_CONST_INT, type, {0}, {0}, payload, {0}}, value);
}
static bool emit_group(SourceContext *ctx, XrXirInstruction op, const SourceValue *args,
                       uint32_t count, SourceValue *value) {
    SourceFunction *body = &ctx->bodies[ctx->function];
    if (count > UINT32_MAX - body->operand_count)
        return source_fail(ctx, NULL, XR_XIR_BUDGET, "operand capacity overflow");
    uint32_t needed = body->operand_count + count;
    if (needed > body->operand_capacity) {
        uint32_t capacity = needed <= UINT32_MAX / 2 ? needed * 2 : needed;
        uint32_t *operands = source_alloc(ctx, capacity, sizeof(*operands));
        if (!operands) return false;
        if (body->operand_count) memcpy(operands, body->operands, body->operand_count * sizeof(*operands));
        body->operands = operands; body->operand_capacity = capacity;
    }
    op.args[0] = count ? body->operand_count : 0; op.args[1] = count;
    for (uint32_t i = 0; i < count; ++i) body->operands[body->operand_count++] = args[i].id;
    return emit(ctx, op, value);
}
static bool statement(SourceContext *ctx, AstNode *node, bool top);
static SourceName *visible_name(SourceContext *ctx, const char *name) {
    SourceName *symbol = find_name(ctx, ctx->locals, name);
    return symbol ? symbol : find_name(ctx, ctx->names[ctx->module], name);
}
static SourceName *imported_declaration(SourceContext *ctx, SourceName *symbol, const char *name) {
    SourceName *target = find_name(ctx, ctx->names[symbol->module], name);
    if (!target || (target->kind != SOURCE_FUNCTION && target->kind != SOURCE_NOMINAL) || !target->node->is_exported) {
        source_fail(ctx, symbol->node, XR_XIR_BAD_STRUCTURE, "import requires an exported declaration"); return NULL;
    }
    if (symbol->kind == SOURCE_IMPORT && symbol->declaration) {
        XrXirSourceDeclaration *declarations = (XrXirSourceDeclaration *) ctx->query.declarations;
        declarations[symbol->declaration - 1].target = target->declaration;
    }
    return target;
}
static uint32_t stream_primitive(SourceContext *ctx, const char *name) {
    const XrModuleSpec *spec = &ctx->graph->specs[ctx->module];
    if (spec->authority.kind != XR_MODULE_IDENTITY_STDLIB || !spec->authority.namespace_id ||
        strcmp(spec->authority.namespace_id, "io") || !spec->logical_path ||
        strcmp(spec->logical_path, "io/output.xr")) return 0;
    if (!strcmp(name, "__writeStdout")) return 1;
    if (!strcmp(name, "__writeStderr")) return 2;
    return 0;
}
typedef struct SourceTypeArguments {
    XrTypeRef **refs;
    uint32_t count;
    SourceSubstitution substitution;
} SourceTypeArguments;
static bool source_type_arguments(SourceContext *ctx, AstNode *node, const XrXirType *types,
    uint32_t count, XrXirInstruction *op) {
    XrXirGeneric *caller = &ctx->generics[ctx->function];
    if (count > UINT32_MAX - caller->argument_count)
        return source_fail(ctx, node, XR_XIR_BUDGET, "type argument table overflow");
    uint32_t needed = caller->argument_count + count;
    SourceFunction *body = &ctx->bodies[ctx->function];
    if (needed > body->type_capacity) {
        uint32_t capacity = needed <= UINT32_MAX / 2 ? needed * 2 : needed;
        XrXirType *table = source_alloc(ctx, capacity, sizeof(*table));
        if (!table) return false;
        if (caller->argument_count) memcpy(table, caller->arguments, caller->argument_count * sizeof(*table));
        caller->arguments = table; body->type_capacity = capacity;
    }
    op->type_arguments[0] = count ? caller->argument_count : 0; op->type_arguments[1] = count;
    if (count) memcpy((XrXirType *) caller->arguments + caller->argument_count, types, count * sizeof(*types));
    caller->argument_count = needed; return true;
}
static bool source_instantiation(SourceContext *ctx, AstNode *node, const XrXirGeneric *callee,
    SourceTypeArguments *arguments) {
    uint32_t count = arguments->count;
    if (count > 65536 || count != callee->parameter_count || (count && !arguments->refs))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "function requires its exact explicit type arguments");
    XrXirType *types = count ? source_alloc(ctx, count, sizeof(*types)) : NULL;
    if (count && !types) return false;
    XrXirDeclarations declarations = {0};
    declarations.modules = ctx->modules; declarations.module_count = (uint32_t) ctx->graph->spec_count;
    declarations.functions = ctx->identities;
    XrXirModule view = {XR_XIR_BUILT, ctx->functions, ctx->function_count, &declarations, ctx->generics, &ctx->types, NULL};
    for (uint32_t i = 0; i < count; ++i) {
        if (!source_work(ctx, node) || !source_type(ctx, arguments->refs[i], &types[i])) return false;
        XrXirStatus status = xr_xir_type_satisfies(&view, ctx->function, types[i], callee->constraints[i], &ctx->budget);
        if (status != XR_XIR_OK)
            return source_fail(ctx, node, status, status == XR_XIR_BUDGET ?
                "type argument constraint work budget exhausted" : "type argument does not prove the declared constraint");
    }
    arguments->substitution = (SourceSubstitution) {types,count};
    return true;
}
#include "xxir_source_arguments.inc.c"
static bool prepare_call(SourceContext *ctx, AstNode *node, SourceName *target,
    SourceSubstitution *substitution, XrXirInstruction *op) {
    CallExprNode *call = &node->as.call_expr;
    const XrXirFunction *function = &ctx->functions[target->index];
    if (!source_argument_arity(ctx, node, target->index, (uint32_t)call->arg_count)) return false;
    SourceTypeArguments arguments = {call->type_args,(uint32_t) call->type_arg_count,{0}};
    *op = (XrXirInstruction) {XR_XIR_CALL,XR_XIR_UNIT,{0},{0},target->index, {0}};
    if (!source_instantiation(ctx,node,&ctx->generics[target->index],&arguments) ||
        !source_substitute(ctx,&arguments.substitution,function->result,0,&op->type)) return false;
    *substitution = arguments.substitution; return true;
}
static bool source_value_place(SourceContext *ctx, AstNode *node, SourceValue *place);
#include "xxir_source_array.inc.c"
#include "xxir_source_string.inc.c"
static bool source_constructor_receiver(SourceContext *ctx, AstNode *node);
static bool source_constructor_field(SourceContext *ctx, AstNode *node, const char *name, SourceValue *value, AstNode *incoming);
#include "xxir_source_struct.inc.c"
#include "xxir_source_path.inc.c"
#include "xxir_source_enum.inc.c"
#include "xxir_source_defaults.inc.c"
#include "xxir_source_constructors.inc.c"
#include "xxir_source_panic.inc.c"
#include "xxir_source_methods.inc.c"
static bool source_call(SourceContext *ctx, AstNode *node, SourceValue *value) {
    CallExprNode *call = &node->as.call_expr;
    if (call->arg_count < 0 || call->arg_count > 65536 || call->type_arg_count < 0 ||
        call->type_arg_count > 65536 || call->default_arg_count)
        return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "call arity or type arguments are not admitted");
    SourceName *target = NULL, *binding = NULL;
    bool print = false, atomic = false;
    uint32_t stream = 0;
    AstNode *callee = call->callee;
    SourceValue receiver = {0}, indirect = {0};
    bool indirect_ready = false;
    SourceStaticMethod selected = {0};
    XrXirOp method = XR_XIR_INVALID;
    if (callee->type == AST_VARIABLE) {
        const char *name = callee->as.variable.name;
        target = visible_name(ctx, name);
        binding = target;
        if (!target) {
            const XrCoreIntrinsicDesc *intrinsic = xr_core_intrinsic_by_source_name(name, strlen(name));
            if (intrinsic && intrinsic->id == XR_CORE_BUILTIN_LEN)
                return source_length(ctx, node, intrinsic, value);
            if (!strcmp(name, "PanicInfo"))
                return source_fail(ctx, node, XR_XIR_BAD_TYPE, "PanicInfo construction requires class support");
            print = !strcmp(name, "print"); atomic = !strcmp(name, "Atomic"); stream = stream_primitive(ctx, name);
        }
        if (target && target->kind == SOURCE_IMPORT) target = imported_declaration(ctx, target, target->imported);
    } else if (callee->type == AST_MEMBER_ACCESS) {
        MemberAccessNode *member = &callee->as.member_access;
        SourceName *base = member->object->type == AST_VARIABLE ?
            visible_name(ctx, member->object->as.variable.name) : NULL;
        if (!base && member->object->type == AST_VARIABLE && !strcmp(member->object->as.variable.name, "Coro") &&
            !strcmp(member->name, "yield")) {
            if (call->arg_count || call->type_arg_count)
                return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Coro.yield accepts no value or type arguments");
            return emit(ctx, (XrXirInstruction) {XR_XIR_SUSPEND, XR_XIR_UNIT, {0}, {0}, 0, {0}}, value);
        }
        if (!source_static_select(ctx, callee, &selected)) return false;
        if (selected.method) {
            SourceTypeArguments arguments = {call->type_args, (uint32_t)call->type_arg_count, {0}};
            if (!source_method_instantiation(ctx, node, selected.method->index, selected.substitution, &arguments)) return false;
            selected.substitution = arguments.substitution;
            binding = target = selected.method;
        } else if (base && base->kind == SOURCE_MODULE) { binding = base; target = imported_declaration(ctx, base, member->name); }
        else if (source_constructor_receiver(ctx, member->object) &&
            !source_method_find(ctx, ctx->functions[ctx->function].result, member->name)) {
            if (!expression(ctx, callee, &indirect)) return false;
            indirect_ready = true;
        } else {
            if (base && xr_xir_type_is_array(&ctx->types, base->type))
                return source_array_call(ctx, node, NULL, value);
            XrXirType path_type;
            if (!source_path_type(ctx,member->object,&path_type)) return false;
            if (xr_xir_type_is_array(&ctx->types,path_type) &&
                (!strcmp(member->name,"set") || !strcmp(member->name,"push")))
                return source_array_call(ctx,node,NULL,value);
            if (!expression(ctx, member->object, &receiver)) return false;
            if (xr_xir_type_is_array(&ctx->types, receiver.type))
                return source_array_call(ctx, node, &receiver, value);
            if (receiver.type == XR_XIR_STRING) return source_string_call(ctx, node, receiver, value);
            if (receiver.type == XR_XIR_PANIC_INFO)
                return source_fail(ctx, node, XR_XIR_BAD_TYPE, "PanicInfo methods require class support");
            if (xr_xir_type_is_nominal(&ctx->types, receiver.type)) {
                SourceName *member_method = source_method_find(ctx, receiver.type, member->name);
                if (member_method) return source_method_call(ctx, node, receiver, member_method, value);
                if (!source_struct_get_value(ctx, callee, receiver, &indirect) ||
                    !source_query_expression(ctx, callee, indirect.type)) return false;
                indirect_ready = true;
            }
            if (receiver.type == XR_XIR_ATOMIC_I64) {
                if (!strcmp(member->name, "load") && !call->arg_count) method = XR_XIR_ATOMIC_I64_LOAD;
                if (!strcmp(member->name, "fetchAdd") && call->arg_count == 1) method = XR_XIR_ATOMIC_I64_FETCH_ADD;
            }
        }
    }
    if (ctx->diagnostic.status != XR_XIR_OK) return false;
    if (target && target->kind == SOURCE_NOMINAL) {
        if (call->type_arg_count < 0)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "invalid constructor type arguments");
        XrXirType type;
        return source_nominal_apply(ctx, target, call->type_args, (uint32_t)call->type_arg_count, &type) &&
            source_constructor_call(ctx, node, type, binding, target, value);
    }
    XrXirTypeNode signature = {0};
    if (!print && !atomic && !stream && method == XR_XIR_INVALID && (!target || target->kind != SOURCE_FUNCTION)) {
        if (call->type_arg_count) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "indirect call has no generic declaration parameters");
        if (!indirect_ready && !expression(ctx, callee, &indirect)) return false;
        const XrXirTypeNode *found = xr_xir_callable_signature(&ctx->types, indirect.type);
        if (!found || found->parameter_count != (uint32_t) call->arg_count)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "indirect call requires its declared signature");
        signature = *found;
    }
    if ((print || atomic || stream || method != XR_XIR_INVALID) && call->type_arg_count)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "primitive does not admit explicit type arguments");
    XrXirInstruction op = {0}; SourceSubstitution substitution = {0};
    if (selected.method) {
        substitution = selected.substitution;
        op = (XrXirInstruction) {XR_XIR_CALL, XR_XIR_UNIT, {0}, {0}, target->index, {0}};
        if (!source_argument_arity(ctx, node, target->index, (uint32_t)call->arg_count) ||
            !source_substitute(ctx, &substitution, ctx->functions[target->index].result, 0, &op.type)) return false;
    } else if (target && target->kind == SOURCE_FUNCTION && !prepare_call(ctx, node, target, &substitution, &op)) return false;
    if (target && target->kind == SOURCE_FUNCTION &&
        !source_query_reference(ctx, callee, binding, target, XR_XIR_SOURCE_CALL)) return false;
    uint32_t argument_count = op.op == XR_XIR_CALL ? ctx->functions[target->index].parameter_count : (uint32_t)call->arg_count;
    SourceValue *args = argument_count ? source_alloc(ctx, argument_count, sizeof(*args)) : NULL;
    if (argument_count && !args) return false;
    for (int i = 0; i < call->arg_count; ++i) {
        if (call->arg_accesses && call->arg_accesses[i] != XR_CALL_ARG_PLAIN)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "ref and move arguments require an implemented contract");
        XrXirType expected = XR_XIR_UNIT;
        if (op.op == XR_XIR_CALL && !source_substitute(ctx, &substitution,
            ctx->functions[target->index].parameters[i], 0, &expected)) return false;
        if (indirect.type) expected = signature.parameters[i].type;
        if (atomic || method == XR_XIR_ATOMIC_I64_FETCH_ADD) expected = XR_XIR_I64;
        if (stream) expected = XR_XIR_STRING;
        if (!expression_in(ctx, call->arguments[i], expected, &args[i])) return false;
        if (args[i].type == XR_XIR_UNIT) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "unit argument is not admitted");
    }
    if (indirect.type) {
        for (uint32_t p = 0; p < (uint32_t) call->arg_count; ++p)
            if (args[p].type != signature.parameters[p].type)
                return source_fail(ctx, node, XR_XIR_BAD_TYPE, "indirect argument type mismatch");
        return emit_group(ctx, (XrXirInstruction) {XR_XIR_CALL_INDIRECT, signature.result, {0}, {0}, indirect.id, {0}},
            args, (uint32_t) call->arg_count, value);
    }
    if (stream) {
        if (call->arg_count != 1 || args[0].type != XR_XIR_STRING)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "stream write requires one string");
        op = (XrXirInstruction) {XR_XIR_WRITE_STREAM, XR_XIR_BOOL, {args[0].id, 0}, {0, 0}, stream, {0}};
    } else if (print) {
        for (int i = 0; i < call->arg_count; ++i)
            if (args[i].type != XR_XIR_BOOL && !xr_xir_type_is_number(args[i].type) && args[i].type != XR_XIR_STRING)
                return source_fail(ctx, node, XR_XIR_BAD_TYPE, "print requires an admitted display type");
        op = (XrXirInstruction) {XR_XIR_PRINT, XR_XIR_UNIT, {0, 0}, {0, 0}, 0, {0}};
    } else if (atomic) {
        if (call->arg_count != 1 || args[0].type != XR_XIR_I64)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Atomic requires one i64 initializer");
        op = (XrXirInstruction) {XR_XIR_ATOMIC_I64_NEW, XR_XIR_ATOMIC_I64, {args[0].id, 0}, {0, 0}, 0, {0}};
    } else if (method != XR_XIR_INVALID) {
        if (method == XR_XIR_ATOMIC_I64_FETCH_ADD && args[0].type != XR_XIR_I64)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Atomic fetchAdd requires i64");
        op = (XrXirInstruction) {method, XR_XIR_I64, {receiver.id, call->arg_count ? args[0].id : 0}, {0, 0}, 0, {0}};
    }
    if (op.op == XR_XIR_CALL)
        for (uint32_t p = (uint32_t)call->arg_count; p < argument_count; ++p)
            if (!source_argument_default(ctx, node, target->index, p, &substitution, &args[p])) return false;
    if (op.op == XR_XIR_CALL && !source_type_arguments(ctx, node, substitution.types, substitution.count, &op)) return false;
    if (op.op == XR_XIR_CALL || op.op == XR_XIR_PRINT)
        return emit_group(ctx, op, args, argument_count, value);
    return emit(ctx, op, value);
}
static bool source_literal(SourceContext *ctx, AstNode *node, SourceValue *value) {
    if (node->type == AST_LITERAL_STRING) {
        if (ctx->literal_count == ctx->literal_capacity) {
            uint32_t capacity = ctx->literal_capacity ? ctx->literal_capacity * 2 : 16;
            if (capacity < ctx->literal_capacity) return source_fail(ctx, node, XR_XIR_BUDGET, "literal capacity overflow");
            XrXirLiteral *literals = source_alloc(ctx, capacity, sizeof(*literals));
            if (!literals) return false;
            if (ctx->literal_count) memcpy(literals, ctx->literals, ctx->literal_count * sizeof(*literals));
            ctx->literals = literals; ctx->literal_capacity = capacity;
        }
        const char *bytes = node->as.literal.raw_value.string_val;
        size_t length = node->as.literal.string_length;
        if (length > UINT32_MAX) return source_fail(ctx, node, XR_XIR_BUDGET, "string literal exceeds format limit");
        uint32_t id = ctx->literal_count++;
        ctx->literals[id] = (XrXirLiteral) {bytes, (uint32_t) length};
        return emit(ctx, (XrXirInstruction) {XR_XIR_CONST_STRING, XR_XIR_STRING, {0, 0}, {0, 0}, id, {0}}, value);
    }
    return emit(ctx, (XrXirInstruction) {XR_XIR_CONST_BOOL, XR_XIR_BOOL, {0, 0}, {0, 0},
        node->as.literal.raw_value.bool_val, {0}}, value);
}
static bool source_logic(SourceContext *ctx, AstNode *node, SourceValue *value) {
    bool negate = node->type == AST_UNARY_NOT;
    SourceValue left, initial, place, right;
    if (!expression(ctx, negate ? node->as.unary.operand : node->as.binary.left, &left)) return false;
    if (left.type != XR_XIR_BOOL) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "logical operand must be bool");
    initial = left;
    if (negate && !emit(ctx, (XrXirInstruction) {XR_XIR_CONST_BOOL, XR_XIR_BOOL, {0}, {0}, 0, {0}}, &initial)) return false;
    if (!emit(ctx, (XrXirInstruction) {XR_XIR_LOCAL_NEW, XR_XIR_BOOL, {initial.id, 0}, {0}, 0, {0}}, &place)) return false;
    SourceFunction *body = &ctx->bodies[ctx->function];
    uint32_t branch = body->count, rhs = body->block_count;
    if (!emit(ctx, (XrXirInstruction) {XR_XIR_BRANCH, XR_XIR_UNIT, {left.id, 0}, {0}, 0, {0}}, NULL) || !begin_block(ctx)) return false;
    if (negate) {
        if (!emit(ctx, (XrXirInstruction) {XR_XIR_CONST_BOOL, XR_XIR_BOOL, {0}, {0}, 1, {0}}, &right)) return false;
    } else if (!expression(ctx, node->as.binary.right, &right)) return false;
    if (right.type != XR_XIR_BOOL) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "logical operand must be bool");
    if (!emit(ctx, (XrXirInstruction) {XR_XIR_LOCAL_WRITE, XR_XIR_UNIT, {place.id, right.id}, {0}, 0, {0}}, NULL)) return false;
    uint32_t join = body->block_count;
    if (!emit(ctx, (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {join, 0}, 0, {0}}, NULL) || !begin_block(ctx)) return false;
    bool conjunction = node->type == AST_BINARY_AND;
    body->ops[branch].targets[0] = conjunction ? rhs : join;
    body->ops[branch].targets[1] = conjunction ? join : rhs;
    return emit(ctx, (XrXirInstruction) {XR_XIR_LOCAL_READ, XR_XIR_BOOL, {place.id, 0}, {0}, 0, {0}}, value);
}
static bool source_number_cast(SourceContext *ctx, AstNode *node, SourceValue *value) {
    AsExprNode *cast = &node->as.as_expr;
    XrXirType target; SourceValue input;
    if (cast->is_safe || !source_type(ctx, cast->type, &target) || !xr_xir_type_is_number(target))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "numeric cast requires a concrete nonnullable numeric target");
    SourceInteger literal;
    if (!source_direct_integer(ctx, cast->expr, &literal)) return false;
    XrXirType context = literal.present && !literal.negative && literal.magnitude > INT64_MAX ? XR_XIR_U64 : XR_XIR_UNIT;
    if (!expression_in(ctx, cast->expr, context, &input)) return false;
    if (!xr_xir_type_is_number(input.type))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "numeric cast requires a concrete numeric operand");
    return emit(ctx, (XrXirInstruction) {XR_XIR_CONVERT_NUMBER, target, {input.id, 0}, {0}, 0, {0}}, value);
}
static bool source_float_binary(SourceContext *ctx, AstNode *node, AstNodeType operation,
    SourceValue left, SourceValue right, SourceValue *value) {
    XrXirType common = left.type == XR_XIR_F64 || right.type == XR_XIR_F64 ? XR_XIR_F64 : XR_XIR_F32;
    if (!source_expect(ctx, node, common, &left) || !source_expect(ctx, node, common, &right)) return false;
    XrXirOp op;
    switch (operation) {
    case AST_BINARY_ADD: op = XR_XIR_ADD_FLOAT; break;
    case AST_BINARY_SUB: op = XR_XIR_SUB_FLOAT; break;
    case AST_BINARY_MUL: op = XR_XIR_MUL_FLOAT; break;
    case AST_BINARY_DIV: op = XR_XIR_DIV_FLOAT; break;
    case AST_BINARY_EQ: op = XR_XIR_EQ_FLOAT; break;
    case AST_BINARY_NE: op = XR_XIR_NE_FLOAT; break;
    case AST_BINARY_LT: op = XR_XIR_LT_FLOAT; break;
    case AST_BINARY_LE: op = XR_XIR_LE_FLOAT; break;
    case AST_BINARY_GT: op = XR_XIR_GT_FLOAT; break;
    case AST_BINARY_GE: op = XR_XIR_GE_FLOAT; break;
    default: return source_fail(ctx, node, XR_XIR_BAD_TYPE, "operator is not defined for floating operands");
    }
    XrXirType result = op >= XR_XIR_ADD_FLOAT && op <= XR_XIR_DIV_FLOAT ? common : XR_XIR_BOOL;
    return emit(ctx, (XrXirInstruction) {op, result, {left.id, right.id}, {0}, 0, {0}}, value);
}
static bool source_binary(SourceContext *ctx, AstNode *node, AstNodeType operation,
                          SourceValue left, SourceValue right, SourceValue *value) {
    if (operation == AST_BINARY_ADD && left.type == XR_XIR_STRING && right.type == XR_XIR_STRING)
        return emit(ctx, (XrXirInstruction) {XR_XIR_CONCAT_STRING, XR_XIR_STRING, {left.id, right.id}, {0}, 0, {0}}, value);
    if (left.type == XR_XIR_STRING && right.type == XR_XIR_STRING &&
        (operation == AST_BINARY_EQ || operation == AST_BINARY_NE))
        return emit(ctx, (XrXirInstruction) {operation == AST_BINARY_EQ ? XR_XIR_EQ_STRING : XR_XIR_NE_STRING,
            XR_XIR_BOOL, {left.id, right.id}, {0}, 0, {0}}, value);
    if (xr_xir_float_bits(left.type) && xr_xir_float_bits(right.type))
        return source_float_binary(ctx, node, operation, left, right, value);
    if (!xr_xir_type_is_integer(left.type) || !xr_xir_type_is_integer(right.type))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "operator requires a declared concrete operand contract");
    bool shift = operation == AST_BINARY_LSHIFT || operation == AST_BINARY_RSHIFT;
    if (!shift) {
        if (xr_xir_integer_signed(left.type) != xr_xir_integer_signed(right.type))
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "integer signedness requires an explicit cast");
        XrXirType target = xr_xir_integer_bits(left.type) < xr_xir_integer_bits(right.type) ? right.type : left.type;
        if (left.type != target && !emit(ctx, (XrXirInstruction) {XR_XIR_CONVERT_NUMBER, target, {left.id, 0}, {0}, 0, {0}}, &left)) return false;
        if (right.type != target && !emit(ctx, (XrXirInstruction) {XR_XIR_CONVERT_NUMBER, target, {right.id, 0}, {0}, 0, {0}}, &right)) return false;
    }
    XrXirOp op;
    switch (operation) {
    case AST_BINARY_ADD: op = XR_XIR_ADD_INT; break;
    case AST_UNARY_NEG: case AST_BINARY_SUB: op = XR_XIR_SUB_INT; break;
    case AST_BINARY_MUL: op = XR_XIR_MUL_INT; break;
    case AST_BINARY_DIV: op = XR_XIR_DIV_INT; break;
    case AST_BINARY_MOD: op = XR_XIR_REM_INT; break;
    case AST_BINARY_BAND: op = XR_XIR_AND_INT; break;
    case AST_BINARY_BOR: op = XR_XIR_OR_INT; break;
    case AST_UNARY_BNOT: case AST_BINARY_BXOR: op = XR_XIR_XOR_INT; break;
    case AST_BINARY_LSHIFT: op = XR_XIR_SHL_INT; break;
    case AST_BINARY_RSHIFT: op = XR_XIR_SHR_INT; break;
    case AST_BINARY_EQ: op = XR_XIR_EQ_INT; break;
    case AST_BINARY_NE: op = XR_XIR_NE_INT; break;
    case AST_BINARY_LT: op = XR_XIR_LT_INT; break;
    case AST_BINARY_LE: op = XR_XIR_LE_INT; break;
    case AST_BINARY_GT: op = XR_XIR_GT_INT; break;
    case AST_BINARY_GE: op = XR_XIR_GE_INT; break;
    default: return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "unknown arithmetic operator");
    }
    bool comparison = operation >= AST_BINARY_EQ && operation <= AST_BINARY_GE;
    return emit(ctx, (XrXirInstruction) {op, comparison ? XR_XIR_BOOL : left.type,
        {left.id, right.id}, {0}, 0, {0}}, value);
}
static bool source_arithmetic(SourceContext *ctx, AstNode *node, XrXirType expected, SourceValue *value) {
    SourceValue left, right;
    if (node->type == AST_UNARY_NEG || node->type == AST_UNARY_BNOT) {
        SourceInteger literal;
        if (!source_direct_integer(ctx, node->as.unary.operand, &literal) ||
            !expression_in(ctx, node->as.unary.operand,
                literal.present && xr_xir_type_is_integer(expected) ? expected : XR_XIR_UNIT, &right)) return false;
        if (node->type == AST_UNARY_NEG && xr_xir_float_bits(right.type))
            return emit(ctx, (XrXirInstruction) {XR_XIR_NEG_FLOAT, right.type, {right.id, 0}, {0}, 0, {0}}, value);
        if (!xr_xir_type_is_integer(right.type)) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "unary arithmetic requires an integer");
        int64_t initial = 0;
        if (node->type == AST_UNARY_BNOT) initial = xr_xir_integer_signed(right.type) || xr_xir_integer_bits(right.type) == 64 ?
            -1 : (int64_t) ((UINT64_C(1) << xr_xir_integer_bits(right.type)) - 1);
        if (!emit(ctx, (XrXirInstruction) {XR_XIR_CONST_INT, right.type, {0}, {0}, initial, {0}}, &left)) return false;
    } else {
        SourceInteger first, second;
        if (!source_direct_integer(ctx, node->as.binary.left, &first) ||
            !source_direct_integer(ctx, node->as.binary.right, &second)) return false;
        SourceDecimal a, b;
        if (!source_direct_decimal(ctx, node->as.binary.left, &a) || !source_direct_decimal(ctx, node->as.binary.right, &b)) return false;
        first.present = first.present || a.node; second.present = second.present || b.node;
        bool shift = node->type == AST_BINARY_LSHIFT || node->type == AST_BINARY_RSHIFT;
        XrXirType context = xr_xir_type_is_number(expected) ? expected : a.node || b.node ? XR_XIR_F64 : XR_XIR_UNIT;
        if (!shift && first.present && !second.present) {
            if (!expression(ctx, node->as.binary.right, &right) ||
                !expression_in(ctx, node->as.binary.left, right.type, &left)) return false;
        } else if (!expression_in(ctx, node->as.binary.left,
                       first.present && (shift || second.present) ? context : XR_XIR_UNIT, &left) ||
                   !expression_in(ctx, node->as.binary.right,
                       !shift && second.present ? left.type : XR_XIR_UNIT, &right)) return false;
    }
    return source_binary(ctx, node, node->type, left, right, value);
}
#include "xxir_source_update.inc.c"
static bool source_conditional(SourceContext *ctx, AstNode *node, XrXirType expected, SourceValue *value) {
    SourceValue condition, yes, no;
    TernaryNode *ternary = &node->as.ternary;
    if (!expression(ctx, ternary->condition, &condition)) return false;
    if (condition.type != XR_XIR_BOOL) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "conditional requires bool");
    SourceFunction *body = &ctx->bodies[ctx->function];
    if (!body->block_count && !begin_block(ctx)) return false;
    uint32_t branch = body->count, yes_block = body->block_count;
    if (!emit(ctx, (XrXirInstruction) {XR_XIR_BRANCH, XR_XIR_UNIT, {condition.id}, {0}, 0, {0}}, NULL) ||
        !begin_block(ctx) || !expression_in(ctx, ternary->true_expr, expected, &yes)) return false;
    uint32_t yes_end = body->block_count - 1, yes_jump = body->count;
    if (!emit(ctx, (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {0}, 0, {0}}, NULL)) return false;
    uint32_t no_block = body->block_count;
    if (!begin_block(ctx) || !expression_in(ctx, ternary->false_expr, expected, &no)) return false;
    XrXirType common = yes.type;
    if (yes.type != no.type) {
        if (xr_xir_float_bits(yes.type) && xr_xir_float_bits(no.type)) common = XR_XIR_F64;
        else if (!xr_xir_type_is_integer(yes.type) || !xr_xir_type_is_integer(no.type) ||
            xr_xir_integer_signed(yes.type) != xr_xir_integer_signed(no.type))
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "conditional requires a common admitted type");
        if (xr_xir_integer_bits(no.type) > xr_xir_integer_bits(common)) common = no.type;
    }
    if (!source_expect(ctx, node, common, &no)) return false;
    uint32_t no_end = body->block_count - 1, no_jump = body->count;
    if (!emit(ctx, (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {0}, 0, {0}}, NULL)) return false;
    if (yes.type != common) {
        body->ops[yes_jump].targets[0] = body->block_count;
        if (!begin_block(ctx) || !source_expect(ctx, node, common, &yes)) return false;
        yes_end = body->block_count - 1; yes_jump = body->count;
        if (!emit(ctx, (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {0}, 0, {0}}, NULL)) return false;
    }
    uint32_t join = body->block_count;
    if (!begin_block(ctx)) return false;
    body->ops[branch].targets[0] = yes_block; body->ops[branch].targets[1] = no_block;
    body->ops[yes_jump].targets[0] = join; body->ops[no_jump].targets[0] = join;
    if (yes.type == XR_XIR_UNIT) { *value = (SourceValue) {0, XR_XIR_UNIT}; return true; }
    SourceValue inputs[] = {{yes_end, XR_XIR_UNIT}, yes, {no_end, XR_XIR_UNIT}, no};
    if (yes_end > no_end) {
        inputs[0].id = no_end; inputs[1] = no; inputs[2].id = yes_end; inputs[3] = yes;
    }
    return emit_group(ctx, (XrXirInstruction) {XR_XIR_PHI, yes.type, {0}, {0}, 0, {0}}, inputs, 4, value);
}
#include "xxir_source_match.inc.c"
static bool source_function_value(SourceContext *ctx, AstNode *node, SourceName *binding, SourceName *symbol, XrXirType expected, SourceValue *value) {
    if (symbol && symbol->kind == SOURCE_IMPORT) symbol = imported_declaration(ctx, symbol, symbol->imported);
    if (!symbol || symbol->kind != SOURCE_FUNCTION)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "function value requires a declared function");
    if (!source_query_reference(ctx, node, binding, symbol, XR_XIR_SOURCE_FUNCTION_VALUE)) return false;
    const XrXirFunction *function = &ctx->functions[symbol->index];
    SourceTypeArguments arguments = {0};
    if (node->type == AST_FUNCTION_REF) {
        arguments.refs = node->as.function_ref.type_args;
        arguments.count = (uint32_t) node->as.function_ref.type_arg_count;
    }
    XrXirInstruction op = {XR_XIR_FUNCTION_REF,XR_XIR_UNIT,{0},{0},symbol->index, {0}};
    if (!source_instantiation(ctx,node,&ctx->generics[symbol->index],&arguments)) return false;
    uint32_t count = function->parameter_count;
    XrXirCallableParameter *parameters = count ? source_alloc(ctx,count,sizeof(*parameters)) : NULL;
    if (count && !parameters) return false;
    for (uint32_t p = 0; p < count; ++p)
        if (!source_substitute(ctx,&arguments.substitution,function->parameters[p],0,&parameters[p].type)) return false;
    XrXirType result;
    if (!source_substitute(ctx,&arguments.substitution,function->result,0,&result) ||
        !source_signature(ctx,parameters,count,result,&op.type)) return false;
    if (!source_reference_promise(ctx, node, symbol->index, expected, &op.type)) return false;
    return source_type_arguments(ctx, node, arguments.substitution.types, arguments.count, &op) && emit(ctx,op,value);
}
static bool source_explicit_reference(SourceContext *ctx, AstNode *node, XrXirType expected, SourceValue *value) {
    AstNode *callee = node->as.function_ref.callee;
    SourceName *symbol = NULL, *binding = NULL;
    if (callee && callee->type == AST_VARIABLE) binding = symbol = visible_name(ctx,callee->as.variable.name);
    else if (callee && callee->type == AST_MEMBER_ACCESS) {
        MemberAccessNode *member = &callee->as.member_access;
        SourceName *base = member->object->type == AST_VARIABLE ? visible_name(ctx,member->object->as.variable.name) : NULL;
        if (base && base->kind == SOURCE_MODULE) { binding = base; symbol = imported_declaration(ctx,base,member->name); }
        else {
            SourceTypeArguments arguments = {node->as.function_ref.type_args, (uint32_t)node->as.function_ref.type_arg_count, {0}};
            return source_member_value(ctx, callee, &arguments, expected, value);
        }
    }
    return source_function_value(ctx,node,binding,symbol,expected,value);
}
static bool source_closure(SourceContext *ctx, AstNode *node, XrXirType expected, SourceValue *value);
static bool source_defer(SourceContext *ctx, AstNode *node);
static bool expression_body(SourceContext *ctx, AstNode *node, XrXirType expected, SourceValue *value) {
    switch (node->type) {
    case AST_ARRAY_LITERAL: return source_array_literal(ctx, node, expected, value);
    case AST_INDEX_GET: return source_array_get(ctx, node, node->as.index_get.array,
        node->as.index_get.index, NULL, NULL, value);
    case AST_INDEX_SET: return source_array_set(ctx, node, node->as.index_set.array,
        node->as.index_set.index, node->as.index_set.value, NULL, true, value);
    case AST_LITERAL_TRUE: case AST_LITERAL_FALSE: case AST_LITERAL_STRING:
        return source_literal(ctx, node, value);
    case AST_AS_EXPR: return source_number_cast(ctx, node, value);
    case AST_TERNARY: return source_conditional(ctx, node, expected, value);
    case AST_MATCH_EXPR: return source_match(ctx, node, expected, true, value);
    case AST_GROUPING: return expression_in(ctx, node->as.grouping, expected, value);
    case AST_CALL_EXPR: return source_call(ctx, node, value);
    case AST_FUNCTION_REF: return source_explicit_reference(ctx, node, expected, value);
    case AST_FUNCTION_EXPR: return source_closure(ctx, node, expected, value);
    case AST_STRUCT_LITERAL: return source_struct_literal(ctx, node, value);
    case AST_ENUM_CONSTRUCT: return source_enum_literal(ctx, node, value);
    case AST_MEMBER_SET: return source_struct_set(ctx, node, value);
    case AST_UNARY_NOT: case AST_BINARY_AND: case AST_BINARY_OR: return source_logic(ctx, node, value);
    case AST_MEMBER_ACCESS: {
        MemberAccessNode *member = &node->as.member_access;
        SourceName *base = member->object->type == AST_VARIABLE ? visible_name(ctx, member->object->as.variable.name) : NULL;
        SourceTypeArguments arguments = {0};
        if (!base || base->kind != SOURCE_MODULE) return source_member_value(ctx, node, &arguments, expected, value);
        return source_function_value(ctx, node, base, imported_declaration(ctx, base, member->name), expected, value);
    }
    case AST_THIS_EXPR: case AST_VARIABLE: {
        SourceName *symbol = visible_name(ctx, node->type == AST_THIS_EXPR ? "this" : node->as.variable.name);
        if (symbol && (symbol->kind == SOURCE_FUNCTION || symbol->kind == SOURCE_IMPORT))
            return source_function_value(ctx, node, symbol, symbol, expected, value);
        if (!symbol || (symbol->kind != SOURCE_SLOT && symbol->kind != SOURCE_LOCAL) || !symbol->type)
            return source_fail(ctx, node, XR_XIR_BAD_VALUE, "name is not an initialized value");
        if (!source_query_reference(ctx, node, symbol, symbol, XR_XIR_SOURCE_READ)) return false;
        if (symbol->kind == SOURCE_LOCAL) {
            if (symbol->construction) return source_constructor_value(ctx, node, value);
            if (symbol->mutable) return emit(ctx, (XrXirInstruction) {XR_XIR_CELL_READ, symbol->type,
                {symbol->index, 0}, {0}, 0, {0}}, value);
            *value = (SourceValue) {symbol->index, symbol->type}; return true;
        }
        return emit(ctx, (XrXirInstruction) {XR_XIR_SLOT_LOAD, symbol->type, {0, 0}, {0, 0}, symbol->index, {0}}, value);
    }
    case AST_UNARY_BNOT: case AST_BINARY_BAND: case AST_BINARY_BOR: case AST_BINARY_BXOR:
    case AST_BINARY_LSHIFT: case AST_BINARY_RSHIFT:
    case AST_UNARY_NEG: case AST_BINARY_ADD: case AST_BINARY_SUB: case AST_BINARY_MUL:
    case AST_BINARY_DIV: case AST_BINARY_MOD: case AST_BINARY_EQ: case AST_BINARY_NE:
    case AST_BINARY_LT: case AST_BINARY_LE: case AST_BINARY_GT: case AST_BINARY_GE:
        return source_arithmetic(ctx, node, expected, value);
    case AST_COMPOUND_ASSIGNMENT: return source_compound(ctx, node, value);
    case AST_ASSIGNMENT: {
        SourceName *symbol = visible_name(ctx, node->as.assignment.name);
        SourceValue assigned;
        if (!symbol || !symbol->mutable || (symbol->kind != SOURCE_LOCAL && symbol->kind != SOURCE_SLOT))
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "assignment requires a mutable binding");
        if (!source_query_reference(ctx, node, symbol, symbol, XR_XIR_SOURCE_WRITE)) return false;
        if (!expression_in(ctx, node->as.assignment.value, symbol->type, &assigned)) return false;
        if (assigned.type != symbol->type) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "assignment type mismatch");
        if (symbol->kind == SOURCE_LOCAL) {
            if (!emit(ctx, (XrXirInstruction) {XR_XIR_CELL_WRITE, XR_XIR_UNIT,
                {symbol->index, assigned.id}, {0}, 0, {0}}, NULL)) return false;
        }
        else if (!emit(ctx, (XrXirInstruction) {XR_XIR_SLOT_STORE, XR_XIR_UNIT, {assigned.id, 0}, {0, 0}, symbol->index, {0}}, NULL)) return false;
        *value = assigned; return true;
    }
    default: return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "expression syntax is not implemented in XIR");
    }
}
static bool expression(SourceContext *ctx, AstNode *node, SourceValue *value) {
    return expression_in(ctx, node, XR_XIR_UNIT, value);
}
static bool expression_in(SourceContext *ctx, AstNode *node, XrXirType expected, SourceValue *value) {
    if (!node || !source_work(ctx, node)) return false;
    if (ctx->depth >= 128) return source_fail(ctx, node, XR_XIR_BUDGET, "source expression depth exhausted");
    ++ctx->depth;
    SourceInteger literal; SourceDecimal decimal;
    bool result = source_direct_integer(ctx, node, &literal) && source_direct_decimal(ctx, node, &decimal) &&
        (literal.present ? source_integer(ctx, node, &literal, expected, value) : decimal.node ?
         source_decimal(ctx, &decimal, expected, value) : expression_body(ctx, node, expected, value));
    --ctx->depth;
    return result && source_expect(ctx, node, expected, value) && source_query_expression(ctx, node, value->type);
}
static bool source_binding(SourceContext *ctx, AstNode *node, bool top) {
    VarDeclNode *decl = &node->as.var_decl;
    if (decl->attr_count || (!decl->initializer && (decl->is_const || !decl->type_annotation)))
        return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "binding needs an initializer or explicit mutable type and no attributes");
    SourceValue initial;
    XrXirType annotation = XR_XIR_UNIT;
    if (decl->type_annotation && !source_type(ctx, decl->type_annotation, &annotation)) return false;
    if (decl->initializer) {
        if (!expression_in(ctx, decl->initializer, annotation, &initial)) return false;
    } else if (!source_default_value(ctx, node, annotation, &initial)) return false;
    if (initial.type == XR_XIR_UNIT) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "binding cannot store unit");
    if (decl->type_annotation && annotation != initial.type)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "binding annotation mismatch");
    SourceName *symbol;
    if (top) {
        symbol = find_name(ctx, ctx->names[ctx->module], decl->name);
        if (!symbol || symbol->kind != SOURCE_SLOT) return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "binding declaration missing");
        symbol->type = initial.type; ctx->slots[symbol->index].type = initial.type;
        source_query_binding_type(ctx, symbol);
        return emit(ctx, (XrXirInstruction) {XR_XIR_SLOT_INIT, XR_XIR_UNIT, {initial.id, 0}, {0, 0}, symbol->index, {0}}, NULL);
    }
    for (SourceName *p = ctx->locals; p != ctx->scope; p = p->next) {
        if (!source_work(ctx, node)) return false;
        if (!strcmp(p->name, decl->name)) return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "duplicate local name");
    }
    symbol = source_alloc(ctx, 1, sizeof(*symbol));
    if (!symbol) return false;
    XrXirType logical_type = initial.type;
    if (!decl->is_const) {
        XrXirType cell;
        if (!source_cell_type(ctx, initial.type, &cell) ||
            !emit(ctx, (XrXirInstruction) {XR_XIR_CELL_NEW, cell, {initial.id, 0}, {0}, 0, {0}}, &initial)) return false;
    }
    *symbol = (SourceName) {ctx->locals, decl->name, NULL, node, SOURCE_LOCAL, initial.id, ctx->module, logical_type, !decl->is_const, false, 0};
    ctx->locals = symbol;
    if (!source_query_declare(ctx, symbol, XR_XIR_SOURCE_BINDING, ctx->bodies[ctx->function].declaration,
        source_query_range(ctx, node, symbol->name))) return false;
    source_query_binding_type(ctx, symbol);
    return true;
}
static bool scoped_statement(SourceContext *ctx, AstNode *node) {
    if (ctx->depth >= 128) return source_fail(ctx, node, XR_XIR_BUDGET, "source control depth exhausted");
    SourceName *saved = ctx->locals, *scope = ctx->scope;
    ctx->scope = saved; ++ctx->depth;
    bool result = statement(ctx, node, false);
    --ctx->depth; ctx->locals = saved; ctx->scope = scope; return result;
}
static bool source_if(SourceContext *ctx, AstNode *node) {
    SourceValue condition;
    if (!expression(ctx, node->as.if_stmt.condition, &condition)) return false;
    if (condition.type != XR_XIR_BOOL) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "if requires bool");
    SourceFunction *body = &ctx->bodies[ctx->function];
    if (!body->block_count && !begin_block(ctx)) return false;
    uint32_t branch = body->count, yes = body->block_count;
    if (!emit(ctx, (XrXirInstruction) {XR_XIR_BRANCH, XR_XIR_UNIT, {condition.id, 0}, {0}, 0, {0}}, NULL) ||
        !begin_block(ctx) || !scoped_statement(ctx, node->as.if_stmt.then_branch)) return false;
    uint32_t yes_jump = UINT32_MAX, no_jump = UINT32_MAX;
    if (!ctx->returned) {
        yes_jump = body->count;
        if (!emit(ctx, (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {0}, 0, {0}}, NULL)) return false;
    }
    uint32_t no = body->block_count;
    if (!begin_block(ctx)) return false;
    if (node->as.if_stmt.else_branch && !scoped_statement(ctx, node->as.if_stmt.else_branch)) return false;
    if (!ctx->returned) {
        no_jump = body->count;
        if (!emit(ctx, (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {0}, 0, {0}}, NULL)) return false;
    }
    body->ops[branch].targets[0] = yes; body->ops[branch].targets[1] = no;
    ctx->returned = yes_jump == UINT32_MAX && no_jump == UINT32_MAX;
    if (!ctx->returned) {
        uint32_t join = body->block_count;
        if (!begin_block(ctx)) return false;
        if (yes_jump != UINT32_MAX) body->ops[yes_jump].targets[0] = join;
        if (no_jump != UINT32_MAX) body->ops[no_jump].targets[0] = join;
    }
    return true;
}
static bool source_increment(SourceContext *ctx, AstNode *node) {
    const char *name = node->type == AST_INC ? node->as.inc.name : node->as.dec.name;
    SourceName *symbol = visible_name(ctx, name);
    if (!symbol || !symbol->mutable || !xr_xir_type_is_integer(symbol->type) ||
        (symbol->kind != SOURCE_LOCAL && symbol->kind != SOURCE_SLOT))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "increment requires a mutable integer binding");
    if (!source_query_reference(ctx, node, symbol, symbol, XR_XIR_SOURCE_READ_WRITE)) return false;
    SourceValue old, one, result;
    XrXirInstruction read = symbol->kind == SOURCE_LOCAL ?
        (XrXirInstruction) {XR_XIR_CELL_READ, symbol->type, {symbol->index, 0}, {0}, 0, {0}} :
        (XrXirInstruction) {XR_XIR_SLOT_LOAD, symbol->type, {0}, {0}, symbol->index, {0}};
    if (!emit(ctx, read, &old) ||
        !emit(ctx, (XrXirInstruction) {XR_XIR_CONST_INT, symbol->type, {0}, {0}, 1, {0}}, &one) ||
        !emit(ctx, (XrXirInstruction) {node->type == AST_INC ? XR_XIR_ADD_INT : XR_XIR_SUB_INT, symbol->type, {old.id, one.id}, {0}, 0, {0}}, &result)) return false;
    XrXirInstruction write = symbol->kind == SOURCE_LOCAL ?
        (XrXirInstruction) {XR_XIR_CELL_WRITE, XR_XIR_UNIT, {symbol->index, result.id}, {0}, 0, {0}} :
        (XrXirInstruction) {XR_XIR_SLOT_STORE, XR_XIR_UNIT, {result.id, 0}, {0}, symbol->index, {0}};
    return emit(ctx, write, NULL);
}
static bool source_step(SourceContext *ctx, AstNode *node) {
    if (!node) return true;
    if (!source_work(ctx, node)) return false;
    if (node->type == AST_INC || node->type == AST_DEC) return source_increment(ctx, node);
    SourceValue ignored; return expression(ctx, node, &ignored);
}
static bool check_dead_step(SourceContext *ctx, AstNode *node) {
    if (!node) return true;
    SourceFunction *body = &ctx->bodies[ctx->function];
    SourceFunction saved = *body;
    XrXirGeneric generic = ctx->generics[ctx->function];
    bool returned = ctx->returned;
    bool result = begin_block(ctx) && source_step(ctx, node);
    *body = saved; ctx->generics[ctx->function] = generic; ctx->returned = returned;
    return result;
}
static bool patch_exits(SourceContext *ctx, SourcePatch *patch, uint32_t target) {
    for (; patch; patch = patch->next) {
        if (!source_work(ctx, NULL)) return false;
        ctx->bodies[ctx->function].ops[patch->instruction].targets[0] = target;
    }
    return true;
}
static bool source_loop(SourceContext *ctx, AstNode *node, AstNode *condition_node, AstNode *loop_body, AstNode *step) {
    SourceFunction *body = &ctx->bodies[ctx->function];
    if (!body->block_count && !begin_block(ctx)) return false;
    uint32_t condition_block = body->block_count;
    if (!emit(ctx, (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {condition_block, 0}, 0, {0}}, NULL) || !begin_block(ctx)) return false;
    SourceValue condition;
    if (condition_node) { if (!expression(ctx, condition_node, &condition)) return false; }
    else if (!emit(ctx, (XrXirInstruction) {XR_XIR_CONST_BOOL, XR_XIR_BOOL, {0}, {0}, 1, {0}}, &condition)) return false;
    if (condition.type != XR_XIR_BOOL) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "loop condition requires bool");
    uint32_t branch = body->count, entry = body->block_count;
    if (!emit(ctx, (XrXirInstruction) {XR_XIR_BRANCH, XR_XIR_UNIT, {condition.id, 0}, {entry, 0}, 0, {0}}, NULL) || !begin_block(ctx)) return false;
    SourceLoop loop = {ctx->loop, NULL, NULL, body->frontier}; ctx->loop = &loop;
    bool ok = scoped_statement(ctx, loop_body); ctx->loop = loop.parent;
    if (!ok) return false;
    if (!ctx->returned || loop.continues) {
        uint32_t next = step ? body->block_count : condition_block;
        if (!ctx->returned && !emit(ctx, (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {next, 0}, 0, {0}}, NULL)) return false;
        if (!patch_exits(ctx, loop.continues, next)) return false;
        if (step && (!begin_block(ctx) || !source_step(ctx, step) ||
            !emit(ctx, (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {condition_block, 0}, 0, {0}}, NULL))) return false;
    } else if (!check_dead_step(ctx, step)) return false;
    uint32_t exit = body->block_count;
    if (!begin_block(ctx)) return false;
    body->ops[branch].targets[1] = exit;
    return patch_exits(ctx, loop.breaks, exit);
}
static bool source_for(SourceContext *ctx, AstNode *node) {
    ForStmtNode *loop = &node->as.for_stmt;
    if (loop->label) return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "labelled loops are not admitted");
    SourceName *saved = ctx->locals, *scope = ctx->scope; ctx->scope = saved;
    bool ok = !loop->initializer || statement(ctx, loop->initializer, false);
    if (ok) ok = source_loop(ctx, node, loop->condition, loop->body, loop->increment);
    ctx->locals = saved; ctx->scope = scope; return ok;
}
static bool source_loop_exit(SourceContext *ctx, AstNode *node) {
    bool stop = node->type == AST_BREAK_STMT;
    const char *label = stop ? node->as.break_stmt.label : node->as.continue_stmt.label;
    if (!ctx->loop || label) return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE,
        ctx->identities[ctx->function].cleanup_owner ? "E0395: defer cannot exit an enclosing loop outside its body" :
        "loop exit requires an unlabelled enclosing loop");
    SourcePatch **head = stop ? &ctx->loop->breaks : &ctx->loop->continues;
    SourcePatch *patch = source_alloc(ctx, 1, sizeof(*patch)); if (!patch) return false;
    *patch = (SourcePatch) {*head, ctx->bodies[ctx->function].count}; *head = patch;
    ctx->returned = true;
    uint32_t target = ctx->loop->frontier;
    bool leaving = ctx->bodies[ctx->function].frontier != target;
    return emit(ctx, (XrXirInstruction) {leaving ? XR_XIR_CLEANUP_LEAVE : XR_XIR_JUMP,
        XR_XIR_UNIT, {0}, {0}, leaving ? target : 0, {0}}, NULL);
}
#include "xxir_source_catch.inc.c"
static bool statement(SourceContext *ctx, AstNode *node, bool top) {
    if (!node || !source_work(ctx, node)) return false;
    if (ctx->returned) return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "unreachable statements are not admitted");
    switch (node->type) {
    case AST_DEFER_STMT: return source_defer(ctx, node);
    case AST_TRY_CATCH: return source_try(ctx, node);
    case AST_IF_STMT: return source_if(ctx, node);
    case AST_WHILE_STMT:
        if (node->as.while_stmt.label) return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "labelled loops are not admitted");
        return source_loop(ctx, node, node->as.while_stmt.condition, node->as.while_stmt.body, NULL);
    case AST_FOR_STMT: return source_for(ctx, node);
    case AST_INC: case AST_DEC: return source_increment(ctx, node);
    case AST_BREAK_STMT: case AST_CONTINUE_STMT: return source_loop_exit(ctx, node);
    case AST_IMPORT_STMT: case AST_FUNCTION_DECL: case AST_STRUCT_DECL: case AST_ENUM_DECL:
        return top || source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "nested declarations are not admitted");
    case AST_VAR_DECL: case AST_CONST_DECL: return source_binding(ctx, node, top);
    case AST_EXPR_STMT: {
        SourceValue value; AstNode *expr=node->as.expr_stmt;
        if (expr->type!=AST_MATCH_EXPR) return expression(ctx,expr,&value);
        if (ctx->depth>=128) return source_fail(ctx,expr,XR_XIR_BUDGET,"source match depth exhausted");
        ++ctx->depth;
        bool ok=source_match(ctx,expr,XR_XIR_UNIT,false,&value);
        --ctx->depth;
        return ok && (ctx->returned || source_query_expression(ctx,expr,value.type));
    }
    case AST_THROW_STMT: {
        SourceValue value = {0};
        if (!expression(ctx, node->as.throw_stmt.expression, &value)) return false;
        const XrXirGeneric *generic = &ctx->generics[ctx->function];
        XrXirStatus status = xr_xir_type_markers(&ctx->types, value.type, XR_XIR_CONSTRAINT_ERROR,
            generic->constraints, generic->parameter_count, &ctx->budget.work);
        if (status != XR_XIR_OK)
            return source_fail(ctx, node, status, "throw requires a proved enum error value");
        if (ctx->bodies[ctx->function].error_context) {
            if (!source_expect(ctx,node,XR_XIR_ERROR,&value) || !source_error_edge(ctx,value)) return false;
            ctx->returned = true; return true;
        }
        ctx->returned = true;
        return emit(ctx, (XrXirInstruction) {XR_XIR_THROW, XR_XIR_UNIT, {value.id, 0}, {0, 0}, 0, {0}}, NULL);
    }
    case AST_RETURN_STMT: {
        if (ctx->identities[ctx->function].cleanup_owner)
            return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "E0395: defer cannot return from its owning function");
        if (source_constructor_active(ctx)) return source_constructor_return(ctx, node);
        if (top || !ctx->bodies[ctx->function].node || node->as.return_stmt.value_count > 1)
            return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "invalid return placement or arity");
        SourceValue value = {0};
        SourceFunction *body = &ctx->bodies[ctx->function];
        XrXirType expected = body->infer_result ? XR_XIR_UNIT : ctx->functions[ctx->function].result;
        if (node->as.return_stmt.value_count && !expression_in(ctx, node->as.return_stmt.values[0], expected, &value)) return false;
        if (body->infer_result && !body->saw_return) ctx->functions[ctx->function].result = value.type;
        body->saw_return = true;
        if (value.type != ctx->functions[ctx->function].result)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "return type does not match declaration");
        ctx->returned = true;
        return emit(ctx, (XrXirInstruction) {XR_XIR_RETURN, XR_XIR_UNIT, {value.type == XR_XIR_UNIT ? 0 : value.id, 0}, {0, 0}, 0, {0}}, NULL);
    }
    case AST_BLOCK: {
        if (ctx->depth >= 128) return source_fail(ctx, node, XR_XIR_BUDGET, "source block depth exhausted");
        SourceName *saved = ctx->locals, *scope = ctx->scope;
        SourceFunction *body = &ctx->bodies[ctx->function]; uint32_t entry = body->frontier;
        ctx->scope = saved; ++ctx->depth; ++body->lexical_depth;
        bool ok = true;
        for (int i = 0; ok && i < node->as.block.count; ++i)
            ok = statement(ctx, node->as.block.statements[i], false);
        --ctx->depth; --body->lexical_depth; ctx->locals = saved; ctx->scope = scope;
        if (ok && !ctx->returned && body->frontier != entry) {
            ok = emit(ctx, (XrXirInstruction){XR_XIR_CLEANUP_LEAVE, XR_XIR_UNIT, {0}, {body->block_count}, entry, {0}}, NULL);
            body->frontier = entry;
            if (ok) ok = begin_block(ctx);
        } else body->frontier = entry;
        return ok;
    }
    default: return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "statement syntax is not implemented in XIR");
    }
}
static bool declare_function(SourceContext *ctx, AstNode *node, uint32_t index) {
    FunctionDeclNode *decl = &node->as.function_decl;
    if (decl->is_generator || decl->is_extern || decl->attr_count || decl->type_param_count < 0 || decl->type_param_count > 65536 ||
        decl->throws_count || decl->borrow_origin_count || !decl->body || decl->param_count > 65536 || decl->param_count < 0)
        return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "function contract is not implemented in XIR");
    SourceName *symbol = add_name(ctx, &ctx->names[ctx->module], decl->name, node);
    if (!symbol) return false;
    symbol->kind = SOURCE_FUNCTION; symbol->index = index;
    SourceFunction *body = &ctx->bodies[index]; body->node = body->type_owner = node; body->module = ctx->module;
    ctx->function = index;
    if (!source_query_declare(ctx, symbol, XR_XIR_SOURCE_FUNCTION, 0, source_query_range(ctx, node, decl->name))) return false;
    body->declaration = symbol->declaration;
    body->generic_owner = decl->type_param_count ? symbol->declaration : 0;
    body->type_parameters = decl->type_params; body->type_parameter_count = (uint32_t)decl->type_param_count;
    uint32_t *constraints = decl->type_param_count ? source_alloc(ctx, (size_t) decl->type_param_count, sizeof(*constraints)) : NULL;
    if (decl->type_param_count && !constraints) return false;
    ctx->generics[index].constraints = constraints;
    ctx->generics[index].parameter_count = (uint32_t) decl->type_param_count;
    ctx->has_generics |= decl->type_param_count != 0;
    for (int i = 0; i < decl->type_param_count; ++i) {
        XrGenericParam *parameter = decl->type_params[i];
        if (!source_parameter_markers(ctx, node, parameter, &constraints[i])) return false;
        for (int j = 0; j < i; ++j) {
            if (!source_work(ctx, node)) return false;
            if (!strcmp(parameter->name, decl->type_params[j]->name))
                return source_fail(ctx, node, XR_XIR_BAD_TYPE, "duplicate type parameter");
        }
    }
    body->parameters = decl->param_count ? source_alloc(ctx, (size_t) decl->param_count, sizeof(*body->parameters)) : NULL;
    if (decl->param_count && !body->parameters) return false;
    XrXirFunction *function = &ctx->functions[index];
    *function = (XrXirFunction) {decl->name, (uint32_t) strlen(decl->name), body->parameters,
        (uint32_t) decl->param_count, XR_XIR_UNIT, NULL, 0, NULL, 0, NULL, 0};
    if (!source_type(ctx, decl->return_type, &function->result)) return false;
    for (int i = 0; i < decl->param_count; ++i) {
        XrParamNode *param = decl->params[i];
        if (!param->type || param->passing_mode != XR_PARAM_READ || param->pattern || param->is_rest ||
            !source_type(ctx, param->type, &body->parameters[i]) || body->parameters[i] == XR_XIR_UNIT)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "parameter contract is not implemented in XIR");
        for (int j = 0; j < i; ++j) {
            if (!source_work(ctx, node)) return false;
            if (!strcmp(param->name, decl->params[j]->name))
                return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "duplicate parameter name");
        }
    }
    ctx->identities[index] = (XrXirFunctionIdentity) {ctx->module, node->is_exported, 0, 0, 0, 0};
    return source_query_parameters(ctx, symbol->declaration);
}
static bool source_same_text(const char *first, const char *second) {
    return first == second || (first && second && !strcmp(first, second));
}
static bool source_resolved_identity(const XrModuleSpec *spec, const XrModuleId *id) {
    return spec->kind == id->kind && spec->authority.kind == id->authority.kind &&
        source_same_text(spec->logical_path, id->logical_path) &&
        source_same_text(spec->authority.namespace_id, id->authority.namespace_id) &&
        source_same_text(spec->authority.physical_root, id->authority.physical_root) &&
        (spec->embedded_source ? !id->source_path : source_same_text(spec->source_path, id->source_path));
}
static bool declare_import(SourceContext *ctx, AstNode *node) {
    ImportStmtNode *decl = &node->as.import_stmt;
    XrModuleSpec *spec = &ctx->graph->specs[ctx->module];
    XrModuleId id = {0}; char *error = NULL;
    int resolved = xr_module_resolver_resolve(ctx->graph->resolver, decl->module_name, spec->source_path, &spec->authority, &id, &error);
    int target = resolved == 0 && id.canonical && id.logical_path && xr_module_identity_authority_valid(&id.authority) ?
        xr_module_graph_find(ctx->graph, id.canonical) : -1;
    if (target >= 0 && !source_resolved_identity(&ctx->graph->specs[target], &id)) target = -1;
    xr_free(error); xr_module_id_cleanup(&id);
    if (target < 0) {
        ctx->query_ready = false;
        return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "import does not resolve to the parsed graph");
    }
    for (int i = 0; i < (decl->member_count ? decl->member_count : 1); ++i) {
        const char *name = decl->member_count ? (decl->members[i].alias ? decl->members[i].alias : decl->members[i].name) : decl->alias;
        SourceName *symbol = add_name(ctx, &ctx->names[ctx->module], name, node);
        if (!symbol) return false;
        symbol->kind = decl->member_count ? SOURCE_IMPORT : SOURCE_MODULE;
        symbol->module = (uint32_t) target;
        symbol->imported = decl->member_count ? decl->members[i].name : NULL;
        /* Import aliases do not yet have individual parser token locations. */
        if (!source_query_declare(ctx, symbol, decl->member_count ? XR_XIR_SOURCE_IMPORT : XR_XIR_SOURCE_MODULE,
            0, (XrXirSourceRange) {ctx->module, 0, 0, 0, 0})) return false;
    }
    return true;
}
typedef struct SourceClosureCount { SourceContext *ctx; uint32_t count, depth, defaults; } SourceClosureCount;
static bool count_closures(AstNode *node, void *pointer) {
    SourceClosureCount *scan = pointer;
    if (!node) return true;
    if (!source_work(scan->ctx,node)) return false;
    if (scan->depth == 128 || ((node->type == AST_FUNCTION_EXPR || node->type == AST_DEFER_STMT) && scan->count == scan->ctx->budget.functions))
        return source_fail(scan->ctx,node,XR_XIR_BUDGET,"closure declaration budget exhausted");
    if (node->type == AST_FUNCTION_EXPR || node->type == AST_DEFER_STMT) ++scan->count;
    XrParamNode **parameters = node->type == AST_FUNCTION_DECL ? node->as.function_decl.params :
        node->type == AST_METHOD_DECL ? node->as.method_decl.params : NULL;
    int count = node->type == AST_FUNCTION_DECL ? node->as.function_decl.param_count :
        node->type == AST_METHOD_DECL ? node->as.method_decl.param_count : 0;
    for (int p = 0; p < count; ++p) {
        if (!source_work(scan->ctx, node)) return false;
        if (parameters[p]->default_value) {
            if (scan->defaults == scan->ctx->budget.functions)
                return source_fail(scan->ctx, node, XR_XIR_BUDGET, "default argument function budget exhausted");
            ++scan->defaults;
        }
    }
    ++scan->depth;
    bool ok = xr_ast_for_each_child(node,count_closures,scan);
    --scan->depth;
    return ok || source_fail(scan->ctx,node,XR_XIR_BAD_STRUCTURE,"unknown source declaration shape");
}
static bool collect_declarations(SourceContext *ctx) {
    uint32_t count = (uint32_t) ctx->graph->spec_count, functions = count + 1, slots = 0, nominals = 0;
    SourceClosureCount closures = {ctx,0,0,0};
    for (uint32_t m = 0; m < count; ++m) {
        ctx->module = m;
        AstNode *ast = ctx->graph->specs[m].ast;
        if (!count_closures(ast,&closures)) return false;
        if (!ast || ast->type != AST_PROGRAM) return source_fail(ctx, ast, XR_XIR_BAD_STRUCTURE, "parsed module required");
        for (int i = 0; i < ast->as.program.count; ++i) {
            AstNode *node = ast->as.program.statements[i];
            if (!source_work(ctx, node)) return false;
            if (node->type == AST_FUNCTION_DECL) ++functions;
            if (node->type == AST_STRUCT_DECL || node->type == AST_ENUM_DECL) ++nominals;
            if (node->type == AST_STRUCT_DECL) {
                ClassDeclNode *decl = &node->as.struct_decl;
                if (functions > ctx->budget.functions || decl->method_count < 0 || (uint32_t)decl->method_count > ctx->budget.functions - functions)
                    return source_fail(ctx, node, XR_XIR_BUDGET, "method function budget exhausted");
                functions += (uint32_t)decl->method_count;
                for (int f = 0; f < decl->field_count; ++f) {
                    if (!source_work(ctx, decl->fields[f])) return false;
                    if (decl->fields[f]->type == AST_FIELD_DECL && decl->fields[f]->as.field_decl.initializer) {
                        if (functions >= ctx->budget.functions)
                            return source_fail(ctx, node, XR_XIR_BUDGET, "field initializer function budget exhausted");
                        ++functions;
                    }
                }
            }
            if (node->type == AST_VAR_DECL || node->type == AST_CONST_DECL) ++slots;
        }
    }
    if (functions > ctx->budget.functions || closures.defaults > ctx->budget.functions - functions)
        return source_fail(ctx, NULL, XR_XIR_BUDGET, "default argument function budget exhausted");
    functions += closures.defaults;
    if (functions > ctx->budget.functions || closures.count > ctx->budget.functions - functions)
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"function budget exhausted");
    ctx->first_closure = ctx->next_closure = functions - 1;
    functions += closures.count;
    ctx->function_count = functions; ctx->slot_count = slots;
    if (nominals > UINT32_MAX - functions)
        return source_fail(ctx, NULL, XR_XIR_BUDGET, "default constructor capacity exhausted");
    uint32_t capacity = functions + nominals;
    ctx->functions = source_alloc(ctx, capacity, sizeof(*ctx->functions));
    ctx->bodies = source_alloc(ctx, capacity, sizeof(*ctx->bodies));
    ctx->identities = source_alloc(ctx, capacity, sizeof(*ctx->identities));
    ctx->generics = source_alloc(ctx, capacity, sizeof(*ctx->generics));
    ctx->modules = source_alloc(ctx, count, sizeof(*ctx->modules));
    ctx->names = source_alloc(ctx, count, sizeof(*ctx->names));
    ctx->slots = source_alloc(ctx, slots, sizeof(*ctx->slots));
    ctx->nominals.declarations = source_alloc(ctx, nominals, sizeof(*ctx->nominals.declarations));
    ctx->nominal_members = source_alloc(ctx, nominals, sizeof(*ctx->nominal_members));
    ctx->nominal_variants = source_alloc(ctx, nominals, sizeof(*ctx->nominal_variants));
    ctx->nominal_defaults = source_alloc(ctx, nominals, sizeof(*ctx->nominal_defaults));
    ctx->nominal_sources = source_alloc(ctx, nominals, sizeof(*ctx->nominal_sources));
    ctx->nominal_methods = source_alloc(ctx, nominals, sizeof(*ctx->nominal_methods));
    ctx->nominal_defaultable = source_alloc(ctx, nominals, sizeof(*ctx->nominal_defaultable));
    ctx->nominal_constructors = source_alloc(ctx, nominals, sizeof(*ctx->nominal_constructors));
    if (nominals) ctx->types.nominals = &ctx->nominals;
    if (ctx->diagnostic.status != XR_XIR_OK) return false;
    for (uint32_t m = 0; m < count; ++m) {
        ctx->module = m;
        AstNode *ast = ctx->graph->specs[m].ast;
        for (int i = 0; i < ast->as.program.count; ++i) {
            AstNode *node = ast->as.program.statements[i];
            if (node->type == AST_STRUCT_DECL && !source_struct_declare(ctx, node)) return false;
            if (node->type == AST_ENUM_DECL && !source_enum_declare(ctx, node)) return false;
            if (node->type == AST_IMPORT_STMT && !declare_import(ctx, node)) return false;
        }
    }
    if (!source_struct_fields(ctx) || !source_enum_fields(ctx)) return false;
    if (!source_struct_defaultability(ctx)) return false;
    uint32_t function = count, slot = 0;
    for (uint32_t m = 0; m < count; ++m) {
        ctx->module = m;
        XrModuleSpec *spec = &ctx->graph->specs[m];
        uint32_t *deps = source_alloc(ctx, (size_t) spec->dep_count, sizeof(*deps));
        if (!deps) return false;
        for (int i = 0; i < spec->dep_count; ++i) deps[i] = (uint32_t) spec->dep_indices[i];
        ctx->modules[m] = (XrXirSourceModule) {spec->canonical, (uint32_t) strlen(spec->canonical), deps, (uint32_t) spec->dep_count, m};
        ctx->bodies[m].module = m;
        ctx->functions[m] = (XrXirFunction) {"$init", 5, NULL, 0, XR_XIR_UNIT, NULL, 0, NULL, 0, NULL, 0};
        ctx->identities[m].module = m;
        for (int i = 0; i < spec->ast->as.program.count; ++i) {
            AstNode *node = spec->ast->as.program.statements[i];
            if (node->type == AST_FUNCTION_DECL) { if (!declare_function(ctx, node, function++)) return false; }
            else if (node->type == AST_VAR_DECL || node->type == AST_CONST_DECL) {
                if (node->is_exported || (!node->as.var_decl.is_const && m != (uint32_t) ctx->graph->entry_index))
                    return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "library mutable or exported state is not admitted");
                SourceName *symbol = add_name(ctx, &ctx->names[m], node->as.var_decl.name, node);
                if (!symbol) return false;
                symbol->kind = SOURCE_SLOT; symbol->index = slot; symbol->mutable = !node->as.var_decl.is_const;
                if (!source_query_declare(ctx, symbol, XR_XIR_SOURCE_BINDING, 0, source_query_range(ctx, node, symbol->name))) return false;
                ctx->slots[slot++] = (XrXirSlot) {m, XR_XIR_UNIT, symbol->mutable};
            }
        }
    }
    if (!source_struct_methods(ctx, &function) || !source_struct_default_functions(ctx, &function) || !source_struct_constructors(ctx, &function) || !source_argument_functions(ctx, &function) || function != ctx->first_closure)
        return source_fail(ctx, NULL, XR_XIR_BAD_STRUCTURE, "field initializer function inventory mismatch");
    for (uint32_t m = 0; m < count; ++m) {
        ctx->module = m;
        for (SourceName *p = ctx->names[m]; p; p = p->next)
            if (p->kind == SOURCE_IMPORT && !imported_declaration(ctx, p, p->imported)) return false;
    }
    return true;
}
static bool source_parameter_promise(SourceContext *ctx, uint32_t function, uint32_t parameter) {
    SourceFunction *body = &ctx->bodies[function];
    AstNode *node = body->node;
    if (parameter >= ctx->functions[function].parameter_count)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "callable promise parameter is missing");
    const XrXirTypeNode *found = xr_xir_callable_signature(&ctx->types, body->parameters[parameter]);
    if (!found || found->flags)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "callable promise target is invalid or duplicated");
    XrXirTypeNode qualified = *found;
    qualified.flags = XR_XIR_CALLABLE_NO_SUSPEND;
    if (!source_intern_type(ctx, qualified, &body->parameters[parameter])) return false;
    if (body->argument_defaults && body->argument_defaults[parameter])
        ctx->functions[body->argument_defaults[parameter]].result = body->parameters[parameter];
    ctx->function = function; ctx->module = body->module;
    return source_query_parameters(ctx, body->declaration);
}
static bool source_promises(SourceContext *ctx, const XrXirSourcePromises *declarations) {
    if (!declarations) return true;
    if (declarations->count && !declarations->items)
        return source_fail(ctx, NULL, XR_XIR_BAD_STRUCTURE, "source declaration table is missing");
    for (uint32_t d = 0; d < declarations->count; ++d) {
        const XrXirSourcePromise *item = &declarations->items[d];
        if (!source_work(ctx, NULL)) return false;
        if (!item->module.bytes || !item->module.length || !item->function.bytes ||
            !item->function.length || (item->owner.length && !item->owner.bytes) ||
            item->promises != XR_XIR_FUNCTION_NO_SUSPEND)
            return source_fail(ctx, NULL, XR_XIR_BAD_STRUCTURE, "unsupported source declaration promise");
        uint32_t selected = UINT32_MAX;
        for (uint32_t f = 0; f < ctx->function_count; ++f) {
            if (!source_work(ctx, NULL)) return false;
            AstNode *node = ctx->bodies[f].node;
            if (!node || ctx->bodies[f].default_expression) continue;
            uint32_t owner = ctx->identities[f].nominal_owner;
            if (item->owner.length) {
                if (node->type != AST_METHOD_DECL || node->as.method_decl.is_constructor || !owner) continue;
                const XrXirLiteral *name = &ctx->nominals.declarations[owner - 1].name;
                if (name->length != item->owner.length || memcmp(name->bytes, item->owner.bytes, name->length)) continue;
            } else if (node->type != AST_FUNCTION_DECL || owner) continue;
            const XrXirFunction *function = &ctx->functions[f];
            uint32_t module = ctx->identities[f].module;
            if (ctx->modules[module].name_length != item->module.length ||
                memcmp(ctx->modules[module].name, item->module.bytes, item->module.length) ||
                item->function.length != function->name_length ||
                memcmp(function->name, item->function.bytes, function->name_length)) continue;
            if (selected != UINT32_MAX)
                return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "ambiguous source declaration promise");
            selected = f;
        }
        if (selected == UINT32_MAX)
            return source_fail(ctx, NULL, XR_XIR_BAD_STRUCTURE, "source declaration promise target is missing");
        if (item->parameter) {
            AstNode *node = ctx->bodies[selected].node;
            uint32_t offset = node->type == AST_METHOD_DECL && !node->as.method_decl.is_static ? 1 : 0;
            if (!source_parameter_promise(ctx, selected, item->parameter - 1 + offset)) return false;
            continue;
        }
        if (ctx->identities[selected].promises)
            return source_fail(ctx, ctx->bodies[selected].node, XR_XIR_BAD_STRUCTURE, "duplicate source declaration promise");
        ctx->identities[selected].promises = item->promises;
    }
    return true;
}
static bool finish_body(SourceContext *ctx) {
    XrXirFunction *function = &ctx->functions[ctx->function];
    SourceFunction *body = &ctx->bodies[ctx->function];
    if (!ctx->returned) {
        if (function->result != XR_XIR_UNIT) return source_fail(ctx, body->node, XR_XIR_BAD_TYPE, "value function requires a return");
        if (!emit(ctx, (XrXirInstruction) {XR_XIR_RETURN, XR_XIR_UNIT, {0, 0}, {0, 0}, 0, {0}}, NULL)) return false;
    }
    function->blocks = body->blocks; function->block_count = body->block_count;
    function->instructions = body->ops; function->instruction_count = body->count;
    function->operands = body->operands; function->operand_count = body->operand_count;
    return true;
}
typedef struct SourceCapture {
    struct SourceCapture *next;
    SourceName *source;
    uint32_t index;
} SourceCapture;
typedef struct SourceCaptureScan {
    SourceContext *ctx;
    SourceName *bound;
    SourceCapture *captures;
    uint32_t count, depth;
} SourceCaptureScan;
static bool capture_bind(SourceCaptureScan *scan, const char *name, AstNode *node) {
    SourceName *binding = source_alloc(scan->ctx, 1, sizeof(*binding));
    if (!binding) return false;
    binding->name = name; binding->node = node; binding->next = scan->bound; scan->bound = binding;
    return true;
}
static bool capture_name(SourceCaptureScan *scan, const char *name, AstNode *node) {
    SourceContext *ctx = scan->ctx;
    if (find_name(ctx, scan->bound, name)) return true;
    SourceName *source = visible_name(ctx, name);
    if (!source || source->kind != SOURCE_LOCAL) return ctx->diagnostic.status == XR_XIR_OK;
    for (SourceCapture *p = scan->captures; p; p = p->next) {
        if (!source_work(ctx, node)) return false;
        if (p->source == source) return true;
    }
    SourceCapture *capture = source_alloc(ctx, 1, sizeof(*capture));
    if (!capture) return false;
    *capture = (SourceCapture) {scan->captures, source, scan->count++}; scan->captures = capture;
    return true;
}
static bool capture_scan(AstNode *node, void *pointer);
static bool capture_scope(SourceCaptureScan *scan, AstNode *node) {
    SourceName *saved = scan->bound;
    bool ok = capture_scan(node, scan); scan->bound = saved; return ok;
}
static bool capture_pattern(SourceCaptureScan *scan, AstNode *node, uint32_t depth) {
    if (depth>=128) return source_fail(scan->ctx,node,XR_XIR_BUDGET,"pattern capture depth exhausted");
    if (!node || !source_work(scan->ctx,node)) return false;
    if (node->type==AST_PATTERN_MULTI) {
        if (node->as.pattern_multi.count<=0) return source_fail(scan->ctx,node,XR_XIR_BAD_STRUCTURE,"empty multi-pattern");
        return capture_pattern(scan,node->as.pattern_multi.patterns[0],depth+1);
    }
    if (source_pattern_binding(node)) return capture_bind(scan,node->as.pattern_literal.value->as.variable.name,node);
    if (node->type==AST_PATTERN_ADT) {
        for (int i=0;i<node->as.pattern_adt.count;++i)
            if (!capture_pattern(scan,node->as.pattern_adt.patterns[i],depth+1)) return false;
    }
    return true;
}
static bool capture_children(SourceCaptureScan *scan, AstNode *node) {
    switch (node->type) {
    case AST_MATCH_EXPR: {
        if (!capture_scan(node->as.match_expr.expr,scan)) return false;
        SourceName *saved=scan->bound;
        for (int i=0;i<node->as.match_expr.arm_count;++i) {
            MatchArmNode *arm=&node->as.match_expr.arms[i]->as.match_arm;
            scan->bound=saved;
            if (!capture_pattern(scan,arm->pattern,0) || !capture_scan(arm->guard,scan) || !capture_scan(arm->body,scan)) return false;
        }
        scan->bound=saved; return true;
    }
    case AST_TRY_CATCH: {
        TryCatchNode *attempt = &node->as.try_catch;
        if (!capture_scope(scan,attempt->try_body)) return false;
        for (int i = 0; i < attempt->catch_count; ++i) {
            XrCatchClause *clause = attempt->catch_clauses[i]; SourceName *saved = scan->bound;
            if (clause->var_name && !capture_bind(scan,clause->var_name,node)) return false;
            if (!capture_scope(scan,clause->body)) return false;
            scan->bound = saved;
        }
        return true;
    }
    case AST_THIS_EXPR: return capture_name(scan, "this", node);
    case AST_VARIABLE: return capture_name(scan, node->as.variable.name, node);
    case AST_INC: return capture_name(scan, node->as.inc.name, node);
    case AST_DEC: return capture_name(scan, node->as.dec.name, node);
    case AST_ASSIGNMENT:
        if (!capture_name(scan, node->as.assignment.name, node)) return false;
        break;
    case AST_COMPOUND_ASSIGNMENT:
        if (!node->as.compound_assignment.object && node->as.compound_assignment.name &&
            !capture_name(scan, node->as.compound_assignment.name, node)) return false;
        break;
    case AST_VAR_DECL: case AST_CONST_DECL:
        return capture_scan(node->as.var_decl.initializer, scan) && capture_bind(scan, node->as.var_decl.name, node);
    case AST_BLOCK: {
        SourceName *saved = scan->bound;
        for (int i = 0; i < node->as.block.count; ++i)
            if (!capture_scan(node->as.block.statements[i], scan)) return false;
        scan->bound = saved; return true;
    }
    case AST_FUNCTION_EXPR: {
        SourceName *saved = scan->bound;
        for (int i = 0; i < node->as.function_expr.param_count; ++i)
            if (!capture_bind(scan, node->as.function_expr.params[i]->name, node)) return false;
        bool ok = capture_scan(node->as.function_expr.body, scan); scan->bound = saved; return ok;
    }
    case AST_IF_STMT:
        return capture_scan(node->as.if_stmt.condition, scan) && capture_scope(scan, node->as.if_stmt.then_branch) &&
            capture_scope(scan, node->as.if_stmt.else_branch);
    case AST_WHILE_STMT:
        return capture_scan(node->as.while_stmt.condition, scan) && capture_scope(scan, node->as.while_stmt.body);
    case AST_FOR_STMT: {
        SourceName *saved = scan->bound;
        ForStmtNode *loop = &node->as.for_stmt;
        bool ok = capture_scan(loop->initializer, scan) && capture_scan(loop->condition, scan) &&
            capture_scope(scan, loop->body) && capture_scope(scan, loop->increment);
        scan->bound = saved; return ok;
    }
    default: break;
    }
    return xr_ast_for_each_child(node, capture_scan, scan) ||
        source_fail(scan->ctx, node, XR_XIR_BAD_STRUCTURE, "unknown closure syntax");
}
static bool capture_scan(AstNode *node, void *pointer) {
    SourceCaptureScan *scan = pointer;
    if (!node) return true;
    if (!source_work(scan->ctx, node)) return false;
    if (scan->depth == 128) return source_fail(scan->ctx, node, XR_XIR_BUDGET, "capture scan depth exhausted");
    ++scan->depth;
    bool ok = capture_children(scan, node); --scan->depth; return ok;
}
static bool source_capture_parameters(SourceContext *ctx, AstNode *node, const SourceCaptureScan *scan) {
    SourceFunction *body = &ctx->bodies[ctx->function];
    for (SourceCapture *p = scan->captures; p; p = p->next) {
        SourceName *symbol = add_name(ctx, &ctx->locals, p->source->name, node);
        if (!symbol) return false;
        symbol->kind = SOURCE_LOCAL; symbol->index = p->index; symbol->type = p->source->type;
        symbol->mutable = p->source->mutable;
        symbol->declaration = p->source->declaration;
        if (p->source->construction && ctx->identities[ctx->function].cleanup_owner) {
            symbol->construction = true; body->constructor_captured = true; body->constructor_shared = true;
            uint32_t count = xr_xir_type_node(&ctx->types, symbol->type)->nominal.field_count;
            body->constructor_places = count ? source_alloc(ctx, count, sizeof(*body->constructor_places)) : NULL;
            if (count && !body->constructor_places) return false;
            for (uint32_t f = 0; f < count; ++f) {
                body->constructor_places[f] = p->index + f;
                if (!source_constructor_storage_type(ctx, symbol->type, f, &body->parameters[p->index + f])) return false;
            }
            continue;
        }
        XrXirType type = symbol->type;
        if (symbol->mutable && !source_cell_type(ctx, type, &type)) return false;
        body->parameters[p->index] = type;
    }
    return true;
}
static bool closure_parameters(SourceContext *ctx, AstNode *node, const SourceCaptureScan *scan,
    XrXirCallableParameter *parameters) {
    if (!source_capture_parameters(ctx, node, scan)) return false;
    SourceFunction *body = &ctx->bodies[ctx->function];
    FunctionDeclNode *decl = &node->as.function_expr;
    for (int i = 0; i < decl->param_count; ++i) {
        XrParamNode *param = decl->params[i];
        XrXirType type;
        if (!param->type || param->passing_mode != XR_PARAM_READ || param->default_value || param->pattern || param->is_rest ||
            !source_type(ctx, param->type, &type) || type == XR_XIR_UNIT)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "closure parameter contract is not implemented in XIR");
        SourceName *symbol = add_name(ctx, &ctx->locals, param->name, node);
        if (!symbol) return false;
        symbol->kind = SOURCE_LOCAL; symbol->index = scan->count + (uint32_t) i; symbol->type = type;
        XrXirSourceRange range = {ctx->module, param->line, param->column, param->line, 0};
        if (param->column > 0 && strlen(param->name) <= (size_t) (INT_MAX - param->column))
            range.end_column = param->column + (int) strlen(param->name);
        if (!source_query_declare(ctx, symbol, XR_XIR_SOURCE_PARAMETER, body->declaration, range)) return false;
        source_query_binding_type(ctx, symbol);
        body->parameters[symbol->index] = type;
        parameters[i] = (XrXirCallableParameter) {type, XR_PARAM_READ};
    }
    return true;
}
#include "xxir_source_cleanup.inc.c"
static bool source_closure(SourceContext *ctx, AstNode *node, XrXirType expected, SourceValue *value) {
    const XrXirTypeNode *context = xr_xir_callable_signature(&ctx->types, expected);
    bool no_suspend = context && (context->flags & XR_XIR_CALLABLE_NO_SUSPEND);
    FunctionDeclNode *decl = &node->as.function_expr;
    if (decl->is_generator || decl->is_extern || decl->attr_count || decl->type_param_count || decl->throws_count ||
        decl->borrow_origin_count || !decl->body || decl->param_count < 0 || decl->param_count > 65536)
        return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "closure contract is not implemented in XIR");
    SourceCaptureScan scan = {ctx, NULL, NULL, 0, 0};
    if (!capture_scan(node, &scan)) return false;
    if (scan.count > 65536 - (uint32_t) decl->param_count || ctx->next_closure >= ctx->function_count - 1)
        return source_fail(ctx, node, XR_XIR_BUDGET, "closure parameter or declaration budget exhausted");
    uint32_t outer = ctx->function, index = ctx->next_closure++;
    SourceFunction *body = &ctx->bodies[index];
    body->node = node; body->type_owner = ctx->bodies[outer].type_owner; body->module = ctx->module;
    body->generic_owner = ctx->bodies[outer].generic_owner;
    body->type_parameters = ctx->bodies[outer].type_parameters;
    body->type_parameter_count = ctx->bodies[outer].type_parameter_count;
    body->infer_result = !decl->return_type;
    ctx->generics[index].parameter_count = ctx->generics[outer].parameter_count;
    ctx->generics[index].constraints = ctx->generics[outer].constraints;
    char *name = source_alloc(ctx, 32, sizeof(*name));
    body->parameters = source_alloc(ctx, scan.count + (uint32_t) decl->param_count, sizeof(*body->parameters));
    XrXirCallableParameter *parameters = decl->param_count ? source_alloc(ctx, (uint32_t) decl->param_count, sizeof(*parameters)) : NULL;
    SourceValue *captures = source_alloc(ctx, scan.count, sizeof(*captures));
    if (!name || !body->parameters || (decl->param_count && !parameters) || !captures) return false;
    snprintf(name, 32, "$closure%u", index);
    SourceName declaration = {0}; declaration.name = name; declaration.node = node;
    if (!source_query_declare(ctx, &declaration, XR_XIR_SOURCE_FUNCTION, ctx->bodies[outer].declaration,
        source_query_range(ctx, node, NULL))) return false;
    body->declaration = declaration.declaration;
    ctx->functions[index] = (XrXirFunction) {name, (uint32_t) strlen(name), body->parameters,
        scan.count + (uint32_t) decl->param_count, XR_XIR_UNIT, NULL, 0, NULL, 0, NULL, 0};
    ctx->identities[index].module = ctx->module;
    ctx->identities[index].nominal_owner = ctx->identities[outer].nominal_owner;
    ctx->identities[index].promises = no_suspend ? XR_XIR_FUNCTION_NO_SUSPEND : 0;
    SourceName *locals = ctx->locals, *scope = ctx->scope;
    SourceLoop *loop = ctx->loop; bool returned = ctx->returned;
    ctx->function = index; ctx->locals = ctx->scope = NULL; ctx->loop = NULL; ctx->returned = false;
    XrXirType type = XR_XIR_UNIT;
    bool ok = closure_parameters(ctx, node, &scan, parameters) &&
        source_type(ctx, decl->return_type, &ctx->functions[index].result) &&
        statement(ctx, decl->body, false) && finish_body(ctx) &&
        source_signature(ctx, parameters, (uint32_t) decl->param_count, ctx->functions[index].result, &type);
    if (ok && no_suspend) {
        XrXirTypeNode signature = *xr_xir_callable_signature(&ctx->types, type);
        signature.flags = XR_XIR_CALLABLE_NO_SUSPEND;
        ok = source_intern_type(ctx, signature, &type);
    }
    if (ok) {
        XrXirSourceType *query_parameters = decl->param_count ? source_alloc(ctx, (size_t) decl->param_count, sizeof(*query_parameters)) : NULL;
        ok = !decl->param_count || query_parameters;
        for (int i = 0; ok && i < decl->param_count; ++i) {
            ok = source_work(ctx, node);
            query_parameters[i] = source_query_type(ctx, parameters[i].type);
        }
        XrXirSourceDeclaration *query_declaration = (XrXirSourceDeclaration *) &ctx->query.declarations[body->declaration - 1];
        query_declaration->type = source_query_type(ctx, ctx->functions[index].result);
        query_declaration->parameters = query_parameters; query_declaration->parameter_count = (uint32_t) decl->param_count;
        query_declaration->generic_parent = body->generic_owner;
        query_declaration->generic_parent_count = body->type_parameter_count;
        query_declaration->generic_parameter_count = body->type_parameter_count;
        query_declaration->generic_constraints = ctx->generics[index].constraints;
    }
    ctx->function = outer; ctx->locals = locals; ctx->scope = scope; ctx->loop = loop; ctx->returned = returned;
    if (!ok) return false;
    for (SourceCapture *p = scan.captures; p; p = p->next) {
        XrXirType capture_type = p->source->type;
        if (p->source->mutable && !source_cell_type(ctx, capture_type, &capture_type)) return false;
        if (p->source->construction) {
            if (!source_constructor_value(ctx, node, &captures[p->index])) return false;
        } else captures[p->index] = (SourceValue) {p->source->index, capture_type};
    }
    XrXirInstruction op = {XR_XIR_FUNCTION_REF, type, {0}, {0}, index, {0}};
    uint32_t count = ctx->generics[index].parameter_count;
    XrXirType *types = count ? source_alloc(ctx, count, sizeof(*types)) : NULL;
    if (count && !types) return false;
    for (uint32_t i = 0; i < count; ++i) types[i] = (XrXirType) (XR_XIR_TYPE_PARAMETER_BASE + i);
    return source_type_arguments(ctx, node, types, count, &op) && emit_group(ctx, op, captures, scan.count, value);
}
static bool build_bodies(SourceContext *ctx) {
    uint32_t count = (uint32_t) ctx->graph->spec_count;
    for (int t = 0; t < ctx->graph->topo_count; ++t) {
        ctx->module = (uint32_t) ctx->graph->topo_order[t]; ctx->function = ctx->module;
        ctx->locals = ctx->scope = NULL; ctx->returned = false;
        AstNode *ast = ctx->graph->specs[ctx->module].ast;
        for (int i = 0; i < ast->as.program.count; ++i) if (!statement(ctx, ast->as.program.statements[i], true)) return false;
        if (!finish_body(ctx)) return false;
    }
    for (uint32_t f = count; f < ctx->first_closure; ++f) {
        ctx->function = f; ctx->module = ctx->bodies[f].module; ctx->returned = false;
        ctx->locals = ctx->scope = NULL;
        if (ctx->bodies[f].node->type == AST_STRUCT_DECL) {
            if (!source_struct_constructor_body(ctx) || !finish_body(ctx)) return false;
            continue;
        }
        if (ctx->bodies[f].default_expression || ctx->bodies[f].node->type == AST_FIELD_DECL) {
            SourceValue value;
            if (!expression_in(ctx, ctx->bodies[f].default_expression ? ctx->bodies[f].default_expression :
                ctx->bodies[f].node->as.field_decl.initializer, ctx->functions[f].result, &value) ||
                !emit(ctx, (XrXirInstruction) {XR_XIR_RETURN, XR_XIR_UNIT, {value.id, 0}, {0}, 0, {0}}, NULL)) return false;
            ctx->returned = true;
            if (!finish_body(ctx)) return false;
            continue;
        }
        if (ctx->bodies[f].node->type == AST_METHOD_DECL) {
            if (!(ctx->bodies[f].node->as.method_decl.is_constructor ? source_constructor_body(ctx) : source_method_body(ctx)) || !finish_body(ctx)) return false;
            continue;
        }
        FunctionDeclNode *decl = &ctx->bodies[f].node->as.function_decl;
        for (int i = 0; i < decl->param_count; ++i) {
            SourceName *symbol = add_name(ctx, &ctx->locals, decl->params[i]->name, ctx->bodies[f].node);
            if (!symbol) return false;
            symbol->kind = SOURCE_LOCAL; symbol->index = (uint32_t) i; symbol->type = ctx->bodies[f].parameters[i];
            XrParamNode *param = decl->params[i];
            XrXirSourceRange range = {ctx->module, param->line, param->column, param->line, 0};
            if (param->column > 0 && strlen(param->name) <= (size_t) (INT_MAX - param->column))
                range.end_column = param->column + (int) strlen(param->name);
            if (!source_query_declare(ctx, symbol, XR_XIR_SOURCE_PARAMETER, ctx->bodies[f].declaration, range)) return false;
            source_query_binding_type(ctx, symbol);
        }
        if (!statement(ctx, decl->body, false) || !finish_body(ctx)) return false;
    }
    if (ctx->next_closure != ctx->function_count - 1)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"closure declaration was not checked");
    ctx->function = ctx->function_count - 1; ctx->module = (uint32_t) ctx->graph->entry_index;
    ctx->bodies[ctx->function].module = ctx->module;
    ctx->identities[ctx->function].module = ctx->module;
    ctx->functions[ctx->function] = (XrXirFunction) {"$entry", 6, NULL, 0, XR_XIR_I64,
        NULL, 0, NULL, 0, NULL, 0};
    if (!emit(ctx, (XrXirInstruction) {XR_XIR_CONST_INT, XR_XIR_I64, {0, 0}, {0, 0}, 0, {0}}, NULL) ||
        !emit(ctx, (XrXirInstruction) {XR_XIR_RETURN, XR_XIR_UNIT, {0, 0}, {0, 0}, 0, {0}}, NULL)) return false;
    ctx->returned = true;
    return finish_body(ctx);
}
static bool source_query_modules(SourceContext *ctx) {
    uint32_t count = (uint32_t) ctx->graph->spec_count;
    XrXirSourceQueryModule *modules = source_alloc(ctx, count, sizeof(*modules));
    if (count && !modules) return false;
    ctx->query.modules = modules; ctx->query.module_count = count;
    ctx->query_module_capacity = count;
    for (uint32_t i = 0; i < count; ++i) {
        if (!source_work(ctx, NULL)) return false;
        const XrModuleSpec *module = &ctx->graph->specs[i];
        modules[i] = (XrXirSourceQueryModule) {module->canonical, module->source_path, module->source_content_fingerprint};
    }
    return true;
}
static void source_query_publish(SourceContext *ctx, XrXirSourceResult *output) {
    if (!output || !ctx->query_ready || ctx->diagnostic.status == XR_XIR_OUT_OF_MEMORY || ctx->diagnostic.status == XR_XIR_BUDGET) {
        if (output) xr_xir_source_result_free(output);
        return;
    }
    ctx->query.complete = ctx->diagnostic.status == XR_XIR_OK;
    ctx->query.diagnostic = ctx->diagnostic;
    ctx->query.types = ctx->types.count || ctx->types.nominals ? &ctx->types : NULL;
    XrXirBudget remaining = ctx->budget;
    remaining.metadata_bytes -= ctx->allocated;
    XrXirStatus status = xr_xir_source_snapshot_copy(&ctx->query, &remaining, &output->snapshot);
    if (status != XR_XIR_OK) {
        xr_xir_source_result_free(output); memset(&ctx->diagnostic, 0, sizeof(ctx->diagnostic));
        source_fail(ctx, NULL, status, "source query snapshot publication failed");
    }
}
XrXirStatus xr_xir_source_check(const XrXirSourceRequest *request,
    XrXirSourceResult *output, XrXirSourceDiagnostic *diagnostic) {
    if (output) memset(output, 0, sizeof(*output));
    SourceContext ctx = {0};
    ctx.budget = request && request->budget ? *request->budget : xr_xir_default_budget();
    XrXirBudget checking = ctx.budget;
    XrModuleResolver *resolver = NULL;
    char *error = NULL;
    if (!request || !request->session || !request->entry_path || !request->authority || !output) {
        source_fail(&ctx, NULL, XR_XIR_BAD_STRUCTURE, "source request is incomplete"); goto done;
    }
    XrModuleResolverConfig config = {request->stdlib_path, NULL};
    resolver = xr_module_resolver_new(&config);
    ctx.graph = resolver ? xr_module_graph_new(request->session, resolver) : NULL;
    if (!ctx.graph) { source_fail(&ctx, NULL, XR_XIR_OUT_OF_MEMORY, "module graph allocation failed"); goto done; }
    if (xr_module_graph_build(ctx.graph, request->entry_path, request->authority, &error) != 0 ||
        xr_module_graph_topological_sort(ctx.graph) != 0 || ctx.graph->has_cycle || ctx.graph->entry_index < 0) {
        source_fail(&ctx, NULL, XR_XIR_BAD_STRUCTURE, error ? error : "module graph is not an acyclic source closure"); goto done;
    }
    ctx.query_ready = true;
    if (source_query_modules(&ctx) && collect_declarations(&ctx) &&
        source_promises(&ctx, request->declarations) && build_bodies(&ctx)) {
        XrXirDeclarations declarations = {ctx.modules, (uint32_t) ctx.graph->spec_count, ctx.identities,
            ctx.slots, ctx.slot_count, ctx.literals, ctx.literal_count, (uint32_t) ctx.graph->entry_index, ctx.function_count - 1};
        XrXirModule built = {XR_XIR_BUILT, ctx.functions, ctx.function_count, &declarations, ctx.has_generics ? ctx.generics : NULL, ctx.types.count || ctx.types.nominals ? &ctx.types : NULL, NULL};
        XrXirDiagnostic location = {0};
        XrXirStatus status = xr_xir_check(&built, &checking, &output->checked, &location);
        for (uint32_t f=0;status==XR_XIR_OK && f<ctx.function_count;++f) {
            if (!ctx.bodies[f].initialization_regions) continue;
            location.function=f;
            status=xr_xir_initialization_check(&built,f,
                ctx.bodies[f].initialization_regions,&checking,&location);
        }
        if (status != XR_XIR_OK) {
            xr_xir_artifact_free(output->checked); output->checked=NULL;
            char message[128];
            snprintf(message, sizeof(message), "constructed XIR failed checking at function %u block %u instruction %u",
                location.function, location.block, location.instruction);
            AstNode *site = NULL;
            if (location.function < ctx.function_count) {
                site = ctx.bodies[location.function].node;
                ctx.module = ctx.bodies[location.function].module;
            }
            const char *cause = location.reason == XR_XIR_DIAGNOSTIC_NO_SUSPEND ?
                "declared no_suspend function may suspend or call an unqualified callable" :
                location.reason == XR_XIR_DIAGNOSTIC_CLEANUP_THROW ?
                "E0387: an error can escape the defer body" :
                location.reason == XR_XIR_DIAGNOSTIC_CLEANUP_SUSPEND ?
                "E0392: defer may suspend or create a task" :
                location.reason == XR_XIR_DIAGNOSTIC_UNINITIALIZED_READ ?
                "read requires storage initialized on every incoming path" :
                location.reason == XR_XIR_DIAGNOSTIC_READONLY_WRITE ?
                "write may overwrite already initialized const storage" : message;
            source_fail(&ctx, site, status, cause);
        }
    }
done:
    source_query_publish(&ctx, output);
    xr_free(error);
    while (ctx.memory) { SourceMemory *next = ctx.memory->next; xr_free(ctx.memory); ctx.memory = next; }
    xr_module_graph_free(ctx.graph); xr_module_resolver_free(resolver);
    if (diagnostic) *diagnostic = ctx.diagnostic;
    return ctx.diagnostic.status;
}
