/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_mono.c - Monomorphization Pass infrastructure
 *
 * KEY CONCEPT:
 *   Provides AST cloning, type substitution, and name mangling for
 *   monomorphizing generic functions and classes. Each concrete type
 *   combination gets its own specialized AST and bytecode.
 *
 * WHY THIS DESIGN:
 *   - Rep-sharing: all reference types (string, Array, class) share one PTR
 *     version, so each generic produces at most 3 versions (I64, F64, PTR)
 *   - Duck-typed: no trait bounds needed, errors reported at instantiation
 */

#include "xanalyzer_mono.h"
#include "../../base/xlog.h"
#include "../../base/xchecks.h"
#include "../../runtime/value/xtype.h"
#include "../parser/xast_nodes.h"
#include "../parser/xtype_ref.h"
#include "xtype_ref_resolve.h"
#include "../../base/xmalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========== Safety Limits ========== */

// Max total mono instances across all generics in one program
#define XR_MONO_MAX_INSTANCES 256

// Max instantiations per single generic (prevents combinatorial explosion)
#define XR_MONO_MAX_PER_GENERIC 32

/* ========== Name Mangling ========== */

/* User-facing display name for a concrete type argument.
 * Returns canonical names: "int", "float", "string", "bool", etc.
 * For named/generic types, returns the type's own name (e.g. "Array"). */
static const char *mono_type_display_name(XrTypeRef *t) {
    if (!t)
        return "unknown";
    switch ((XrTypeRefKind) t->kind) {
        case XR_TREF_INT:
        case XR_TREF_INT_WIDTH:
            return "int";
        case XR_TREF_FLOAT:
        case XR_TREF_FLOAT_WIDTH:
            return "float";
        case XR_TREF_BOOL:
            return "bool";
        case XR_TREF_STRING:
            return "string";
        case XR_TREF_NULL:
            return "null";
        case XR_TREF_UNIT:
            return "()";
        case XR_TREF_NAMED:
        case XR_TREF_GENERIC:
            return t->name ? t->name : "object";
        case XR_TREF_FUNCTION:
            return "function";
        case XR_TREF_OPTIONAL:
            return "optional";
        case XR_TREF_TYPE_PARAM:
            return t->name ? t->name : "T";
        default:
            return "unknown";
    }
}

const char *xr_mono_type_tag(XrTypeRef *t) {
    if (!t)
        return "unknown";
    switch ((XrTypeRefKind) t->kind) {
        case XR_TREF_INT:
        case XR_TREF_INT_WIDTH:
            return "i64";
        case XR_TREF_FLOAT:
        case XR_TREF_FLOAT_WIDTH:
            return "f64";
        case XR_TREF_BOOL:
            return "bool";
        case XR_TREF_STRING:
            return "str";
        case XR_TREF_NULL:
            return "null";
        case XR_TREF_UNIT:
            return "unit";
        case XR_TREF_NAMED:
        case XR_TREF_GENERIC:
            return t->name ? t->name : "obj";
        case XR_TREF_FUNCTION:
            return "fn";
        case XR_TREF_OPTIONAL:
            return "opt";
        case XR_TREF_TYPE_PARAM:
            return t->name ? t->name : "T";
        default:
            return "unknown";
    }
}

char *xr_mono_mangle(const char *name, XrTypeRef **type_args, int count) {
    if (!name || count <= 0 || !type_args)
        return xr_strdup(name ? name : "");

    // Calculate buffer size: name + '$' + tags joined by '_'
    size_t len = strlen(name) + 1;  // name + '$'
    for (int i = 0; i < count; i++) {
        const char *tag = xr_mono_type_tag(type_args[i]);
        len += strlen(tag) + 1;  // tag + '_' separator
    }
    len += 1;  // null terminator

    char *buf = (char *) xr_malloc(len);
    if (!buf)
        return xr_strdup(name);

    char *p = buf;
    size_t remaining = len;
    int written = snprintf(p, remaining, "%s$", name);
    p += written;
    remaining -= written;

    for (int i = 0; i < count; i++) {
        const char *tag = xr_mono_type_tag(type_args[i]);
        if (i > 0) {
            *p++ = '_';
            remaining--;
        }
        written = snprintf(p, remaining, "%s", tag);
        p += written;
        remaining -= written;
    }
    return buf;
}

/* ========== Type Substitution ========== */

XrTypeRef *xr_mono_type_substitute(XrTypeRef *type, XrMonoTypeMap *map, int map_count) {
    if (!type || !map || map_count <= 0)
        return type;

    /* Direct substitution for type parameters and named refs matching a param */
    if ((type->kind == XR_TREF_TYPE_PARAM || type->kind == XR_TREF_NAMED) && type->name) {
        for (int i = 0; i < map_count; i++) {
            if (map[i].param_name && strcmp(type->name, map[i].param_name) == 0)
                return map[i].concrete_type ? map[i].concrete_type : type;
        }
    }

    /* Recurse into children (OPTIONAL, UNION, GENERIC, FUNCTION, etc.) */
    if (type->nchildren > 0 && type->children) {
        bool changed = false;
        XrTypeRef **new_children = (XrTypeRef **) xr_calloc(type->nchildren, sizeof(XrTypeRef *));
        if (!new_children)
            return type;
        for (int i = 0; i < type->nchildren; i++) {
            new_children[i] = xr_mono_type_substitute(type->children[i], map, map_count);
            if (new_children[i] != type->children[i])
                changed = true;
        }
        if (!changed) {
            xr_free(new_children);
            return type;
        }
        XrTypeRef *result = (XrTypeRef *) xr_calloc(1, sizeof(XrTypeRef));
        if (!result) {
            xr_free(new_children);
            return type;
        }
        *result = *type;
        result->children = new_children;
        return result;
    }

    return type;
}

/* ========== AST Clone ========== */

static char *clone_str(const char *s) {
    return s ? xr_strdup(s) : NULL;
}

static AstNode **clone_node_array(AstNode **arr, int count, XrMonoTypeMap *map, int map_count) {
    if (!arr || count <= 0)
        return NULL;
    AstNode **result = (AstNode **) xr_calloc(count, sizeof(AstNode *));
    for (int i = 0; i < count; i++) {
        result[i] = xr_ast_clone(arr[i], map, map_count);
    }
    return result;
}

/* Substitute type parameters in an XrTypeRef tree.
 * Returns a new XrTypeRef if substitution occurred,
 * or the original pointer unchanged. */
static XrTypeRef *sub_tref(XrTypeRef *t, XrMonoTypeMap *map, int mc) {
    return (map && mc > 0) ? xr_mono_type_substitute(t, map, mc) : t;
}

static XrTypeRef **clone_tref_array(XrTypeRef **arr, int count, XrMonoTypeMap *map, int mc) {
    if (!arr || count <= 0)
        return NULL;
    XrTypeRef **result = (XrTypeRef **) xr_calloc((size_t) count, sizeof(XrTypeRef *));
    for (int i = 0; i < count; i++)
        result[i] = sub_tref(arr[i], map, mc);
    return result;
}

static XrParamNode **clone_params(XrParamNode **params, int count, XrMonoTypeMap *map, int mc) {
    if (!params || count <= 0)
        return NULL;
    XrParamNode **result = (XrParamNode **) xr_calloc(count, sizeof(XrParamNode *));
    for (int i = 0; i < count; i++) {
        XrParamNode *p = (XrParamNode *) xr_calloc(1, sizeof(XrParamNode));
        *p = *params[i];
        p->name = clone_str(params[i]->name);
        p->type = sub_tref(params[i]->type, map, mc);
        p->default_value = xr_ast_clone(params[i]->default_value, map, mc);
        // pattern clone omitted (not used in generic contexts)
        result[i] = p;
    }
    return result;
}

static char **clone_str_array(char **arr, int count) {
    if (!arr || count <= 0)
        return NULL;
    char **result = (char **) xr_calloc(count, sizeof(char *));
    for (int i = 0; i < count; i++) {
        result[i] = clone_str(arr[i]);
    }
    return result;
}

AstNode *xr_ast_clone(AstNode *node, XrMonoTypeMap *map, int mc) {
    XR_DCHECK(map != NULL || mc == 0, "xr_ast_clone: map is NULL with non-zero mc");
    if (!node)
        return NULL;

    AstNode *n = (AstNode *) xr_calloc(1, sizeof(AstNode));
    n->type = node->type;
    n->line = node->line;
    n->column = node->column;
    n->leading_comments = NULL;   // Comments not needed for mono clones
    n->trailing_comments = NULL;  // (L-06)
    // AstNode no longer carries an inline type — the post-mono
    // xa_analyzer_analyze() pass in xcompiler.c re-infers every cloned
    // node and writes the result to the analyzer's side table, so
    // dropping the per-node copy here is safe.

    switch (node->type) {
        // === Literals ===
        case AST_LITERAL_INT:
        case AST_LITERAL_FLOAT:
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
        case AST_LITERAL_NULL:
            n->as.literal = node->as.literal;
            break;
        case AST_LITERAL_STRING:
            n->as.literal = node->as.literal;
            n->as.literal.raw_value.string_val = clone_str(node->as.literal.raw_value.string_val);
            break;
        case AST_LITERAL_BIGINT:
            n->as.literal = node->as.literal;
            n->as.literal.raw_value.bigint_val = clone_str(node->as.literal.raw_value.bigint_val);
            break;
        case AST_LITERAL_REGEX:
            n->as.literal = node->as.literal;
            n->as.literal.raw_value.regex.pattern =
                clone_str(node->as.literal.raw_value.regex.pattern);
            n->as.literal.raw_value.regex.flags = clone_str(node->as.literal.raw_value.regex.flags);
            break;

        // === Binary / Unary ===
        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_MOD:
        case AST_BINARY_BAND:
        case AST_BINARY_BOR:
        case AST_BINARY_BXOR:
        case AST_BINARY_LSHIFT:
        case AST_BINARY_RSHIFT:
        case AST_BINARY_EQ:
        case AST_BINARY_NE:
        case AST_BINARY_EQ_STRICT:
        case AST_BINARY_NE_STRICT:
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_BINARY_AND:
        case AST_BINARY_OR:
        case AST_NULLISH_COALESCE:
            n->as.binary.left = xr_ast_clone(node->as.binary.left, map, mc);
            n->as.binary.right = xr_ast_clone(node->as.binary.right, map, mc);
            break;
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
            n->as.unary.operand = xr_ast_clone(node->as.unary.operand, map, mc);
            break;

        // === Grouping / Expr stmt ===
        case AST_GROUPING:
            n->as.grouping = xr_ast_clone(node->as.grouping, map, mc);
            break;
        case AST_EXPR_STMT:
            n->as.expr_stmt = xr_ast_clone(node->as.expr_stmt, map, mc);
            break;

        // === Print ===
        case AST_PRINT_STMT:
            n->as.print_stmt.expr_count = node->as.print_stmt.expr_count;
            n->as.print_stmt.exprs = clone_node_array(node->as.print_stmt.exprs,
                                                      node->as.print_stmt.expr_count, map, mc);
            break;

        // === Block ===
        case AST_BLOCK:
            n->as.block.count = node->as.block.count;
            n->as.block.capacity = node->as.block.count;
            n->as.block.statements =
                clone_node_array(node->as.block.statements, node->as.block.count, map, mc);
            break;

        // === Variable ===
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            n->as.var_decl.name = clone_str(node->as.var_decl.name);
            n->as.var_decl.initializer = xr_ast_clone(node->as.var_decl.initializer, map, mc);
            n->as.var_decl.is_const = node->as.var_decl.is_const;
            n->as.var_decl.storage_mode = node->as.var_decl.storage_mode;
            n->as.var_decl.type_annotation = sub_tref(node->as.var_decl.type_annotation, map, mc);
            break;
        case AST_VARIABLE:
            n->as.variable.name = clone_str(node->as.variable.name);
            break;
        case AST_ASSIGNMENT:
            n->as.assignment.name = clone_str(node->as.assignment.name);
            n->as.assignment.value = xr_ast_clone(node->as.assignment.value, map, mc);
            break;
        case AST_COMPOUND_ASSIGNMENT:
            n->as.compound_assignment.name = clone_str(node->as.compound_assignment.name);
            n->as.compound_assignment.op = node->as.compound_assignment.op;
            n->as.compound_assignment.value =
                xr_ast_clone(node->as.compound_assignment.value, map, mc);
            n->as.compound_assignment.object =
                xr_ast_clone(node->as.compound_assignment.object, map, mc);
            break;
        case AST_INC:
        case AST_DEC:
            n->as.inc.name = clone_str(node->as.inc.name);
            break;

        // === Control flow ===
        case AST_IF_STMT:
            n->as.if_stmt.condition = xr_ast_clone(node->as.if_stmt.condition, map, mc);
            n->as.if_stmt.then_branch = xr_ast_clone(node->as.if_stmt.then_branch, map, mc);
            n->as.if_stmt.else_branch = xr_ast_clone(node->as.if_stmt.else_branch, map, mc);
            break;
        case AST_WHILE_STMT:
            n->as.while_stmt.condition = xr_ast_clone(node->as.while_stmt.condition, map, mc);
            n->as.while_stmt.body = xr_ast_clone(node->as.while_stmt.body, map, mc);
            break;
        case AST_FOR_STMT:
            n->as.for_stmt.initializer = xr_ast_clone(node->as.for_stmt.initializer, map, mc);
            n->as.for_stmt.condition = xr_ast_clone(node->as.for_stmt.condition, map, mc);
            n->as.for_stmt.increment = xr_ast_clone(node->as.for_stmt.increment, map, mc);
            n->as.for_stmt.body = xr_ast_clone(node->as.for_stmt.body, map, mc);
            break;
        case AST_FOR_IN_STMT:
            n->as.for_in_stmt.item_name = clone_str(node->as.for_in_stmt.item_name);
            n->as.for_in_stmt.value_name = clone_str(node->as.for_in_stmt.value_name);
            n->as.for_in_stmt.is_keyvalue = node->as.for_in_stmt.is_keyvalue;
            n->as.for_in_stmt.item_type = sub_tref(node->as.for_in_stmt.item_type, map, mc);
            n->as.for_in_stmt.collection = xr_ast_clone(node->as.for_in_stmt.collection, map, mc);
            n->as.for_in_stmt.body = xr_ast_clone(node->as.for_in_stmt.body, map, mc);
            break;
        case AST_BREAK_STMT:
        case AST_CONTINUE_STMT:
            break;  // No fields to clone

        // === Function ===
        case AST_FUNCTION_DECL:
        case AST_FUNCTION_EXPR: {
            FunctionDeclNode *src = &node->as.function_decl;
            FunctionDeclNode *dst = &n->as.function_decl;
            dst->name = clone_str(src->name);
            dst->params = clone_params(src->params, src->param_count, map, mc);
            dst->param_count = src->param_count;
            dst->required_count = src->required_count;
            dst->return_type = sub_tref(src->return_type, map, mc);
            dst->body = xr_ast_clone(src->body, map, mc);
            dst->is_generator = src->is_generator;
            dst->attributes = NULL;  // Attributes not cloned for mono
            dst->attr_count = 0;
            dst->type_params = NULL;  // Cleared: mono version has no type params
            dst->type_param_count = 0;
            break;
        }

        // === Call ===
        case AST_CALL_EXPR:
            n->as.call_expr.callee = xr_ast_clone(node->as.call_expr.callee, map, mc);
            n->as.call_expr.arg_count = node->as.call_expr.arg_count;
            n->as.call_expr.arguments = clone_node_array(node->as.call_expr.arguments,
                                                         node->as.call_expr.arg_count, map, mc);
            n->as.call_expr.type_args = clone_tref_array(
                node->as.call_expr.type_args, node->as.call_expr.type_arg_count, map, mc);
            n->as.call_expr.type_arg_count = node->as.call_expr.type_arg_count;
            break;

        // === Return / Yield ===
        case AST_RETURN_STMT:
            n->as.return_stmt.value_count = node->as.return_stmt.value_count;
            n->as.return_stmt.values = clone_node_array(node->as.return_stmt.values,
                                                        node->as.return_stmt.value_count, map, mc);
            break;
        // === Type check ===
        case AST_IS_EXPR:
            n->as.is_expr.expr = xr_ast_clone(node->as.is_expr.expr, map, mc);
            n->as.is_expr.type = sub_tref(node->as.is_expr.type, map, mc);
            break;
        case AST_AS_EXPR:
            n->as.as_expr.expr = xr_ast_clone(node->as.as_expr.expr, map, mc);
            n->as.as_expr.type = sub_tref(node->as.as_expr.type, map, mc);
            n->as.as_expr.is_safe = node->as.as_expr.is_safe;
            break;

        // === Array / Index / Slice ===
        case AST_ARRAY_LITERAL:
            n->as.array_literal.count = node->as.array_literal.count;
            n->as.array_literal.elements = clone_node_array(node->as.array_literal.elements,
                                                            node->as.array_literal.count, map, mc);
            break;
        case AST_INDEX_GET:
            n->as.index_get.array = xr_ast_clone(node->as.index_get.array, map, mc);
            n->as.index_get.index = xr_ast_clone(node->as.index_get.index, map, mc);
            break;
        case AST_INDEX_SET:
            n->as.index_set.array = xr_ast_clone(node->as.index_set.array, map, mc);
            n->as.index_set.index = xr_ast_clone(node->as.index_set.index, map, mc);
            n->as.index_set.value = xr_ast_clone(node->as.index_set.value, map, mc);
            break;
        case AST_SLICE_EXPR:
            n->as.slice_expr.source = xr_ast_clone(node->as.slice_expr.source, map, mc);
            n->as.slice_expr.start = xr_ast_clone(node->as.slice_expr.start, map, mc);
            n->as.slice_expr.end = xr_ast_clone(node->as.slice_expr.end, map, mc);
            break;

        // === Member access ===
        case AST_MEMBER_ACCESS:
            n->as.member_access.object = xr_ast_clone(node->as.member_access.object, map, mc);
            n->as.member_access.name = clone_str(node->as.member_access.name);
            break;
        case AST_MEMBER_SET:
            n->as.member_set.object = xr_ast_clone(node->as.member_set.object, map, mc);
            n->as.member_set.member = clone_str(node->as.member_set.member);
            n->as.member_set.value = xr_ast_clone(node->as.member_set.value, map, mc);
            break;

        // === Template string ===
        case AST_TEMPLATE_STRING:
            n->as.template_str.part_count = node->as.template_str.part_count;
            n->as.template_str.parts = clone_node_array(node->as.template_str.parts,
                                                        node->as.template_str.part_count, map, mc);
            break;

        // === Object / Map / Set literals ===
        case AST_OBJECT_LITERAL:
            n->as.object_literal.count = node->as.object_literal.count;
            n->as.object_literal.keys = clone_node_array(node->as.object_literal.keys,
                                                         node->as.object_literal.count, map, mc);
            n->as.object_literal.values = clone_node_array(node->as.object_literal.values,
                                                           node->as.object_literal.count, map, mc);
            if (node->as.object_literal.computed) {
                n->as.object_literal.computed =
                    (bool *) xr_calloc(node->as.object_literal.count, sizeof(bool));
                memcpy(n->as.object_literal.computed, node->as.object_literal.computed,
                       node->as.object_literal.count * sizeof(bool));
            }
            break;
        case AST_MAP_LITERAL:
            n->as.map_literal.count = node->as.map_literal.count;
            n->as.map_literal.keys =
                clone_node_array(node->as.map_literal.keys, node->as.map_literal.count, map, mc);
            n->as.map_literal.values =
                clone_node_array(node->as.map_literal.values, node->as.map_literal.count, map, mc);
            break;
        case AST_SET_LITERAL:
            n->as.set_literal.count = node->as.set_literal.count;
            n->as.set_literal.elements = clone_node_array(node->as.set_literal.elements,
                                                          node->as.set_literal.count, map, mc);
            break;

        // === Ternary / Range ===
        case AST_TERNARY:
            n->as.ternary.condition = xr_ast_clone(node->as.ternary.condition, map, mc);
            n->as.ternary.true_expr = xr_ast_clone(node->as.ternary.true_expr, map, mc);
            n->as.ternary.false_expr = xr_ast_clone(node->as.ternary.false_expr, map, mc);
            break;
        case AST_RANGE:
            n->as.range.start = xr_ast_clone(node->as.range.start, map, mc);
            n->as.range.end = xr_ast_clone(node->as.range.end, map, mc);
            break;

        // === Optional chain / Force unwrap ===
        case AST_OPTIONAL_CHAIN:
            n->as.optional_chain.object = xr_ast_clone(node->as.optional_chain.object, map, mc);
            n->as.optional_chain.name = clone_str(node->as.optional_chain.name);
            n->as.optional_chain.index = xr_ast_clone(node->as.optional_chain.index, map, mc);
            n->as.optional_chain.chain_type = node->as.optional_chain.chain_type;
            break;
        case AST_FORCE_UNWRAP:
        case AST_TRY_OPTIONAL:
        case AST_TRY_FORCE:
            n->as.unary.operand = xr_ast_clone(node->as.unary.operand, map, mc);
            break;

        // === Try-catch / Throw ===
        case AST_TRY_CATCH: {
            TryCatchNode *src_tc = &node->as.try_catch;
            TryCatchNode *dst_tc = &n->as.try_catch;
            dst_tc->try_body = xr_ast_clone(src_tc->try_body, map, mc);
            dst_tc->catch_count = src_tc->catch_count;
            dst_tc->catch_clauses = NULL;
            if (src_tc->catch_count > 0) {
                dst_tc->catch_clauses = (XrCatchClause **) xr_calloc((size_t) src_tc->catch_count,
                                                                     sizeof(XrCatchClause *));
                for (int ci = 0; ci < src_tc->catch_count; ci++) {
                    XrCatchClause *sc = src_tc->catch_clauses[ci];
                    if (!sc)
                        continue;
                    XrCatchClause *dc = (XrCatchClause *) xr_calloc(1, sizeof(XrCatchClause));
                    dc->var_name = clone_str(sc->var_name);
                    dc->var_line = sc->var_line;
                    dc->var_column = sc->var_column;
                    dc->type = sub_tref(sc->type, map, mc);
                    dc->body = xr_ast_clone(sc->body, map, mc);
                    dc->symbol_id = 0;
                    dst_tc->catch_clauses[ci] = dc;
                }
            }
            dst_tc->finally_body = xr_ast_clone(src_tc->finally_body, map, mc);
            break;
        }
        case AST_THROW_STMT:
            n->as.throw_stmt.expression = xr_ast_clone(node->as.throw_stmt.expression, map, mc);
            break;

        // === new expression ===
        case AST_NEW_EXPR:
            n->as.new_expr.module_name = clone_str(node->as.new_expr.module_name);
            n->as.new_expr.class_name = clone_str(node->as.new_expr.class_name);
            n->as.new_expr.arg_count = node->as.new_expr.arg_count;
            n->as.new_expr.arguments =
                clone_node_array(node->as.new_expr.arguments, node->as.new_expr.arg_count, map, mc);
            n->as.new_expr.type_args = clone_tref_array(node->as.new_expr.type_args,
                                                        node->as.new_expr.type_arg_count, map, mc);
            n->as.new_expr.type_arg_count = node->as.new_expr.type_arg_count;
            break;
        case AST_THIS_EXPR:
            break;

        // === Super call ===
        case AST_SUPER_CALL:
            n->as.super_call.method_name = clone_str(node->as.super_call.method_name);
            n->as.super_call.arg_count = node->as.super_call.arg_count;
            n->as.super_call.arguments = clone_node_array(node->as.super_call.arguments,
                                                          node->as.super_call.arg_count, map, mc);
            break;

        // === Match expression ===
        case AST_MATCH_EXPR:
            n->as.match_expr.expr = xr_ast_clone(node->as.match_expr.expr, map, mc);
            n->as.match_expr.arm_count = node->as.match_expr.arm_count;
            n->as.match_expr.arms =
                clone_node_array(node->as.match_expr.arms, node->as.match_expr.arm_count, map, mc);
            break;
        case AST_MATCH_ARM:
            n->as.match_arm.pattern = xr_ast_clone(node->as.match_arm.pattern, map, mc);
            n->as.match_arm.guard = xr_ast_clone(node->as.match_arm.guard, map, mc);
            n->as.match_arm.body = xr_ast_clone(node->as.match_arm.body, map, mc);
            break;
        case AST_CATCH_EXPR:
            n->as.catch_expr.body = xr_ast_clone(node->as.catch_expr.body, map, mc);
            break;

        // === Pattern nodes ===
        case AST_PATTERN_LITERAL:
            n->as.pattern_literal.value = xr_ast_clone(node->as.pattern_literal.value, map, mc);
            break;
        case AST_PATTERN_RANGE:
            n->as.pattern_range.start = xr_ast_clone(node->as.pattern_range.start, map, mc);
            n->as.pattern_range.end = xr_ast_clone(node->as.pattern_range.end, map, mc);
            break;
        case AST_PATTERN_WILDCARD:
            break;
        case AST_PATTERN_MULTI:
            n->as.pattern_multi.count = node->as.pattern_multi.count;
            n->as.pattern_multi.patterns = clone_node_array(node->as.pattern_multi.patterns,
                                                            node->as.pattern_multi.count, map, mc);
            break;
        case AST_PATTERN_TUPLE:
            n->as.pattern_tuple.count = node->as.pattern_tuple.count;
            n->as.pattern_tuple.patterns = clone_node_array(node->as.pattern_tuple.patterns,
                                                            node->as.pattern_tuple.count, map, mc);
            break;
        case AST_PATTERN_TYPE:
            n->as.pattern_type.type = node->as.pattern_type.type;
            n->as.pattern_type.binding_name = clone_str(node->as.pattern_type.binding_name);
            n->as.pattern_type.symbol_id = node->as.pattern_type.symbol_id;
            break;

        // === Coroutine nodes ===
        case AST_GO_EXPR:
            n->as.go_expr.expr = xr_ast_clone(node->as.go_expr.expr, map, mc);
            n->as.go_expr.name = clone_str(node->as.go_expr.name);
            n->as.go_expr.priority = xr_ast_clone(node->as.go_expr.priority, map, mc);
            n->as.go_expr.link_mode = node->as.go_expr.link_mode;
            break;
        case AST_AWAIT_EXPR:
            n->as.await_expr.expr = xr_ast_clone(node->as.await_expr.expr, map, mc);
            n->as.await_expr.timeout = xr_ast_clone(node->as.await_expr.timeout, map, mc);
            n->as.await_expr.is_any = node->as.await_expr.is_any;
            n->as.await_expr.is_all = node->as.await_expr.is_all;
            n->as.await_expr.is_any_success = node->as.await_expr.is_any_success;
            break;
        case AST_CHANNEL_NEW:
            n->as.channel_new.buffer_size = xr_ast_clone(node->as.channel_new.buffer_size, map, mc);
            break;
        case AST_DEFER_STMT:
            n->as.defer_stmt.expr = xr_ast_clone(node->as.defer_stmt.expr, map, mc);
            break;
        case AST_SCOPE_BLOCK:
            n->as.scope_block.body = xr_ast_clone(node->as.scope_block.body, map, mc);
            n->as.scope_block.scope_mode = node->as.scope_block.scope_mode;
            break;
        case AST_YIELD_STMT:
        case AST_CANCELLED_EXPR:
            break;

        // === Enum nodes ===
        case AST_ENUM_ACCESS:
            n->as.enum_access.enum_name = clone_str(node->as.enum_access.enum_name);
            n->as.enum_access.member_name = clone_str(node->as.enum_access.member_name);
            break;
        case AST_ENUM_CONVERT:
            n->as.enum_convert.enum_name = clone_str(node->as.enum_convert.enum_name);
            n->as.enum_convert.value_expr = xr_ast_clone(node->as.enum_convert.value_expr, map, mc);
            break;
        case AST_ENUM_INDEX:
            n->as.enum_index.collection = xr_ast_clone(node->as.enum_index.collection, map, mc);
            n->as.enum_index.index_expr = xr_ast_clone(node->as.enum_index.index_expr, map, mc);
            break;

        // === Class/struct declaration (deep clone for mono) ===
        case AST_STRUCT_DECL:
        case AST_CLASS_DECL: {
            ClassDeclNode *src =
                (node->type == AST_STRUCT_DECL) ? &node->as.struct_decl : &node->as.class_decl;
            ClassDeclNode *dst =
                (n->type == AST_STRUCT_DECL) ? &n->as.struct_decl : &n->as.class_decl;
            dst->name = clone_str(src->name);
            dst->super_name = clone_str(src->super_name);
            dst->super_module = clone_str(src->super_module);
            dst->interface_count = src->interface_count;
            dst->interfaces = clone_tref_array(src->interfaces, src->interface_count, map, mc);
            dst->field_count = src->field_count;
            dst->fields = clone_node_array(src->fields, src->field_count, map, mc);
            dst->method_count = src->method_count;
            dst->methods = clone_node_array(src->methods, src->method_count, map, mc);
            dst->is_abstract = src->is_abstract;
            dst->is_final = src->is_final;
            dst->type_params = NULL;  // Cleared: mono version has no type params
            dst->type_param_count = 0;
            break;
        }

        // === Method declaration (deep clone for mono) ===
        case AST_METHOD_DECL: {
            MethodDeclNode *src = &node->as.method_decl;
            MethodDeclNode *dst = &n->as.method_decl;
            dst->name = clone_str(src->name);
            dst->param_count = src->param_count;
            dst->parameters = clone_str_array(src->parameters, src->param_count);
            dst->param_types = clone_tref_array(src->param_types, src->param_count, map, mc);
            dst->return_type = sub_tref(src->return_type, map, mc);
            dst->body = xr_ast_clone(src->body, map, mc);
            dst->is_constructor = src->is_constructor;
            dst->is_static = src->is_static;
            dst->is_private = src->is_private;
            dst->is_getter = src->is_getter;
            dst->is_setter = src->is_setter;
            dst->is_abstract = src->is_abstract;
            dst->is_final = src->is_final;
            dst->is_static_constructor = src->is_static_constructor;
            dst->is_operator = src->is_operator;
            dst->op_type = src->op_type;
            dst->base_arg_count = src->base_arg_count;
            dst->base_args = clone_node_array(src->base_args, src->base_arg_count, map, mc);
            dst->default_values = clone_node_array(src->default_values, src->param_count, map, mc);
            dst->type_param_names = NULL;  // Cleared for mono
            dst->type_param_count = 0;
            break;
        }

        // === Field declaration (deep clone for mono) ===
        case AST_FIELD_DECL: {
            FieldDeclNode *src = &node->as.field_decl;
            FieldDeclNode *dst = &n->as.field_decl;
            dst->name = clone_str(src->name);
            dst->field_type = sub_tref(src->field_type, map, mc);
            dst->is_private = src->is_private;
            dst->is_static = src->is_static;
            dst->is_final = src->is_final;
            dst->initializer = xr_ast_clone(src->initializer, map, mc);
            break;
        }

        // === Struct literal (deep clone for mono) ===
        case AST_STRUCT_LITERAL: {
            StructLiteralNode *src = &node->as.struct_literal;
            StructLiteralNode *dst = &n->as.struct_literal;
            dst->struct_name = clone_str(src->struct_name);
            dst->field_count = src->field_count;
            dst->field_names = clone_str_array(src->field_names, src->field_count);
            dst->field_values = clone_node_array(src->field_values, src->field_count, map, mc);
            dst->type_args = clone_tref_array(src->type_args, src->type_arg_count, map, mc);
            dst->type_arg_count = src->type_arg_count;
            break;
        }

        // === Nodes not typically inside generic bodies (shallow copy) ===
        case AST_INTERFACE_DECL:
        case AST_ENUM_DECL:
        case AST_IMPORT_STMT:
        case AST_EXPORT_STMT:
        case AST_TYPE_ALIAS:
        case AST_PROGRAM:
        case AST_SELECT_STMT:
        case AST_SELECT_CASE:
        case AST_CHAN_SEND:
        case AST_CHAN_RECV:
        case AST_DESTRUCTURE_DECL:
        case AST_DESTRUCTURE_ASSIGN:
        case AST_INTERFACE_METHOD:
        case AST_INTERFACE_PROPERTY:
        case AST_ENUM_MEMBER:
        default:
            // Shallow copy union data for unsupported node types
            n->as = node->as;
            break;
    }
    return n;
}

/* ========== Mono Collector ========== */

void xa_mono_collector_init(XaMonoCollector *c) {
    XR_DCHECK(c != NULL, "xa_mono_collector_init: NULL collector");
    c->instances = NULL;
    c->count = 0;
    c->capacity = 0;
}

void xa_mono_collector_free(XaMonoCollector *c) {
    XR_DCHECK(c != NULL, "xa_mono_collector_free: NULL collector");
    for (int i = 0; i < c->count; i++) {
        xr_free((void *) c->instances[i].generic_name);
        xr_free((void *) c->instances[i].mangled_name);
    }
    xr_free(c->instances);
    c->instances = NULL;
    c->count = 0;
    c->capacity = 0;
}

/* Derive a slot-type category from XrTypeRef for rep-sharing dedup.
 * Returns a 4-bit value: distinguishes int/float/bool/string/ptr(ref). */
static uint8_t tref_slot_category(XrTypeRef *t) {
    if (!t)
        return XR_SLOT_ANY;
    switch ((XrTypeRefKind) t->kind) {
        case XR_TREF_INT:
        case XR_TREF_INT_WIDTH:
            return XR_SLOT_I64;
        case XR_TREF_FLOAT:
        case XR_TREF_FLOAT_WIDTH:
            return XR_SLOT_F64;
        case XR_TREF_BOOL:
            return XR_SLOT_BOOL;
        case XR_TREF_STRING:
            return XR_SLOT_PTR;
        case XR_TREF_NAMED:
        case XR_TREF_GENERIC:
        case XR_TREF_OPTIONAL:
        case XR_TREF_FUNCTION:
            return XR_SLOT_PTR;
        default:
            return XR_SLOT_ANY;
    }
}

// Compute slot-type signature for deduplication: combine slot categories.
// Distinguishes bool from int (BOOL=11 vs I64=7) for function generics.
static uint32_t compute_rep_signature(XrTypeRef **type_args, int count) {
    uint32_t sig = 0;
    for (int i = 0; i < count && i < 8; i++) {
        uint8_t st = tref_slot_category(type_args[i]);
        sig = (sig << 4) | (st & 0xF);
    }
    return sig;
}

const char *xa_mono_collector_add(XaMonoCollector *c, const char *generic_name,
                                  XrTypeRef **type_args, int type_arg_count,
                                  bool is_class_generic) {
    if (!c || !generic_name)
        return NULL;

    uint32_t rep_sig = compute_rep_signature(type_args, type_arg_count);

    /* Pre-compute mangled name for class/struct generics so we can
     * do exact-name dedup instead of rep-signature dedup. This avoids
     * conflating Box<string> and Box<MyClass> which share XR_SLOT_PTR. */
    char *candidate_mangled = NULL;
    if (is_class_generic) {
        candidate_mangled = xr_mono_mangle(generic_name, type_args, type_arg_count);
    }

    // Check for duplicate; class generics compare by mangled name,
    // function generics compare by rep-signature (allows rep-sharing).
    int per_generic = 0;
    for (int i = 0; i < c->count; i++) {
        if (strcmp(c->instances[i].generic_name, generic_name) == 0) {
            bool is_dup = false;
            if (is_class_generic && candidate_mangled) {
                is_dup = (strcmp(c->instances[i].mangled_name, candidate_mangled) == 0);
            } else {
                is_dup = (c->instances[i].rep_signature == rep_sig &&
                          c->instances[i].type_arg_count == type_arg_count);
            }
            if (is_dup) {
                xr_free(candidate_mangled);
                return c->instances[i].mangled_name;  // Already registered
            }
            per_generic++;
        }
    }

    // Safety: total instance limit
    if (c->count >= XR_MONO_MAX_INSTANCES) {
        xr_log_warning("mono",
                       "monomorphization limit reached (%d instances), "
                       "skipping %s",
                       XR_MONO_MAX_INSTANCES, generic_name);
        xr_free(candidate_mangled);
        return NULL;
    }

    // Safety: per-generic instance limit
    if (per_generic >= XR_MONO_MAX_PER_GENERIC) {
        xr_log_warning("mono", "too many instantiations of '%s' (%d), skipping", generic_name,
                       XR_MONO_MAX_PER_GENERIC);
        xr_free(candidate_mangled);
        return NULL;
    }

    // Grow if needed
    if (c->count >= c->capacity) {
        c->capacity = c->capacity ? c->capacity * 2 : 8;
        c->instances =
            (XaMonoInstance *) xr_realloc(c->instances, c->capacity * sizeof(XaMonoInstance));
    }

    XaMonoInstance *inst = &c->instances[c->count++];
    inst->generic_name = xr_strdup(generic_name);
    inst->type_args = type_args;
    inst->type_arg_count = type_arg_count;
    inst->mangled_name = candidate_mangled
                             ? candidate_mangled
                             : xr_mono_mangle(generic_name, type_args, type_arg_count);
    inst->rep_signature = rep_sig;
    inst->is_class_generic = is_class_generic;
    return inst->mangled_name;
}

// Lookup mangled name (same dedup logic as add: class generics by name,
// function generics by rep-signature).
static const char *xa_mono_collector_lookup(XaMonoCollector *c, const char *generic_name,
                                            XrTypeRef **type_args, int type_arg_count) {
    if (!c || !generic_name)
        return NULL;
    uint32_t rep_sig = compute_rep_signature(type_args, type_arg_count);
    char *candidate_mangled = xr_mono_mangle(generic_name, type_args, type_arg_count);
    const char *result = NULL;
    for (int i = 0; i < c->count; i++) {
        if (strcmp(c->instances[i].generic_name, generic_name) != 0)
            continue;
        if (c->instances[i].is_class_generic) {
            if (candidate_mangled && strcmp(c->instances[i].mangled_name, candidate_mangled) == 0) {
                result = c->instances[i].mangled_name;
                break;
            }
        } else {
            if (c->instances[i].rep_signature == rep_sig &&
                c->instances[i].type_arg_count == type_arg_count) {
                result = c->instances[i].mangled_name;
                break;
            }
        }
    }
    xr_free(candidate_mangled);
    return result;
}

/* ========== Mono Pass Collect + Instantiate + Rewrite ========== */

// Generic declaration registry: maps generic name → AST node
typedef struct {
    const char *name;
    AstNode *node;  // AST_FUNCTION_DECL or AST_CLASS_DECL
    XrGenericParam **type_params;
    int type_param_count;
} XaGenericDecl;

typedef struct {
    XaGenericDecl *decls;
    int count;
    int capacity;
} XaGenericRegistry;

static void registry_init(XaGenericRegistry *r) {
    r->decls = NULL;
    r->count = 0;
    r->capacity = 0;
}

static void registry_add(XaGenericRegistry *r, const char *name, AstNode *node, XrGenericParam **tp,
                         int tp_count) {
    if (r->count >= r->capacity) {
        int new_cap = r->capacity ? r->capacity * 2 : 8;
        XaGenericDecl *_new_r_decls =
            (XaGenericDecl *) xr_realloc(r->decls, new_cap * sizeof(XaGenericDecl));
        if (!_new_r_decls)
            return;
        r->decls = _new_r_decls;
        r->capacity = new_cap;
    }
    XaGenericDecl *d = &r->decls[r->count++];
    d->name = name;
    d->node = node;
    d->type_params = tp;
    d->type_param_count = tp_count;
}

static XaGenericDecl *registry_find(XaGenericRegistry *r, const char *name) {
    for (int i = 0; i < r->count; i++) {
        if (r->decls[i].name && name && strcmp(r->decls[i].name, name) == 0)
            return &r->decls[i];
    }
    return NULL;
}

// Phase 1: Collect generic function/class declarations from top-level program
static void collect_generic_decls(AstNode *root, XaGenericRegistry *registry) {
    if (!root)
        return;

    if (root->type == AST_PROGRAM) {
        ProgramNode *prog = &root->as.program;
        for (int i = 0; i < prog->count; i++) {
            AstNode *stmt = prog->statements[i];
            if (!stmt)
                continue;

            if (stmt->type == AST_FUNCTION_DECL && stmt->as.function_decl.type_param_count > 0) {
                registry_add(registry, stmt->as.function_decl.name, stmt,
                             stmt->as.function_decl.type_params,
                             stmt->as.function_decl.type_param_count);
            }
            // Generic class: class Box<T> { ... }
            if (stmt->type == AST_CLASS_DECL && stmt->as.class_decl.type_param_count > 0) {
                registry_add(registry, stmt->as.class_decl.name, stmt,
                             stmt->as.class_decl.type_params, stmt->as.class_decl.type_param_count);
            }
            // Generic struct: struct Pair<T, U> { ... }
            if (stmt->type == AST_STRUCT_DECL && stmt->as.struct_decl.type_param_count > 0) {
                registry_add(registry, stmt->as.struct_decl.name, stmt,
                             stmt->as.struct_decl.type_params,
                             stmt->as.struct_decl.type_param_count);
            }
            // Export wrapping: export fn/class ...
            if (stmt->type == AST_EXPORT_STMT && stmt->as.export_stmt.declaration) {
                AstNode *decl = stmt->as.export_stmt.declaration;
                if (decl->type == AST_FUNCTION_DECL &&
                    decl->as.function_decl.type_param_count > 0) {
                    registry_add(registry, decl->as.function_decl.name, decl,
                                 decl->as.function_decl.type_params,
                                 decl->as.function_decl.type_param_count);
                }
                if (decl->type == AST_CLASS_DECL && decl->as.class_decl.type_param_count > 0) {
                    registry_add(registry, decl->as.class_decl.name, decl,
                                 decl->as.class_decl.type_params,
                                 decl->as.class_decl.type_param_count);
                }
                if (decl->type == AST_STRUCT_DECL && decl->as.struct_decl.type_param_count > 0) {
                    registry_add(registry, decl->as.struct_decl.name, decl,
                                 decl->as.struct_decl.type_params,
                                 decl->as.struct_decl.type_param_count);
                }
            }
        }
    }
}

// Phase 2: Walk AST to find generic call sites (CallExpr with type_args)
static void collect_instantiation_sites(AstNode *node, XaGenericRegistry *registry,
                                        XaMonoCollector *collector) {
    if (!node)
        return;

    // Check call expression with explicit type arguments
    if (node->type == AST_CALL_EXPR) {
        CallExprNode *call = &node->as.call_expr;
        if (call->type_arg_count > 0 && call->callee && call->callee->type == AST_VARIABLE) {
            const char *fn_name = call->callee->as.variable.name;
            XaGenericDecl *decl = registry_find(registry, fn_name);
            if (decl && decl->type_param_count == call->type_arg_count) {
                bool is_cls =
                    (decl->node->type == AST_CLASS_DECL || decl->node->type == AST_STRUCT_DECL);
                xa_mono_collector_add(collector, fn_name, call->type_args, call->type_arg_count,
                                      is_cls);
            }
        }
        // Recurse into callee and arguments
        collect_instantiation_sites(call->callee, registry, collector);
        for (int i = 0; i < call->arg_count; i++)
            collect_instantiation_sites(call->arguments[i], registry, collector);
        return;
    }

    // Check new expression with type arguments
    if (node->type == AST_NEW_EXPR) {
        NewExprNode *ne = &node->as.new_expr;
        if (ne->type_arg_count > 0) {
            xa_mono_collector_add(collector, ne->class_name, ne->type_args, ne->type_arg_count,
                                  true);
        }
        for (int i = 0; i < ne->arg_count; i++)
            collect_instantiation_sites(ne->arguments[i], registry, collector);
        return;
    }

    // Check struct literal with type arguments: Pair<int, string>{...}
    if (node->type == AST_STRUCT_LITERAL) {
        StructLiteralNode *sl = &node->as.struct_literal;
        if (sl->type_arg_count > 0 && sl->struct_name) {
            XaGenericDecl *decl = registry_find(registry, sl->struct_name);
            if (decl && decl->type_param_count == sl->type_arg_count) {
                xa_mono_collector_add(collector, sl->struct_name, sl->type_args, sl->type_arg_count,
                                      true);
            }
        }
        for (int i = 0; i < sl->field_count; i++)
            collect_instantiation_sites(sl->field_values[i], registry, collector);
        return;
    }

    // Generic recursive walk for all other node types
    switch (node->type) {
        case AST_PROGRAM:
            for (int i = 0; i < node->as.program.count; i++)
                collect_instantiation_sites(node->as.program.statements[i], registry, collector);
            break;
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count; i++)
                collect_instantiation_sites(node->as.block.statements[i], registry, collector);
            break;
        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_MOD:
        case AST_BINARY_EQ:
        case AST_BINARY_NE:
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_BINARY_AND:
        case AST_BINARY_OR:
        case AST_BINARY_BAND:
        case AST_BINARY_BOR:
        case AST_BINARY_BXOR:
        case AST_BINARY_LSHIFT:
        case AST_BINARY_RSHIFT:
        case AST_BINARY_EQ_STRICT:
        case AST_BINARY_NE_STRICT:
        case AST_NULLISH_COALESCE:
            collect_instantiation_sites(node->as.binary.left, registry, collector);
            collect_instantiation_sites(node->as.binary.right, registry, collector);
            break;
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
        case AST_FORCE_UNWRAP:
        case AST_TRY_OPTIONAL:
        case AST_TRY_FORCE:
            collect_instantiation_sites(node->as.unary.operand, registry, collector);
            break;
        case AST_EXPR_STMT:
            collect_instantiation_sites(node->as.expr_stmt, registry, collector);
            break;
        case AST_GROUPING:
            collect_instantiation_sites(node->as.grouping, registry, collector);
            break;
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            collect_instantiation_sites(node->as.var_decl.initializer, registry, collector);
            break;
        case AST_ASSIGNMENT:
            collect_instantiation_sites(node->as.assignment.value, registry, collector);
            break;
        case AST_COMPOUND_ASSIGNMENT:
            collect_instantiation_sites(node->as.compound_assignment.value, registry, collector);
            collect_instantiation_sites(node->as.compound_assignment.object, registry, collector);
            break;
        case AST_IF_STMT:
            collect_instantiation_sites(node->as.if_stmt.condition, registry, collector);
            collect_instantiation_sites(node->as.if_stmt.then_branch, registry, collector);
            collect_instantiation_sites(node->as.if_stmt.else_branch, registry, collector);
            break;
        case AST_WHILE_STMT:
            collect_instantiation_sites(node->as.while_stmt.condition, registry, collector);
            collect_instantiation_sites(node->as.while_stmt.body, registry, collector);
            break;
        case AST_FOR_STMT:
            collect_instantiation_sites(node->as.for_stmt.initializer, registry, collector);
            collect_instantiation_sites(node->as.for_stmt.condition, registry, collector);
            collect_instantiation_sites(node->as.for_stmt.increment, registry, collector);
            collect_instantiation_sites(node->as.for_stmt.body, registry, collector);
            break;
        case AST_FOR_IN_STMT:
            collect_instantiation_sites(node->as.for_in_stmt.collection, registry, collector);
            collect_instantiation_sites(node->as.for_in_stmt.body, registry, collector);
            break;
        case AST_RETURN_STMT:
            for (int i = 0; i < node->as.return_stmt.value_count; i++)
                collect_instantiation_sites(node->as.return_stmt.values[i], registry, collector);
            break;
        case AST_FUNCTION_DECL:
        case AST_FUNCTION_EXPR:
            collect_instantiation_sites(node->as.function_decl.body, registry, collector);
            break;
        case AST_PRINT_STMT:
            for (int i = 0; i < node->as.print_stmt.expr_count; i++)
                collect_instantiation_sites(node->as.print_stmt.exprs[i], registry, collector);
            break;
        case AST_ARRAY_LITERAL:
            for (int i = 0; i < node->as.array_literal.count; i++)
                collect_instantiation_sites(node->as.array_literal.elements[i], registry,
                                            collector);
            break;
        case AST_INDEX_GET:
            collect_instantiation_sites(node->as.index_get.array, registry, collector);
            collect_instantiation_sites(node->as.index_get.index, registry, collector);
            break;
        case AST_INDEX_SET:
            collect_instantiation_sites(node->as.index_set.array, registry, collector);
            collect_instantiation_sites(node->as.index_set.index, registry, collector);
            collect_instantiation_sites(node->as.index_set.value, registry, collector);
            break;
        case AST_MEMBER_ACCESS:
            collect_instantiation_sites(node->as.member_access.object, registry, collector);
            break;
        case AST_MEMBER_SET:
            collect_instantiation_sites(node->as.member_set.object, registry, collector);
            collect_instantiation_sites(node->as.member_set.value, registry, collector);
            break;
        case AST_TERNARY:
            collect_instantiation_sites(node->as.ternary.condition, registry, collector);
            collect_instantiation_sites(node->as.ternary.true_expr, registry, collector);
            collect_instantiation_sites(node->as.ternary.false_expr, registry, collector);
            break;
        case AST_TEMPLATE_STRING:
            for (int i = 0; i < node->as.template_str.part_count; i++)
                collect_instantiation_sites(node->as.template_str.parts[i], registry, collector);
            break;
        case AST_TRY_CATCH:
            collect_instantiation_sites(node->as.try_catch.try_body, registry, collector);
            for (int ci = 0; ci < node->as.try_catch.catch_count; ci++) {
                XrCatchClause *cc = node->as.try_catch.catch_clauses[ci];
                if (cc)
                    collect_instantiation_sites(cc->body, registry, collector);
            }
            collect_instantiation_sites(node->as.try_catch.finally_body, registry, collector);
            break;
        case AST_THROW_STMT:
            collect_instantiation_sites(node->as.throw_stmt.expression, registry, collector);
            break;
        case AST_EXPORT_STMT:
            collect_instantiation_sites(node->as.export_stmt.declaration, registry, collector);
            break;
        case AST_MATCH_EXPR:
            collect_instantiation_sites(node->as.match_expr.expr, registry, collector);
            for (int i = 0; i < node->as.match_expr.arm_count; i++)
                collect_instantiation_sites(node->as.match_expr.arms[i], registry, collector);
            break;
        case AST_MATCH_ARM:
            collect_instantiation_sites(node->as.match_arm.guard, registry, collector);
            collect_instantiation_sites(node->as.match_arm.body, registry, collector);
            break;
        case AST_CATCH_EXPR:
            collect_instantiation_sites(node->as.catch_expr.body, registry, collector);
            break;
        case AST_IS_EXPR:
            collect_instantiation_sites(node->as.is_expr.expr, registry, collector);
            break;
        case AST_AS_EXPR:
            collect_instantiation_sites(node->as.as_expr.expr, registry, collector);
            break;
        case AST_GO_EXPR:
            collect_instantiation_sites(node->as.go_expr.expr, registry, collector);
            break;
        case AST_AWAIT_EXPR:
            collect_instantiation_sites(node->as.await_expr.expr, registry, collector);
            break;
        case AST_SCOPE_BLOCK:
            collect_instantiation_sites(node->as.scope_block.body, registry, collector);
            break;
        case AST_DEFER_STMT:
            collect_instantiation_sites(node->as.defer_stmt.expr, registry, collector);
            break;
        default:
            break;
    }
}

// Phase 3: Rewrite call sites — replace callee name with mangled name
static void rewrite_call_sites(AstNode *node, XaGenericRegistry *registry,
                               XaMonoCollector *collector) {
    if (!node)
        return;

    if (node->type == AST_CALL_EXPR) {
        CallExprNode *call = &node->as.call_expr;
        if (call->type_arg_count > 0 && call->callee && call->callee->type == AST_VARIABLE) {
            const char *fn_name = call->callee->as.variable.name;
            XaGenericDecl *decl = registry_find(registry, fn_name);
            if (decl) {
                // Lookup deduped mangled name via rep-signature matching
                const char *mangled = xa_mono_collector_lookup(collector, fn_name, call->type_args,
                                                               call->type_arg_count);
                if (mangled) {
                    // Replace callee variable name.
                    // Note: old name is arena-allocated, do not free.
                    call->callee->as.variable.name = xr_strdup(mangled);
                    // Clear type args (no longer generic call)
                    call->type_args = NULL;
                    call->type_arg_count = 0;
                }
            }
        }
        // Recurse
        rewrite_call_sites(call->callee, registry, collector);
        for (int i = 0; i < call->arg_count; i++)
            rewrite_call_sites(call->arguments[i], registry, collector);
        return;
    }

    // Rewrite new ClassName<T>(...) → new MangledName(...)
    if (node->type == AST_NEW_EXPR) {
        NewExprNode *ne = &node->as.new_expr;
        if (ne->type_arg_count > 0 && ne->class_name) {
            XaGenericDecl *decl = registry_find(registry, ne->class_name);
            if (decl) {
                const char *mangled = xa_mono_collector_lookup(collector, ne->class_name,
                                                               ne->type_args, ne->type_arg_count);
                if (mangled) {
                    // Old class_name is arena-allocated, do not free.
                    ne->class_name = xr_strdup(mangled);
                    // Keep type_args/type_arg_count for display and diagnostics.
                }
            }
        }
        for (int i = 0; i < ne->arg_count; i++)
            rewrite_call_sites(ne->arguments[i], registry, collector);
        return;
    }

    // Rewrite StructName<T>{...} → MangledName{...}
    if (node->type == AST_STRUCT_LITERAL) {
        StructLiteralNode *sl = &node->as.struct_literal;
        if (sl->type_arg_count > 0 && sl->struct_name) {
            XaGenericDecl *decl = registry_find(registry, sl->struct_name);
            if (decl) {
                const char *mangled = xa_mono_collector_lookup(collector, sl->struct_name,
                                                               sl->type_args, sl->type_arg_count);
                if (mangled) {
                    // Old struct_name is arena-allocated, do not free.
                    sl->struct_name = xr_strdup(mangled);
                    sl->type_args = NULL;
                    sl->type_arg_count = 0;
                }
            }
        }
        for (int i = 0; i < sl->field_count; i++)
            rewrite_call_sites(sl->field_values[i], registry, collector);
        return;
    }

    // Recursive walk (same structure as collect_instantiation_sites)
    switch (node->type) {
        case AST_PROGRAM:
            for (int i = 0; i < node->as.program.count; i++)
                rewrite_call_sites(node->as.program.statements[i], registry, collector);
            break;
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count; i++)
                rewrite_call_sites(node->as.block.statements[i], registry, collector);
            break;
        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_MOD:
        case AST_BINARY_EQ:
        case AST_BINARY_NE:
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_BINARY_AND:
        case AST_BINARY_OR:
        case AST_BINARY_BAND:
        case AST_BINARY_BOR:
        case AST_BINARY_BXOR:
        case AST_BINARY_LSHIFT:
        case AST_BINARY_RSHIFT:
        case AST_BINARY_EQ_STRICT:
        case AST_BINARY_NE_STRICT:
        case AST_NULLISH_COALESCE:
            rewrite_call_sites(node->as.binary.left, registry, collector);
            rewrite_call_sites(node->as.binary.right, registry, collector);
            break;
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
        case AST_FORCE_UNWRAP:
        case AST_TRY_OPTIONAL:
        case AST_TRY_FORCE:
            rewrite_call_sites(node->as.unary.operand, registry, collector);
            break;
        case AST_EXPR_STMT:
            rewrite_call_sites(node->as.expr_stmt, registry, collector);
            break;
        case AST_GROUPING:
            rewrite_call_sites(node->as.grouping, registry, collector);
            break;
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            rewrite_call_sites(node->as.var_decl.initializer, registry, collector);
            break;
        case AST_ASSIGNMENT:
            rewrite_call_sites(node->as.assignment.value, registry, collector);
            break;
        case AST_IF_STMT:
            rewrite_call_sites(node->as.if_stmt.condition, registry, collector);
            rewrite_call_sites(node->as.if_stmt.then_branch, registry, collector);
            rewrite_call_sites(node->as.if_stmt.else_branch, registry, collector);
            break;
        case AST_WHILE_STMT:
            rewrite_call_sites(node->as.while_stmt.condition, registry, collector);
            rewrite_call_sites(node->as.while_stmt.body, registry, collector);
            break;
        case AST_FOR_STMT:
            rewrite_call_sites(node->as.for_stmt.initializer, registry, collector);
            rewrite_call_sites(node->as.for_stmt.condition, registry, collector);
            rewrite_call_sites(node->as.for_stmt.increment, registry, collector);
            rewrite_call_sites(node->as.for_stmt.body, registry, collector);
            break;
        case AST_FOR_IN_STMT:
            rewrite_call_sites(node->as.for_in_stmt.collection, registry, collector);
            rewrite_call_sites(node->as.for_in_stmt.body, registry, collector);
            break;
        case AST_RETURN_STMT:
            for (int i = 0; i < node->as.return_stmt.value_count; i++)
                rewrite_call_sites(node->as.return_stmt.values[i], registry, collector);
            break;
        case AST_FUNCTION_DECL:
        case AST_FUNCTION_EXPR:
            rewrite_call_sites(node->as.function_decl.body, registry, collector);
            break;
        case AST_PRINT_STMT:
            for (int i = 0; i < node->as.print_stmt.expr_count; i++)
                rewrite_call_sites(node->as.print_stmt.exprs[i], registry, collector);
            break;
        case AST_TRY_CATCH:
            rewrite_call_sites(node->as.try_catch.try_body, registry, collector);
            for (int ci = 0; ci < node->as.try_catch.catch_count; ci++) {
                XrCatchClause *cc = node->as.try_catch.catch_clauses[ci];
                if (cc)
                    rewrite_call_sites(cc->body, registry, collector);
            }
            rewrite_call_sites(node->as.try_catch.finally_body, registry, collector);
            break;
        case AST_THROW_STMT:
            rewrite_call_sites(node->as.throw_stmt.expression, registry, collector);
            break;
        case AST_EXPORT_STMT:
            rewrite_call_sites(node->as.export_stmt.declaration, registry, collector);
            break;
        case AST_MATCH_EXPR:
            rewrite_call_sites(node->as.match_expr.expr, registry, collector);
            for (int i = 0; i < node->as.match_expr.arm_count; i++)
                rewrite_call_sites(node->as.match_expr.arms[i], registry, collector);
            break;
        case AST_MATCH_ARM:
            rewrite_call_sites(node->as.match_arm.guard, registry, collector);
            rewrite_call_sites(node->as.match_arm.body, registry, collector);
            break;
        case AST_CATCH_EXPR:
            rewrite_call_sites(node->as.catch_expr.body, registry, collector);
            break;
        case AST_GO_EXPR:
            rewrite_call_sites(node->as.go_expr.expr, registry, collector);
            break;
        case AST_AWAIT_EXPR:
            rewrite_call_sites(node->as.await_expr.expr, registry, collector);
            break;
        case AST_SCOPE_BLOCK:
            rewrite_call_sites(node->as.scope_block.body, registry, collector);
            break;
        case AST_DEFER_STMT:
            rewrite_call_sites(node->as.defer_stmt.expr, registry, collector);
            break;
        default:
            break;
    }
}

// Inject monomorphized function declarations into the program AST
static void inject_mono_decls(AstNode *root, XaGenericRegistry *registry,
                              XaMonoCollector *collector) {
    if (!root || root->type != AST_PROGRAM || collector->count == 0)
        return;

    ProgramNode *prog = &root->as.program;

    // prog->statements starts as arena-allocated (from xr_ast_program_add).
    // Once we copy it to the heap for growth, heap_owned becomes true and
    // subsequent grows can safely xr_free the old buffer.
    bool heap_owned = false;

    for (int i = 0; i < collector->count; i++) {
        XaMonoInstance *inst = &collector->instances[i];
        XaGenericDecl *decl = registry_find(registry, inst->generic_name);
        if (!decl || !decl->node)
            continue;

        // Build type map from generic params → concrete types
        int map_count = decl->type_param_count;
        if (map_count > inst->type_arg_count)
            map_count = inst->type_arg_count;

        XrMonoTypeMap *map = (XrMonoTypeMap *) xr_calloc(map_count, sizeof(XrMonoTypeMap));
        if (!map)
            continue;
        for (int j = 0; j < map_count; j++) {
            map[j].param_name = decl->type_params[j]->name;
            map[j].concrete_type = inst->type_args[j];
        }

        // Clone the generic function with type substitution
        AstNode *cloned = xr_ast_clone(decl->node, map, map_count);
        xr_free(map);

        if (!cloned)
            continue;

        // Rename cloned function/class to mangled name
        if (cloned->type == AST_FUNCTION_DECL) {
            xr_free(cloned->as.function_decl.name);
            cloned->as.function_decl.name = xr_strdup(inst->mangled_name);
            cloned->as.function_decl.type_param_count = 0;
            cloned->as.function_decl.type_params = NULL;
        } else if (cloned->type == AST_CLASS_DECL) {
            xr_free(cloned->as.class_decl.name);
            cloned->as.class_decl.name = xr_strdup(inst->mangled_name);
            cloned->as.class_decl.type_param_count = 0;
            cloned->as.class_decl.type_params = NULL;
            cloned->as.class_decl.is_monomorphized = true;
            cloned->as.class_decl.generic_origin_name = xr_strdup(inst->generic_name);
            cloned->as.class_decl.display_name = xr_strdup(inst->generic_name);
            /* Store concrete type arg display names for Reflect.typeOf */
            if (inst->type_arg_count > 0 && inst->type_args) {
                const char **names =
                    (const char **) xr_calloc(inst->type_arg_count, sizeof(const char *));
                if (names) {
                    for (int ti = 0; ti < inst->type_arg_count; ti++)
                        names[ti] = mono_type_display_name(inst->type_args[ti]);
                    cloned->as.class_decl.mono_type_arg_names = names;
                    cloned->as.class_decl.mono_type_arg_count = inst->type_arg_count;
                }
            }
            if (decl->node && decl->node->type == AST_CLASS_DECL) {
                decl->node->as.class_decl.is_generic_skeleton = true;
            }
        } else if (cloned->type == AST_STRUCT_DECL) {
            xr_free(cloned->as.struct_decl.name);
            cloned->as.struct_decl.name = xr_strdup(inst->mangled_name);
            cloned->as.struct_decl.type_param_count = 0;
            cloned->as.struct_decl.type_params = NULL;
            cloned->as.struct_decl.is_monomorphized = true;
            cloned->as.struct_decl.generic_origin_name = xr_strdup(inst->generic_name);
            cloned->as.struct_decl.display_name = xr_strdup(inst->generic_name);
            if (inst->type_arg_count > 0 && inst->type_args) {
                const char **names =
                    (const char **) xr_calloc(inst->type_arg_count, sizeof(const char *));
                if (names) {
                    for (int ti = 0; ti < inst->type_arg_count; ti++)
                        names[ti] = mono_type_display_name(inst->type_args[ti]);
                    cloned->as.struct_decl.mono_type_arg_names = names;
                    cloned->as.struct_decl.mono_type_arg_count = inst->type_arg_count;
                }
            }
            if (decl->node && decl->node->type == AST_STRUCT_DECL) {
                decl->node->as.struct_decl.is_generic_skeleton = true;
            }
        }

        // Find the position of the original generic declaration so we insert
        // the monomorphized clone right after it. This ensures the specialized
        // class/function is defined before any call site that uses it.
        int insert_pos = prog->count;  // fallback: append
        for (int j = 0; j < prog->count; j++) {
            AstNode *sj = prog->statements[j];
            if (!sj)
                continue;
            // Unwrap export wrapper
            if (sj->type == AST_EXPORT_STMT && sj->as.export_stmt.declaration)
                sj = sj->as.export_stmt.declaration;
            if (sj == decl->node) {
                insert_pos = j + 1;
                break;
            }
        }

        // Grow array if needed. The initial prog->statements is arena-
        // allocated by the parser; we must not xr_free/xr_realloc it.
        // On the first overflow, allocate a heap buffer and memcpy.
        // After that, normal xr_realloc is safe.
        if (prog->count >= prog->capacity) {
            int new_cap = prog->capacity ? prog->capacity * 2 : (prog->count + 16);
            AstNode **new_buf = (AstNode **) xr_malloc((size_t) new_cap * sizeof(AstNode *));
            if (!new_buf)
                continue;
            if (prog->statements && prog->count > 0)
                memcpy(new_buf, prog->statements, (size_t) prog->count * sizeof(AstNode *));
            if (heap_owned)
                xr_free(prog->statements);
            prog->statements = new_buf;
            prog->capacity = new_cap;
            heap_owned = true;
        }
        // Shift statements after insert_pos to make room
        if (insert_pos < prog->count) {
            memmove(&prog->statements[insert_pos + 1], &prog->statements[insert_pos],
                    (size_t) (prog->count - insert_pos) * sizeof(AstNode *));
        }
        prog->statements[insert_pos] = cloned;
        prog->count++;
    }
}

/* ========== Public API ========== */

void xa_mono_pass(AstNode *root) {
    if (!root || root->type != AST_PROGRAM)
        return;

    XaGenericRegistry registry;
    registry_init(&registry);

    XaMonoCollector collector;
    xa_mono_collector_init(&collector);

    // Phase 1: Collect generic declarations
    collect_generic_decls(root, &registry);
    if (registry.count == 0)
        goto cleanup;

    // Phase 2: Collect instantiation sites
    collect_instantiation_sites(root, &registry, &collector);
    if (collector.count == 0)
        goto cleanup;

    // Phase 3: Clone + substitute + inject monomorphized versions
    inject_mono_decls(root, &registry, &collector);

    // Phase 4: Rewrite call sites to use mangled names
    rewrite_call_sites(root, &registry, &collector);

    // Debug: print mono stats if XRAY_MONO_DEBUG is set
    if (getenv("XRAY_MONO_DEBUG")) {
        xr_log_debug("mono", "%d generic decls, %d mono instances", registry.count,
                     collector.count);
        for (int i = 0; i < collector.count; i++) {
            XaMonoInstance *inst = &collector.instances[i];
            xr_log_debug("mono", "  %s -> %s (rep_sig=0x%08x)", inst->generic_name,
                         inst->mangled_name, inst->rep_signature);
        }
    }

cleanup:
    xa_mono_collector_free(&collector);
    xr_free(registry.decls);
}
