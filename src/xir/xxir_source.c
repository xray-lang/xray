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
#include "xxir_generic.h"
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
    uint32_t type_capacity;
    XrXirBlock *blocks;
    uint32_t block_count, block_capacity;
    XrXirInstruction *ops;
} SourceFunction;
typedef struct SourcePatch { struct SourcePatch *next; uint32_t instruction; } SourcePatch;
typedef struct SourceLoop { struct SourceLoop *parent; SourcePatch *breaks, *continues; } SourceLoop;
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
    XrXirGeneric *generics;
    XrXirSlot *slots;
    XrXirLiteral *literals;
    SourceName **names, *locals, *scope;
    uint32_t function_count, slot_count, literal_count, literal_capacity;
    uint32_t function, module, depth;
    SourceLoop *loop;
    bool returned, has_generics;
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
    case XR_TREF_NAMED: case XR_TREF_TYPE_PARAM: {
        AstNode *node = ctx->bodies[ctx->function].node;
        if (!node || !ref->name) break;
        FunctionDeclNode *decl = &node->as.function_decl;
        for (int i = 0; i < decl->type_param_count; ++i) {
            if (!source_work(ctx, node)) return false;
            if (!strcmp(ref->name, decl->type_params[i]->name)) {
                *type = (XrXirType) (XR_XIR_TYPE_PARAMETER_BASE + (uint32_t) i); return true;
            }
        }
        break;
    }
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
    body->blocks[body->block_count++] = (XrXirBlock) {body->count, 0};
    --ctx->budget.blocks; ctx->returned = false; return true;
}
static bool emit(SourceContext *ctx, XrXirInstruction op, SourceValue *result) {
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
static bool ordinary_call(SourceContext *ctx, AstNode *node, SourceName *target,
                           const SourceValue *args, SourceValue *value) {
    CallExprNode *call = &node->as.call_expr;
    const XrXirFunction *function = &ctx->functions[target->index];
    const XrXirGeneric *callee = &ctx->generics[target->index];
    if ((uint32_t) call->arg_count != function->parameter_count ||
        (uint32_t) call->type_arg_count != callee->parameter_count)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "call requires the declared value and explicit type arguments");
    uint32_t count = (uint32_t) call->type_arg_count;
    XrXirType *types = count ? source_alloc(ctx, count, sizeof(*types)) : NULL;
    if (count && !types) return false;
    XrXirModule view = {XR_XIR_BUILT, ctx->functions, ctx->function_count, NULL, ctx->generics};
    for (uint32_t i = 0; i < count; ++i) {
        if (!source_work(ctx, node) || !source_type(ctx, call->type_args[i], &types[i])) return false;
        if (!xr_xir_type_satisfies(&view, ctx->function, types[i], callee->constraints[i]))
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "type argument does not prove the declared constraint");
    }
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
    XrXirInstruction op = {XR_XIR_CALL, XR_XIR_UNIT, {0},
        {count ? caller->argument_count : 0, count}, target->index};
    if (count) memcpy((XrXirType *) caller->arguments + caller->argument_count, types, count * sizeof(*types));
    caller->argument_count = needed;
    op.type = xr_xir_call_type(&view, ctx->function, &op, function->result);
    for (uint32_t i = 0; i < function->parameter_count; ++i)
        if (args[i].type != xr_xir_call_type(&view, ctx->function, &op, function->parameters[i]))
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "call argument type does not match declaration");
    return emit_group(ctx, op, args, (uint32_t) call->arg_count, value);
}
static bool source_call(SourceContext *ctx, AstNode *node, SourceValue *value) {
    CallExprNode *call = &node->as.call_expr;
    if (call->arg_count < 0 || call->arg_count > 65536 || call->type_arg_count < 0 ||
        call->type_arg_count > 65536 || call->default_arg_count)
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
        if (!base && member->object->type == AST_VARIABLE && !strcmp(member->object->as.variable.name, "Coro") &&
            !strcmp(member->name, "yield")) {
            if (call->arg_count || call->type_arg_count)
                return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Coro.yield accepts no value or type arguments");
            return emit(ctx, (XrXirInstruction) {XR_XIR_SUSPEND, XR_XIR_UNIT, {0}, {0}, 0}, value);
        }
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
    if ((print || atomic || stream || method != XR_XIR_INVALID) && call->type_arg_count)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "primitive does not admit explicit type arguments");
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
        return ordinary_call(ctx, node, target, args, value);
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
static bool source_logic(SourceContext *ctx, AstNode *node, SourceValue *value) {
    bool negate = node->type == AST_UNARY_NOT;
    SourceValue left, initial, place, right;
    if (!expression(ctx, negate ? node->as.unary.operand : node->as.binary.left, &left)) return false;
    if (left.type != XR_XIR_BOOL) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "logical operand must be bool");
    initial = left;
    if (negate && !emit(ctx, (XrXirInstruction) {XR_XIR_CONST_BOOL, XR_XIR_BOOL, {0}, {0}, 0}, &initial)) return false;
    if (!emit(ctx, (XrXirInstruction) {XR_XIR_LOCAL_NEW, XR_XIR_BOOL, {initial.id, 0}, {0}, 0}, &place)) return false;
    SourceFunction *body = &ctx->bodies[ctx->function];
    uint32_t branch = body->count, rhs = body->block_count;
    if (!emit(ctx, (XrXirInstruction) {XR_XIR_BRANCH, XR_XIR_UNIT, {left.id, 0}, {0}, 0}, NULL) || !begin_block(ctx)) return false;
    if (negate) {
        if (!emit(ctx, (XrXirInstruction) {XR_XIR_CONST_BOOL, XR_XIR_BOOL, {0}, {0}, 1}, &right)) return false;
    } else if (!expression(ctx, node->as.binary.right, &right)) return false;
    if (right.type != XR_XIR_BOOL) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "logical operand must be bool");
    if (!emit(ctx, (XrXirInstruction) {XR_XIR_LOCAL_WRITE, XR_XIR_UNIT, {place.id, right.id}, {0}, 0}, NULL)) return false;
    uint32_t join = body->block_count;
    if (!emit(ctx, (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {join, 0}, 0}, NULL) || !begin_block(ctx)) return false;
    bool conjunction = node->type == AST_BINARY_AND;
    body->ops[branch].targets[0] = conjunction ? rhs : join;
    body->ops[branch].targets[1] = conjunction ? join : rhs;
    return emit(ctx, (XrXirInstruction) {XR_XIR_LOCAL_READ, XR_XIR_BOOL, {place.id, 0}, {0}, 0}, value);
}
static bool source_binary(SourceContext *ctx, AstNode *node, AstNodeType operation,
                          SourceValue left, SourceValue right, SourceValue *value) {
    if (operation == AST_BINARY_ADD && left.type == XR_XIR_STRING && right.type == XR_XIR_STRING)
        return emit(ctx, (XrXirInstruction) {XR_XIR_CONCAT_STRING, XR_XIR_STRING, {left.id, right.id}, {0}, 0}, value);
    if (left.type != XR_XIR_I64 || right.type != XR_XIR_I64)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "operator requires a declared concrete operand contract");
    XrXirOp op;
    switch (operation) {
    case AST_BINARY_ADD: op = XR_XIR_ADD_I64; break;
    case AST_UNARY_NEG: case AST_BINARY_SUB: op = XR_XIR_SUB_I64; break;
    case AST_BINARY_MUL: op = XR_XIR_MUL_I64; break;
    case AST_BINARY_DIV: op = XR_XIR_DIV_I64; break;
    case AST_BINARY_MOD: op = XR_XIR_REM_I64; break;
    case AST_BINARY_BAND: op = XR_XIR_AND_I64; break;
    case AST_BINARY_BOR: op = XR_XIR_OR_I64; break;
    case AST_UNARY_BNOT: case AST_BINARY_BXOR: op = XR_XIR_XOR_I64; break;
    case AST_BINARY_LSHIFT: op = XR_XIR_SHL_I64; break;
    case AST_BINARY_RSHIFT: op = XR_XIR_SHR_I64; break;
    case AST_BINARY_EQ: op = XR_XIR_EQ_I64; break;
    case AST_BINARY_NE: op = XR_XIR_NE_I64; break;
    case AST_BINARY_LT: op = XR_XIR_LT_I64; break;
    case AST_BINARY_LE: op = XR_XIR_LE_I64; break;
    case AST_BINARY_GT: op = XR_XIR_GT_I64; break;
    case AST_BINARY_GE: op = XR_XIR_GE_I64; break;
    default: return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "unknown arithmetic operator");
    }
    bool comparison = operation >= AST_BINARY_EQ && operation <= AST_BINARY_GE;
    return emit(ctx, (XrXirInstruction) {op, comparison ? XR_XIR_BOOL : XR_XIR_I64,
        {left.id, right.id}, {0}, 0}, value);
}
static bool source_arithmetic(SourceContext *ctx, AstNode *node, SourceValue *value) {
    SourceValue left, right;
    if (node->type == AST_UNARY_NEG || node->type == AST_UNARY_BNOT) {
        if (!expression(ctx, node->as.unary.operand, &right) ||
            !emit(ctx, (XrXirInstruction) {XR_XIR_CONST_I64, XR_XIR_I64, {0}, {0}, node->type == AST_UNARY_NEG ? 0 : -1}, &left)) return false;
    } else if (!expression(ctx, node->as.binary.left, &left) ||
               !expression(ctx, node->as.binary.right, &right)) return false;
    return source_binary(ctx, node, node->type, left, right, value);
}
static bool source_compound(SourceContext *ctx, AstNode *node, SourceValue *value) {
    CompoundAssignmentNode *assignment = &node->as.compound_assignment;
    if (assignment->object || !assignment->name)
        return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "compound assignment requires a variable");
    SourceName *symbol = visible_name(ctx, assignment->name);
    if (!symbol || !symbol->mutable || (symbol->kind != SOURCE_LOCAL && symbol->kind != SOURCE_SLOT))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "compound assignment requires a mutable binding");
    AstNodeType operation;
    switch (assignment->op) {
    case TK_PLUS_ASSIGN: operation = AST_BINARY_ADD; break;
    case TK_MINUS_ASSIGN: operation = AST_BINARY_SUB; break;
    case TK_MUL_ASSIGN: operation = AST_BINARY_MUL; break;
    case TK_DIV_ASSIGN: operation = AST_BINARY_DIV; break;
    case TK_MOD_ASSIGN: operation = AST_BINARY_MOD; break;
    case TK_AND_ASSIGN: operation = AST_BINARY_BAND; break;
    case TK_OR_ASSIGN: operation = AST_BINARY_BOR; break;
    case TK_XOR_ASSIGN: operation = AST_BINARY_BXOR; break;
    case TK_LSHIFT_ASSIGN: operation = AST_BINARY_LSHIFT; break;
    case TK_RSHIFT_ASSIGN: operation = AST_BINARY_RSHIFT; break;
    default: return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "unknown compound assignment");
    }
    SourceValue left, right;
    XrXirInstruction read = symbol->kind == SOURCE_LOCAL ?
        (XrXirInstruction) {XR_XIR_LOCAL_READ, symbol->type, {symbol->index, 0}, {0}, 0} :
        (XrXirInstruction) {XR_XIR_SLOT_LOAD, symbol->type, {0}, {0}, symbol->index};
    if (!emit(ctx, read, &left) || !expression(ctx, assignment->value, &right) ||
        !source_binary(ctx, node, operation, left, right, value)) return false;
    XrXirInstruction write = symbol->kind == SOURCE_LOCAL ?
        (XrXirInstruction) {XR_XIR_LOCAL_WRITE, XR_XIR_UNIT, {symbol->index, value->id}, {0}, 0} :
        (XrXirInstruction) {XR_XIR_SLOT_STORE, XR_XIR_UNIT, {value->id, 0}, {0}, symbol->index};
    return emit(ctx, write, NULL);
}
static bool source_conditional(SourceContext *ctx, AstNode *node, SourceValue *value) {
    SourceValue condition, yes, no;
    TernaryNode *ternary = &node->as.ternary;
    if (!expression(ctx, ternary->condition, &condition)) return false;
    if (condition.type != XR_XIR_BOOL) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "conditional requires bool");
    SourceFunction *body = &ctx->bodies[ctx->function];
    if (!body->block_count && !begin_block(ctx)) return false;
    uint32_t branch = body->count, yes_block = body->block_count;
    if (!emit(ctx, (XrXirInstruction) {XR_XIR_BRANCH, XR_XIR_UNIT, {condition.id}, {0}, 0}, NULL) ||
        !begin_block(ctx) || !expression(ctx, ternary->true_expr, &yes)) return false;
    uint32_t yes_end = body->block_count - 1, yes_jump = body->count;
    if (!emit(ctx, (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {0}, 0}, NULL)) return false;
    uint32_t no_block = body->block_count;
    if (!begin_block(ctx) || !expression(ctx, ternary->false_expr, &no)) return false;
    if (yes.type != no.type) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "conditional branch types must match");
    uint32_t no_end = body->block_count - 1, join = body->block_count;
    if (!emit(ctx, (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {join}, 0}, NULL) || !begin_block(ctx)) return false;
    body->ops[branch].targets[0] = yes_block; body->ops[branch].targets[1] = no_block;
    body->ops[yes_jump].targets[0] = join;
    if (yes.type == XR_XIR_UNIT) { *value = (SourceValue) {0, XR_XIR_UNIT}; return true; }
    SourceValue inputs[] = {{yes_end, XR_XIR_UNIT}, yes, {no_end, XR_XIR_UNIT}, no};
    return emit_group(ctx, (XrXirInstruction) {XR_XIR_PHI, yes.type, {0}, {0}, 0}, inputs, 4, value);
}
static bool expression_body(SourceContext *ctx, AstNode *node, SourceValue *value) {
    switch (node->type) {
    case AST_LITERAL_INT: case AST_LITERAL_TRUE: case AST_LITERAL_FALSE: case AST_LITERAL_STRING:
        return source_literal(ctx, node, value);
    case AST_TERNARY: return source_conditional(ctx, node, value);
    case AST_GROUPING: return expression(ctx, node->as.grouping, value);
    case AST_CALL_EXPR: return source_call(ctx, node, value);
    case AST_UNARY_NOT: case AST_BINARY_AND: case AST_BINARY_OR: return source_logic(ctx, node, value);
    case AST_VARIABLE: {
        SourceName *symbol = visible_name(ctx, node->as.variable.name);
        if (!symbol || (symbol->kind != SOURCE_SLOT && symbol->kind != SOURCE_LOCAL) || !symbol->type)
            return source_fail(ctx, node, XR_XIR_BAD_VALUE, "name is not an initialized value");
        if (symbol->kind == SOURCE_LOCAL) {
            if (symbol->mutable) return emit(ctx, (XrXirInstruction) {XR_XIR_LOCAL_READ, symbol->type,
                {symbol->index, 0}, {0}, 0}, value);
            *value = (SourceValue) {symbol->index, symbol->type}; return true;
        }
        return emit(ctx, (XrXirInstruction) {XR_XIR_SLOT_LOAD, symbol->type, {0, 0}, {0, 0}, symbol->index}, value);
    }
    case AST_UNARY_BNOT: case AST_BINARY_BAND: case AST_BINARY_BOR: case AST_BINARY_BXOR:
    case AST_BINARY_LSHIFT: case AST_BINARY_RSHIFT:
    case AST_UNARY_NEG: case AST_BINARY_ADD: case AST_BINARY_SUB: case AST_BINARY_MUL:
    case AST_BINARY_DIV: case AST_BINARY_MOD: case AST_BINARY_EQ: case AST_BINARY_NE:
    case AST_BINARY_LT: case AST_BINARY_LE: case AST_BINARY_GT: case AST_BINARY_GE:
        return source_arithmetic(ctx, node, value);
    case AST_COMPOUND_ASSIGNMENT: return source_compound(ctx, node, value);
    case AST_ASSIGNMENT: {
        SourceName *symbol = visible_name(ctx, node->as.assignment.name);
        SourceValue assigned;
        if (!symbol || !symbol->mutable || (symbol->kind != SOURCE_LOCAL && symbol->kind != SOURCE_SLOT))
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "assignment requires a mutable binding");
        if (!expression(ctx, node->as.assignment.value, &assigned)) return false;
        if (assigned.type != symbol->type) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "assignment type mismatch");
        if (symbol->kind == SOURCE_LOCAL) {
            if (!emit(ctx, (XrXirInstruction) {XR_XIR_LOCAL_WRITE, XR_XIR_UNIT,
                {symbol->index, assigned.id}, {0}, 0}, NULL)) return false;
        }
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
    for (SourceName *p = ctx->locals; p != ctx->scope; p = p->next) {
        if (!source_work(ctx, node)) return false;
        if (!strcmp(p->name, decl->name)) return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "duplicate local name");
    }
    symbol = source_alloc(ctx, 1, sizeof(*symbol));
    if (!symbol) return false;
    if (!decl->is_const && !emit(ctx, (XrXirInstruction) {XR_XIR_LOCAL_NEW, initial.type,
        {initial.id, 0}, {0}, 0}, &initial)) return false;
    *symbol = (SourceName) {ctx->locals, decl->name, NULL, node, SOURCE_LOCAL, initial.id, ctx->module, initial.type, !decl->is_const};
    ctx->locals = symbol;
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
    if (!emit(ctx, (XrXirInstruction) {XR_XIR_BRANCH, XR_XIR_UNIT, {condition.id, 0}, {0}, 0}, NULL) ||
        !begin_block(ctx) || !scoped_statement(ctx, node->as.if_stmt.then_branch)) return false;
    uint32_t yes_jump = UINT32_MAX, no_jump = UINT32_MAX;
    if (!ctx->returned) {
        yes_jump = body->count;
        if (!emit(ctx, (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {0}, 0}, NULL)) return false;
    }
    uint32_t no = body->block_count;
    if (!begin_block(ctx)) return false;
    if (node->as.if_stmt.else_branch && !scoped_statement(ctx, node->as.if_stmt.else_branch)) return false;
    if (!ctx->returned) {
        no_jump = body->count;
        if (!emit(ctx, (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {0}, 0}, NULL)) return false;
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
    if (!symbol || !symbol->mutable || symbol->type != XR_XIR_I64 ||
        (symbol->kind != SOURCE_LOCAL && symbol->kind != SOURCE_SLOT))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "increment requires a mutable i64 binding");
    SourceValue old, one, result;
    XrXirInstruction read = symbol->kind == SOURCE_LOCAL ?
        (XrXirInstruction) {XR_XIR_LOCAL_READ, XR_XIR_I64, {symbol->index, 0}, {0}, 0} :
        (XrXirInstruction) {XR_XIR_SLOT_LOAD, XR_XIR_I64, {0}, {0}, symbol->index};
    if (!emit(ctx, read, &old) ||
        !emit(ctx, (XrXirInstruction) {XR_XIR_CONST_I64, XR_XIR_I64, {0}, {0}, node->type == AST_INC ? 1 : -1}, &one) ||
        !emit(ctx, (XrXirInstruction) {XR_XIR_ADD_I64, XR_XIR_I64, {old.id, one.id}, {0}, 0}, &result)) return false;
    XrXirInstruction write = symbol->kind == SOURCE_LOCAL ?
        (XrXirInstruction) {XR_XIR_LOCAL_WRITE, XR_XIR_UNIT, {symbol->index, result.id}, {0}, 0} :
        (XrXirInstruction) {XR_XIR_SLOT_STORE, XR_XIR_UNIT, {result.id, 0}, {0}, symbol->index};
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
    if (!emit(ctx, (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {condition_block, 0}, 0}, NULL) || !begin_block(ctx)) return false;
    SourceValue condition;
    if (condition_node) { if (!expression(ctx, condition_node, &condition)) return false; }
    else if (!emit(ctx, (XrXirInstruction) {XR_XIR_CONST_BOOL, XR_XIR_BOOL, {0}, {0}, 1}, &condition)) return false;
    if (condition.type != XR_XIR_BOOL) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "loop condition requires bool");
    uint32_t branch = body->count, entry = body->block_count;
    if (!emit(ctx, (XrXirInstruction) {XR_XIR_BRANCH, XR_XIR_UNIT, {condition.id, 0}, {entry, 0}, 0}, NULL) || !begin_block(ctx)) return false;
    SourceLoop loop = {ctx->loop, NULL, NULL}; ctx->loop = &loop;
    bool ok = scoped_statement(ctx, loop_body); ctx->loop = loop.parent;
    if (!ok) return false;
    if (!ctx->returned || loop.continues) {
        uint32_t next = step ? body->block_count : condition_block;
        if (!ctx->returned && !emit(ctx, (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {next, 0}, 0}, NULL)) return false;
        if (!patch_exits(ctx, loop.continues, next)) return false;
        if (step && (!begin_block(ctx) || !source_step(ctx, step) ||
            !emit(ctx, (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {condition_block, 0}, 0}, NULL))) return false;
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
    if (!ctx->loop || label) return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "loop exit requires an unlabelled enclosing loop");
    SourcePatch **head = stop ? &ctx->loop->breaks : &ctx->loop->continues;
    SourcePatch *patch = source_alloc(ctx, 1, sizeof(*patch)); if (!patch) return false;
    *patch = (SourcePatch) {*head, ctx->bodies[ctx->function].count}; *head = patch;
    ctx->returned = true;
    return emit(ctx, (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {0}, 0}, NULL);
}
static bool statement(SourceContext *ctx, AstNode *node, bool top) {
    if (!node || !source_work(ctx, node)) return false;
    if (ctx->returned) return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "unreachable statements are not admitted");
    switch (node->type) {
    case AST_IF_STMT: return source_if(ctx, node);
    case AST_WHILE_STMT:
        if (node->as.while_stmt.label) return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "labelled loops are not admitted");
        return source_loop(ctx, node, node->as.while_stmt.condition, node->as.while_stmt.body, NULL);
    case AST_FOR_STMT: return source_for(ctx, node);
    case AST_INC: case AST_DEC: return source_increment(ctx, node);
    case AST_BREAK_STMT: case AST_CONTINUE_STMT: return source_loop_exit(ctx, node);
    case AST_IMPORT_STMT: case AST_FUNCTION_DECL:
        return top || source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "nested declarations are not admitted");
    case AST_VAR_DECL: case AST_CONST_DECL: return source_binding(ctx, node, top);
    case AST_EXPR_STMT: { SourceValue value; return expression(ctx, node->as.expr_stmt, &value); }
    case AST_RETURN_STMT: {
        if (top || !ctx->bodies[ctx->function].node || node->as.return_stmt.value_count > 1)
            return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "invalid return placement or arity");
        SourceValue value = {0};
        if (node->as.return_stmt.value_count && !expression(ctx, node->as.return_stmt.values[0], &value)) return false;
        if (value.type != ctx->functions[ctx->function].result)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "return type does not match declaration");
        ctx->returned = true;
        return emit(ctx, (XrXirInstruction) {XR_XIR_RETURN, XR_XIR_UNIT, {value.type == XR_XIR_UNIT ? 0 : value.id, 0}, {0, 0}, 0}, NULL);
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
    if (decl->is_generator || decl->is_extern || decl->attr_count || decl->type_param_count < 0 || decl->type_param_count > 65536 ||
        decl->throws_count || decl->borrow_origin_count || !decl->body || decl->param_count > 65536 || decl->param_count < 0)
        return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "function contract is not implemented in XIR");
    SourceName *symbol = add_name(ctx, &ctx->names[ctx->module], decl->name, node);
    if (!symbol) return false;
    symbol->kind = SOURCE_FUNCTION; symbol->index = index;
    SourceFunction *body = &ctx->bodies[index]; body->node = node; body->module = ctx->module;
    ctx->function = index;
    uint32_t *constraints = decl->type_param_count ? source_alloc(ctx, (size_t) decl->type_param_count, sizeof(*constraints)) : NULL;
    if (decl->type_param_count && !constraints) return false;
    ctx->generics[index].constraints = constraints;
    ctx->generics[index].parameter_count = (uint32_t) decl->type_param_count;
    ctx->has_generics |= decl->type_param_count != 0;
    for (int i = 0; i < decl->type_param_count; ++i) {
        XrGenericParam *parameter = decl->type_params[i];
        if (!parameter->name || !strcmp(parameter->name, "Sendable") || parameter->constraint_count < 0 || parameter->constraint_count > 1)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "generic parameter contract is not admitted");
        for (int j = 0; j < i; ++j) {
            if (!source_work(ctx, node)) return false;
            if (!strcmp(parameter->name, decl->type_params[j]->name))
                return source_fail(ctx, node, XR_XIR_BAD_TYPE, "duplicate type parameter");
        }
        if (parameter->constraint_count) {
            XrTypeRef *constraint = parameter->constraints[0];
            if (constraint->kind != XR_TREF_NAMED || !constraint->name || strcmp(constraint->name, "Sendable"))
                return source_fail(ctx, node, XR_XIR_BAD_TYPE, "only the Sendable marker constraint is admitted");
            constraints[i] = XR_XIR_CONSTRAINT_SENDABLE;
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
    ctx->generics = source_alloc(ctx, functions, sizeof(*ctx->generics));
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
        ctx->functions[m] = (XrXirFunction) {"$init", 5, NULL, 0, XR_XIR_UNIT, NULL, 0, NULL, 0, NULL, 0};
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
    function->blocks = body->blocks; function->block_count = body->block_count;
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
        NULL, 0, NULL, 0, NULL, 0};
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
        XrXirModule built = {XR_XIR_BUILT, ctx.functions, ctx.function_count, &declarations, ctx.has_generics ? ctx.generics : NULL};
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
