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
#include "../module/xmodule_graph.h"
#include "../frontend/parser/xast.h"
#include "../frontend/parser/xtype_ref.h"
#include "../base/xmalloc.h"
#include <stdio.h>

typedef struct SourceMemory { struct SourceMemory *next; } SourceMemory;
typedef enum SourceKind { SOURCE_SLOT, SOURCE_FUNCTION, SOURCE_MODULE, SOURCE_IMPORT, SOURCE_LOCAL } SourceKind;
typedef struct SourceName {
    struct SourceName *next;
    const char *name, *imported;
    AstNode *node;
    SourceKind kind;
    uint32_t index, module;
    XrXirType type;
    bool mutable;
} SourceName;
typedef struct SourceFunction {
    AstNode *node;
    uint32_t module, count, capacity;
    XrXirType *parameters;
    uint32_t *operands, operand_count, operand_capacity;
    XrXirBlock block;
    XrXirInstruction *ops;
} SourceFunction;
typedef struct SourceValue { uint32_t id; XrXirType type; } SourceValue;
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
    XrXirSlot *slots;
    XrXirLiteral *literals;
    SourceName **names, *locals, *scope;
    uint32_t function_count, slot_count, literal_count, literal_capacity;
    uint32_t function, module, depth;
    bool returned;
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
static bool source_type(SourceContext *ctx, XrTypeRef *ref, XrXirType *type) {
    if (!ref) { *type = XR_XIR_UNIT; return true; }
    switch (ref->kind) {
    case XR_TREF_UNIT: *type = XR_XIR_UNIT; return true;
    case XR_TREF_BOOL: *type = XR_XIR_BOOL; return true;
    case XR_TREF_STRING: *type = XR_XIR_STRING; return true;
    case XR_TREF_SCALAR:
        if (ref->scalar_rep == XR_NATIVE_I64) { *type = XR_XIR_I64; return true; }
        break;
    case XR_TREF_GENERIC:
        if (ref->name && !strcmp(ref->name, "Atomic") && ref->nchildren == 1 &&
            ref->children[0]->kind == XR_TREF_SCALAR && ref->children[0]->scalar_rep == XR_NATIVE_I64) {
            *type = XR_XIR_ATOMIC_I64; return true;
        }
        break;
    default: break;
    }
    return source_fail(ctx, NULL, XR_XIR_BAD_TYPE, "type declaration is not admitted by XIR");
}
static bool emit(SourceContext *ctx, XrXirInstruction op, SourceValue *result) {
    SourceFunction *body = &ctx->bodies[ctx->function];
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
    body->ops[body->count++] = op; --ctx->budget.instructions;
    return true;
}
static bool expression(SourceContext *ctx, AstNode *node, SourceValue *value);
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
static SourceName *imported_function(SourceContext *ctx, SourceName *symbol, const char *name) {
    SourceName *target = find_name(ctx, ctx->names[symbol->module], name);
    if (!target || target->kind != SOURCE_FUNCTION || !target->node->is_exported) {
        source_fail(ctx, symbol->node, XR_XIR_BAD_STRUCTURE, "import requires an exported function"); return NULL;
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
static bool source_call(SourceContext *ctx, AstNode *node, SourceValue *value) {
    CallExprNode *call = &node->as.call_expr;
    if (call->arg_count < 0 || call->arg_count > 65536 || call->type_arg_count || call->default_arg_count)
        return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "call arity or type arguments are not admitted");
    SourceName *target = NULL;
    bool print = false, atomic = false;
    uint32_t stream = 0;
    AstNode *callee = call->callee;
    SourceValue receiver = {0};
    XrXirOp method = XR_XIR_INVALID;
    if (callee->type == AST_VARIABLE) {
        const char *name = callee->as.variable.name;
        target = visible_name(ctx, name);
        if (!target) { print = !strcmp(name, "print"); atomic = !strcmp(name, "Atomic"); stream = stream_primitive(ctx, name); }
        if (target && target->kind == SOURCE_IMPORT) target = imported_function(ctx, target, target->imported);
    } else if (callee->type == AST_MEMBER_ACCESS) {
        MemberAccessNode *member = &callee->as.member_access;
        SourceName *base = member->object->type == AST_VARIABLE ?
            visible_name(ctx, member->object->as.variable.name) : NULL;
        if (base && base->kind == SOURCE_MODULE) target = imported_function(ctx, base, member->name);
        else {
            if (!expression(ctx, member->object, &receiver)) return false;
            if (receiver.type == XR_XIR_ATOMIC_I64) {
                if (!strcmp(member->name, "load") && !call->arg_count) method = XR_XIR_ATOMIC_I64_LOAD;
                if (!strcmp(member->name, "fetchAdd") && call->arg_count == 1) method = XR_XIR_ATOMIC_I64_FETCH_ADD;
            }
        }
    }
    if (ctx->diagnostic.status != XR_XIR_OK) return false;
    if (!print && !atomic && !stream && method == XR_XIR_INVALID && (!target || target->kind != SOURCE_FUNCTION))
        return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "unresolved or unsupported callable");
    SourceValue *args = call->arg_count ? source_alloc(ctx, (size_t) call->arg_count, sizeof(*args)) : NULL;
    if (call->arg_count && !args) return false;
    for (int i = 0; i < call->arg_count; ++i) {
        if (call->arg_accesses && call->arg_accesses[i] != XR_CALL_ARG_PLAIN)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "ref and move arguments require an implemented contract");
        if (!expression(ctx, call->arguments[i], &args[i])) return false;
        if (args[i].type == XR_XIR_UNIT) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "unit argument is not admitted");
    }
    XrXirInstruction op = {0};
    if (stream) {
        if (call->arg_count != 1 || args[0].type != XR_XIR_STRING)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "stream write requires one string");
        op = (XrXirInstruction) {XR_XIR_WRITE_STREAM, XR_XIR_BOOL, {args[0].id, 0}, {0, 0}, stream};
    } else if (print) {
        for (int i = 0; i < call->arg_count; ++i)
            if (args[i].type != XR_XIR_BOOL && args[i].type != XR_XIR_I64 && args[i].type != XR_XIR_STRING)
                return source_fail(ctx, node, XR_XIR_BAD_TYPE, "print requires an admitted display type");
        op = (XrXirInstruction) {XR_XIR_PRINT, XR_XIR_UNIT, {0, 0}, {0, 0}, 0};
    } else if (atomic) {
        if (call->arg_count != 1 || args[0].type != XR_XIR_I64)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Atomic requires one i64 initializer");
        op = (XrXirInstruction) {XR_XIR_ATOMIC_I64_NEW, XR_XIR_ATOMIC_I64, {args[0].id, 0}, {0, 0}, 0};
    } else if (method != XR_XIR_INVALID) {
        if (method == XR_XIR_ATOMIC_I64_FETCH_ADD && args[0].type != XR_XIR_I64)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Atomic fetchAdd requires i64");
        op = (XrXirInstruction) {method, XR_XIR_I64, {receiver.id, call->arg_count ? args[0].id : 0}, {0, 0}, 0};
    } else {
        XrXirFunction *function = &ctx->functions[target->index];
        if ((uint32_t) call->arg_count != function->parameter_count)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "call argument count does not match declaration");
        for (int i = 0; i < call->arg_count; ++i)
            if (args[i].type != function->parameters[i])
                return source_fail(ctx, node, XR_XIR_BAD_TYPE, "call argument type does not match declaration");
        op = (XrXirInstruction) {XR_XIR_CALL, function->result, {0, 0}, {0, 0}, target->index};
    }
    if (op.op == XR_XIR_CALL || op.op == XR_XIR_PRINT)
        return emit_group(ctx, op, args, (uint32_t) call->arg_count, value);
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
        size_t length = strlen(bytes);
        if (length > UINT32_MAX) return source_fail(ctx, node, XR_XIR_BUDGET, "string literal exceeds format limit");
        uint32_t id = ctx->literal_count++;
        ctx->literals[id] = (XrXirLiteral) {bytes, (uint32_t) length};
        return emit(ctx, (XrXirInstruction) {XR_XIR_CONST_STRING, XR_XIR_STRING, {0, 0}, {0, 0}, id}, value);
    }
    if (node->type == AST_LITERAL_INT) {
        if (node->as.literal.int_overflows_i64)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "integer literal exceeds i64");
        return emit(ctx, (XrXirInstruction) {XR_XIR_CONST_I64, XR_XIR_I64, {0, 0}, {0, 0},
            node->as.literal.raw_value.int_val}, value);
    }
    return emit(ctx, (XrXirInstruction) {XR_XIR_CONST_BOOL, XR_XIR_BOOL, {0, 0}, {0, 0},
        node->as.literal.raw_value.bool_val}, value);
}
static bool expression_body(SourceContext *ctx, AstNode *node, SourceValue *value) {
    switch (node->type) {
    case AST_LITERAL_INT: case AST_LITERAL_TRUE: case AST_LITERAL_FALSE: case AST_LITERAL_STRING:
        return source_literal(ctx, node, value);
    case AST_CALL_EXPR: return source_call(ctx, node, value);
    case AST_VARIABLE: {
        SourceName *symbol = visible_name(ctx, node->as.variable.name);
        if (!symbol || (symbol->kind != SOURCE_SLOT && symbol->kind != SOURCE_LOCAL) || !symbol->type)
            return source_fail(ctx, node, XR_XIR_BAD_VALUE, "name is not an initialized value");
        if (symbol->kind == SOURCE_LOCAL) { *value = (SourceValue) {symbol->index, symbol->type}; return true; }
        return emit(ctx, (XrXirInstruction) {XR_XIR_SLOT_LOAD, symbol->type, {0, 0}, {0, 0}, symbol->index}, value);
    }
    case AST_BINARY_ADD: {
        SourceValue left, right;
        if (!expression(ctx, node->as.binary.left, &left) || !expression(ctx, node->as.binary.right, &right)) return false;
        if (left.type != XR_XIR_STRING || right.type != XR_XIR_STRING)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "only string concatenation is admitted for source addition");
        return emit(ctx, (XrXirInstruction) {XR_XIR_CONCAT_STRING, XR_XIR_STRING, {left.id, right.id}, {0, 0}, 0}, value);
    }
    case AST_ASSIGNMENT: {
        SourceName *symbol = visible_name(ctx, node->as.assignment.name);
        SourceValue assigned;
        if (!symbol || !symbol->mutable || (symbol->kind != SOURCE_LOCAL && symbol->kind != SOURCE_SLOT))
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "assignment requires a mutable binding");
        if (!expression(ctx, node->as.assignment.value, &assigned)) return false;
        if (assigned.type != symbol->type) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "assignment type mismatch");
        if (symbol->kind == SOURCE_LOCAL) symbol->index = assigned.id;
        else if (!emit(ctx, (XrXirInstruction) {XR_XIR_SLOT_STORE, XR_XIR_UNIT, {assigned.id, 0}, {0, 0}, symbol->index}, NULL)) return false;
        *value = assigned; return true;
    }
    default: return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "expression syntax is not implemented in XIR");
    }
}
static bool expression(SourceContext *ctx, AstNode *node, SourceValue *value) {
    if (!node || !source_work(ctx, node)) return false;
    if (ctx->depth >= 128) return source_fail(ctx, node, XR_XIR_BUDGET, "source expression depth exhausted");
    ++ctx->depth;
    bool result = expression_body(ctx, node, value);
    --ctx->depth;
    return result;
}
static bool source_binding(SourceContext *ctx, AstNode *node, bool top) {
    VarDeclNode *decl = &node->as.var_decl;
    if (decl->attr_count || !decl->initializer) return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "binding needs an initializer and no attributes");
    SourceValue initial;
    if (!expression(ctx, decl->initializer, &initial)) return false;
    if (initial.type == XR_XIR_UNIT) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "binding cannot store unit");
    XrXirType annotation;
    if (decl->type_annotation && (!source_type(ctx, decl->type_annotation, &annotation) || annotation != initial.type))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "binding annotation mismatch");
    SourceName *symbol;
    if (top) {
        symbol = find_name(ctx, ctx->names[ctx->module], decl->name);
        if (!symbol || symbol->kind != SOURCE_SLOT) return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "binding declaration missing");
        symbol->type = initial.type; ctx->slots[symbol->index].type = initial.type;
        return emit(ctx, (XrXirInstruction) {XR_XIR_SLOT_INIT, XR_XIR_UNIT, {initial.id, 0}, {0, 0}, symbol->index}, NULL);
    }
    for (SourceName *p = ctx->locals; p != ctx->scope; p = p->next)
        if (!strcmp(p->name, decl->name)) return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "duplicate local name");
    symbol = source_alloc(ctx, 1, sizeof(*symbol));
    if (!symbol) return false;
    *symbol = (SourceName) {ctx->locals, decl->name, NULL, node, SOURCE_LOCAL, initial.id, ctx->module, initial.type, !decl->is_const};
    ctx->locals = symbol;
    return true;
}
static bool statement(SourceContext *ctx, AstNode *node, bool top) {
    if (!node || !source_work(ctx, node)) return false;
    if (ctx->returned) return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "unreachable statements are not admitted");
    switch (node->type) {
    case AST_IMPORT_STMT: case AST_FUNCTION_DECL:
        return top || source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "nested declarations are not admitted");
    case AST_VAR_DECL: case AST_CONST_DECL: return source_binding(ctx, node, top);
    case AST_EXPR_STMT: { SourceValue value; return expression(ctx, node->as.expr_stmt, &value); }
    case AST_RETURN_STMT: {
        if (top || node->as.return_stmt.value_count > 1)
            return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "invalid return placement or arity");
        SourceValue value = {0};
        if (node->as.return_stmt.value_count && !expression(ctx, node->as.return_stmt.values[0], &value)) return false;
        if (value.type != ctx->functions[ctx->function].result)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "return type does not match declaration");
        ctx->returned = true;
        return emit(ctx, (XrXirInstruction) {XR_XIR_RETURN, XR_XIR_UNIT, {value.id, 0}, {0, 0}, 0}, NULL);
    }
    case AST_BLOCK: {
        if (ctx->depth >= 128) return source_fail(ctx, node, XR_XIR_BUDGET, "source block depth exhausted");
        SourceName *saved = ctx->locals, *scope = ctx->scope;
        ctx->scope = saved; ++ctx->depth;
        for (int i = 0; i < node->as.block.count; ++i)
            if (!statement(ctx, node->as.block.statements[i], false)) { --ctx->depth; return false; }
        --ctx->depth; ctx->locals = saved; ctx->scope = scope;
        return true;
    }
    default: return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "statement syntax is not implemented in XIR");
    }
}
static bool declare_function(SourceContext *ctx, AstNode *node, uint32_t index) {
    FunctionDeclNode *decl = &node->as.function_decl;
    if (decl->is_generator || decl->is_extern || decl->attr_count || decl->type_param_count ||
        decl->throws_count || decl->borrow_origin_count || !decl->body || decl->param_count > 65536 || decl->param_count < 0)
        return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "function contract is not implemented in XIR");
    SourceName *symbol = add_name(ctx, &ctx->names[ctx->module], decl->name, node);
    if (!symbol) return false;
    symbol->kind = SOURCE_FUNCTION; symbol->index = index;
    SourceFunction *body = &ctx->bodies[index]; body->node = node; body->module = ctx->module;
    body->parameters = decl->param_count ? source_alloc(ctx, (size_t) decl->param_count, sizeof(*body->parameters)) : NULL;
    if (decl->param_count && !body->parameters) return false;
    XrXirFunction *function = &ctx->functions[index];
    *function = (XrXirFunction) {decl->name, (uint32_t) strlen(decl->name), body->parameters,
        (uint32_t) decl->param_count, XR_XIR_UNIT, &body->block, 1, NULL, 0, NULL, 0};
    if (!source_type(ctx, decl->return_type, &function->result)) return false;
    for (int i = 0; i < decl->param_count; ++i) {
        XrParamNode *param = decl->params[i];
        if (!param->type || param->passing_mode != XR_PARAM_READ || param->default_value || param->pattern || param->is_rest ||
            !source_type(ctx, param->type, &body->parameters[i]) || body->parameters[i] == XR_XIR_UNIT)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "parameter contract is not implemented in XIR");
        for (int j = 0; j < i; ++j) {
            if (!source_work(ctx, node)) return false;
            if (!strcmp(param->name, decl->params[j]->name))
                return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "duplicate parameter name");
        }
    }
    ctx->identities[index] = (XrXirFunctionIdentity) {ctx->module, node->is_exported};
    return true;
}
static bool declare_import(SourceContext *ctx, AstNode *node) {
    ImportStmtNode *decl = &node->as.import_stmt;
    XrModuleSpec *spec = &ctx->graph->specs[ctx->module];
    XrModuleId id = {0}; char *error = NULL;
    int resolved = xr_module_resolver_resolve(ctx->graph->resolver, decl->module_name, spec->source_path, &spec->authority, &id, &error);
    int target = resolved == 0 ? xr_module_graph_find(ctx->graph, id.canonical) : -1;
    xr_free(error); xr_module_id_cleanup(&id);
    if (target < 0) return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "import does not resolve to the parsed graph");
    for (int i = 0; i < (decl->member_count ? decl->member_count : 1); ++i) {
        const char *name = decl->member_count ? (decl->members[i].alias ? decl->members[i].alias : decl->members[i].name) : decl->alias;
        SourceName *symbol = add_name(ctx, &ctx->names[ctx->module], name, node);
        if (!symbol) return false;
        symbol->kind = decl->member_count ? SOURCE_IMPORT : SOURCE_MODULE;
        symbol->module = (uint32_t) target;
        symbol->imported = decl->member_count ? decl->members[i].name : NULL;
    }
    return true;
}
static bool collect_declarations(SourceContext *ctx) {
    uint32_t count = (uint32_t) ctx->graph->spec_count, functions = count + 1, slots = 0;
    for (uint32_t m = 0; m < count; ++m) {
        AstNode *ast = ctx->graph->specs[m].ast;
        if (!ast || ast->type != AST_PROGRAM) return source_fail(ctx, ast, XR_XIR_BAD_STRUCTURE, "parsed module required");
        for (int i = 0; i < ast->as.program.count; ++i) {
            AstNode *node = ast->as.program.statements[i];
            if (!source_work(ctx, node)) return false;
            if (node->type == AST_FUNCTION_DECL) ++functions;
            if (node->type == AST_VAR_DECL || node->type == AST_CONST_DECL) ++slots;
        }
    }
    if (functions > ctx->budget.functions) return source_fail(ctx, NULL, XR_XIR_BUDGET, "function budget exhausted");
    ctx->function_count = functions; ctx->slot_count = slots;
    ctx->functions = source_alloc(ctx, functions, sizeof(*ctx->functions));
    ctx->bodies = source_alloc(ctx, functions, sizeof(*ctx->bodies));
    ctx->identities = source_alloc(ctx, functions, sizeof(*ctx->identities));
    ctx->modules = source_alloc(ctx, count, sizeof(*ctx->modules));
    ctx->names = source_alloc(ctx, count, sizeof(*ctx->names));
    ctx->slots = source_alloc(ctx, slots, sizeof(*ctx->slots));
    if (ctx->diagnostic.status != XR_XIR_OK) return false;
    uint32_t function = count, slot = 0;
    for (uint32_t m = 0; m < count; ++m) {
        ctx->module = m;
        XrModuleSpec *spec = &ctx->graph->specs[m];
        uint32_t *deps = source_alloc(ctx, (size_t) spec->dep_count, sizeof(*deps));
        if (!deps) return false;
        for (int i = 0; i < spec->dep_count; ++i) deps[i] = (uint32_t) spec->dep_indices[i];
        ctx->modules[m] = (XrXirSourceModule) {spec->canonical, (uint32_t) strlen(spec->canonical), deps, (uint32_t) spec->dep_count, m};
        ctx->bodies[m].module = m;
        ctx->functions[m] = (XrXirFunction) {"$init", 5, NULL, 0, XR_XIR_UNIT, &ctx->bodies[m].block, 1, NULL, 0, NULL, 0};
        ctx->identities[m].module = m;
        for (int i = 0; i < spec->ast->as.program.count; ++i) {
            AstNode *node = spec->ast->as.program.statements[i];
            if (node->type == AST_FUNCTION_DECL) { if (!declare_function(ctx, node, function++)) return false; }
            else if (node->type == AST_IMPORT_STMT) { if (!declare_import(ctx, node)) return false; }
            else if (node->type == AST_VAR_DECL || node->type == AST_CONST_DECL) {
                if (node->is_exported || (!node->as.var_decl.is_const && m != (uint32_t) ctx->graph->entry_index))
                    return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "library mutable or exported state is not admitted");
                SourceName *symbol = add_name(ctx, &ctx->names[m], node->as.var_decl.name, node);
                if (!symbol) return false;
                symbol->kind = SOURCE_SLOT; symbol->index = slot; symbol->mutable = !node->as.var_decl.is_const;
                ctx->slots[slot++] = (XrXirSlot) {m, XR_XIR_UNIT, symbol->mutable};
            }
        }
    }
    for (uint32_t m = 0; m < count; ++m) for (SourceName *p = ctx->names[m]; p; p = p->next)
        if (p->kind == SOURCE_IMPORT && !imported_function(ctx, p, p->imported)) return false;
    return true;
}
static bool finish_body(SourceContext *ctx) {
    XrXirFunction *function = &ctx->functions[ctx->function];
    SourceFunction *body = &ctx->bodies[ctx->function];
    if (!ctx->returned) {
        if (function->result != XR_XIR_UNIT) return source_fail(ctx, body->node, XR_XIR_BAD_TYPE, "value function requires a return");
        if (!emit(ctx, (XrXirInstruction) {XR_XIR_RETURN, XR_XIR_UNIT, {0, 0}, {0, 0}, 0}, NULL)) return false;
    }
    body->block = (XrXirBlock) {0, body->count};
    function->instructions = body->ops; function->instruction_count = body->count;
    function->operands = body->operands; function->operand_count = body->operand_count;
    return true;
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
    for (uint32_t f = count; f + 1 < ctx->function_count; ++f) {
        ctx->function = f; ctx->module = ctx->bodies[f].module; ctx->returned = false;
        ctx->locals = ctx->scope = NULL;
        FunctionDeclNode *decl = &ctx->bodies[f].node->as.function_decl;
        for (int i = 0; i < decl->param_count; ++i) {
            SourceName *symbol = add_name(ctx, &ctx->locals, decl->params[i]->name, ctx->bodies[f].node);
            if (!symbol) return false;
            symbol->kind = SOURCE_LOCAL; symbol->index = (uint32_t) i; symbol->type = ctx->bodies[f].parameters[i];
        }
        if (!statement(ctx, decl->body, false) || !finish_body(ctx)) return false;
    }
    ctx->function = ctx->function_count - 1; ctx->module = (uint32_t) ctx->graph->entry_index;
    ctx->bodies[ctx->function].module = ctx->module;
    ctx->identities[ctx->function].module = ctx->module;
    ctx->functions[ctx->function] = (XrXirFunction) {"$entry", 6, NULL, 0, XR_XIR_I64,
        &ctx->bodies[ctx->function].block, 1, NULL, 0, NULL, 0};
    if (!emit(ctx, (XrXirInstruction) {XR_XIR_CONST_I64, XR_XIR_I64, {0, 0}, {0, 0}, 0}, NULL) ||
        !emit(ctx, (XrXirInstruction) {XR_XIR_RETURN, XR_XIR_UNIT, {0, 0}, {0, 0}, 0}, NULL)) return false;
    ctx->returned = true;
    return finish_body(ctx);
}
XrXirStatus xr_xir_source_check(const XrXirSourceRequest *request,
    XrXirArtifact **output, XrXirSourceDiagnostic *diagnostic) {
    if (output) *output = NULL;
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
    if (collect_declarations(&ctx) && build_bodies(&ctx)) {
        XrXirDeclarations declarations = {ctx.modules, (uint32_t) ctx.graph->spec_count, ctx.identities,
            ctx.slots, ctx.slot_count, ctx.literals, ctx.literal_count, (uint32_t) ctx.graph->entry_index, ctx.function_count - 1};
        XrXirModule built = {XR_XIR_BUILT, ctx.functions, ctx.function_count, &declarations};
        XrXirStatus status = xr_xir_check(&built, &checking, output, NULL);
        if (status != XR_XIR_OK) source_fail(&ctx, NULL, status, "constructed XIR failed checking");
    }
done:
    xr_free(error);
    while (ctx.memory) { SourceMemory *next = ctx.memory->next; xr_free(ctx.memory); ctx.memory = next; }
    xr_module_graph_free(ctx.graph); xr_module_resolver_free(resolver);
    if (diagnostic) *diagnostic = ctx.diagnostic;
    return ctx.diagnostic.status;
}
