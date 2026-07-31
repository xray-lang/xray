/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xast_walk.c - Generic AST traversal and canonical node signatures
 *
 * KEY CONCEPT:
 *   Two switches over AstNodeType, and only two:
 *
 *     walk_children()  — every AST child slot, in source order.
 *     write_payload()  — every field that is NOT an AST child.
 *
 *   Together they describe an AST node completely. Both end in a `default:`
 *   that reports "unknown", so a node type added to xast_types.h without being
 *   taught here fails loudly at the first consumer instead of being silently
 *   walked as a leaf. See xast_walk.h for what is deliberately excluded.
 */

#include "xast_walk.h"
#include "xast_api.h"
#include "xtype_ref.h"
#include "../../base/xchecks.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ====================================================================== */
/* Child traversal                                                         */
/* ====================================================================== */

// Visit state threaded through the walk so a visitor can stop it early.
typedef struct {
    XrAstChildFn fn;
    void *user_data;
    bool stopped;
} ChildWalk;

static void emit(ChildWalk *w, AstNode *child) {
    if (w->stopped || !w->fn)
        return;
    if (!w->fn(child, w->user_data))
        w->stopped = true;
}

static void emit_array(ChildWalk *w, AstNode **nodes, int count) {
    for (int i = 0; i < count && !w->stopped; i++)
        emit(w, nodes ? nodes[i] : NULL);
}

// Parameter lists carry default-value expressions, which are real children.
static void emit_params(ChildWalk *w, XrParamNode **params, int count) {
    for (int i = 0; i < count && !w->stopped; i++)
        emit(w, params && params[i] ? params[i]->default_value : NULL);
}

// A fixed-array type annotation may carry a length expression.
static void emit_type_ref(ChildWalk *w, const XrTypeRef *ref) {
    if (!ref || w->stopped)
        return;
    if (ref->fixed_length_expr)
        emit(w, ref->fixed_length_expr);
    for (int i = 0; i < (int) ref->nchildren && !w->stopped; i++)
        emit_type_ref(w, ref->children ? ref->children[i] : NULL);
}

static void emit_type_refs(ChildWalk *w, XrTypeRef **refs, int count) {
    for (int i = 0; i < count && !w->stopped; i++)
        emit_type_ref(w, refs ? refs[i] : NULL);
}

// Returns false only for an unrecognised node type.
static bool walk_children(const AstNode *n, ChildWalk *w) {
    switch (n->type) {
        /* ---- Leaves: no AST children ---- */
        case AST_LITERAL_INT:
        case AST_LITERAL_FLOAT:
        case AST_LITERAL_BIGINT:
        case AST_LITERAL_STRING:
        case AST_LITERAL_RUNE:
        case AST_LITERAL_REGEX:
        case AST_LITERAL_NULL:
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
        case AST_FIXED_BYTES_LITERAL:
        case AST_VARIABLE:
        case AST_INC:
        case AST_DEC:
        case AST_BREAK_STMT:
        case AST_CONTINUE_STMT:
        case AST_THIS_EXPR:
        case AST_ENUM_ACCESS:
        case AST_IMPORT_STMT:
        case AST_GLOBAL_ASM:
        case AST_PATTERN_WILDCARD:
        case AST_CANCELLED_EXPR:
            return true;

        /* ---- Unary / binary shapes ---- */
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
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_BINARY_AND:
        case AST_BINARY_OR:
        case AST_NULLISH_COALESCE:
            emit(w, n->as.binary.left);
            emit(w, n->as.binary.right);
            return true;

        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
        case AST_FORCE_UNWRAP:
            emit(w, n->as.unary.operand);
            return true;

        case AST_GROUPING:
            emit(w, n->as.grouping);
            return true;
        case AST_EXPR_STMT:
            emit(w, n->as.expr_stmt);
            return true;
        case AST_SPREAD_EXPR:
            emit(w, n->as.spread_expr.expr);
            return true;
        case AST_COMPTIME_EXPR:
            emit(w, n->as.comptime_expr.expr);
            return true;
        case AST_MOVE_EXPR:
            emit(w, n->as.move_expr.expr);
            return true;
        case AST_UNSAFE_EXPR:
            emit(w, n->as.unsafe_expr.operand);
            return true;
        case AST_THROW_STMT:
            emit(w, n->as.throw_stmt.expression);
            return true;
        case AST_DEFER_STMT:
            emit(w, n->as.defer_stmt.expr);
            return true;
        case AST_YIELD_STMT:
            emit(w, n->as.yield_stmt.value);
            return true;
        case AST_CHANNEL_NEW:
            emit(w, n->as.channel_new.buffer_size);
            return true;
        case AST_PATTERN_LITERAL:
            emit(w, n->as.pattern_literal.value);
            return true;

        /* ---- Statement sequences ---- */
        case AST_PROGRAM:
            emit_array(w, n->as.program.statements, n->as.program.count);
            return true;
        case AST_BLOCK:
            emit_array(w, n->as.block.statements, n->as.block.count);
            return true;
        case AST_PRINT_STMT:
            emit_array(w, n->as.print_stmt.exprs, n->as.print_stmt.expr_count);
            return true;
        case AST_RETURN_STMT:
            emit_array(w, n->as.return_stmt.values, n->as.return_stmt.value_count);
            return true;

        /* ---- Bindings and assignment ---- */
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            emit_type_ref(w, n->as.var_decl.type_annotation);
            emit(w, n->as.var_decl.initializer);
            return true;
        case AST_ASSIGNMENT:
            emit(w, n->as.assignment.value);
            return true;
        case AST_COMPOUND_ASSIGNMENT:
            emit(w, n->as.compound_assignment.object);
            emit(w, n->as.compound_assignment.value);
            return true;
        case AST_DESTRUCTURE_DECL:
            emit(w, n->as.destructure_decl.initializer);
            return true;
        case AST_DESTRUCTURE_ASSIGN:
            emit(w, n->as.destructure_assign.value);
            return true;

        /* ---- Control flow ---- */
        case AST_IF_STMT:
            emit(w, n->as.if_stmt.condition);
            emit(w, n->as.if_stmt.then_branch);
            emit(w, n->as.if_stmt.else_branch);
            return true;
        case AST_WHILE_STMT:
            emit(w, n->as.while_stmt.condition);
            emit(w, n->as.while_stmt.body);
            return true;
        case AST_FOR_STMT:
            emit(w, n->as.for_stmt.initializer);
            emit(w, n->as.for_stmt.condition);
            emit(w, n->as.for_stmt.increment);
            emit(w, n->as.for_stmt.body);
            return true;
        case AST_FOR_IN_STMT:
            emit_type_ref(w, n->as.for_in_stmt.item_type);
            emit(w, n->as.for_in_stmt.collection);
            emit(w, n->as.for_in_stmt.body);
            return true;

        /* ---- Exceptions ---- */
        case AST_TRY_CATCH: {
            emit(w, n->as.try_catch.try_body);
            for (int i = 0; i < n->as.try_catch.catch_count && !w->stopped; i++) {
                const XrCatchClause *c =
                    n->as.try_catch.catch_clauses ? n->as.try_catch.catch_clauses[i] : NULL;
                emit_type_ref(w, c ? c->type : NULL);
                emit(w, c ? c->pattern : NULL);
                emit(w, c ? c->body : NULL);
            }
            return true;
        }

        /* ---- Calls, access, aggregates ---- */
        case AST_CALL_EXPR:
            emit(w, n->as.call_expr.callee);
            emit_type_refs(w, n->as.call_expr.type_args, n->as.call_expr.type_arg_count);
            emit_array(w, n->as.call_expr.arguments, n->as.call_expr.arg_count);
            return true;
        case AST_NEW_EXPR:
            emit_type_refs(w, n->as.new_expr.type_args, n->as.new_expr.type_arg_count);
            emit_array(w, n->as.new_expr.arguments, n->as.new_expr.arg_count);
            return true;
        case AST_SUPER_CALL:
            emit_array(w, n->as.super_call.arguments, n->as.super_call.arg_count);
            return true;
        case AST_MEMBER_ACCESS:
            emit(w, n->as.member_access.object);
            return true;
        case AST_MEMBER_SET:
            emit(w, n->as.member_set.object);
            emit(w, n->as.member_set.value);
            return true;
        case AST_INDEX_GET:
            emit(w, n->as.index_get.array);
            emit(w, n->as.index_get.index);
            return true;
        case AST_INDEX_SET:
            emit(w, n->as.index_set.array);
            emit(w, n->as.index_set.index);
            emit(w, n->as.index_set.value);
            return true;
        case AST_SLICE_EXPR:
            emit(w, n->as.slice_expr.source);
            emit(w, n->as.slice_expr.start);
            emit(w, n->as.slice_expr.end);
            return true;
        case AST_ARRAY_LITERAL:
            emit_array(w, n->as.array_literal.elements, n->as.array_literal.count);
            emit(w, n->as.array_literal.repeat_value);
            emit(w, n->as.array_literal.repeat_count);
            return true;
        case AST_TUPLE_LITERAL:
            emit_array(w, n->as.tuple_literal.elements, n->as.tuple_literal.count);
            return true;
        case AST_SET_LITERAL:
            emit_array(w, n->as.set_literal.elements, n->as.set_literal.count);
            return true;
        case AST_OBJECT_LITERAL:
            for (int i = 0; i < n->as.object_literal.count && !w->stopped; i++) {
                emit(w, n->as.object_literal.keys ? n->as.object_literal.keys[i] : NULL);
                emit(w, n->as.object_literal.values ? n->as.object_literal.values[i] : NULL);
            }
            return true;
        case AST_MAP_LITERAL:
            for (int i = 0; i < n->as.map_literal.count && !w->stopped; i++) {
                emit(w, n->as.map_literal.keys ? n->as.map_literal.keys[i] : NULL);
                emit(w, n->as.map_literal.values ? n->as.map_literal.values[i] : NULL);
            }
            return true;
        case AST_STRUCT_LITERAL:
            emit_type_refs(w, n->as.struct_literal.type_args, n->as.struct_literal.type_arg_count);
            emit_array(w, n->as.struct_literal.field_values, n->as.struct_literal.field_count);
            return true;
        case AST_TEMPLATE_STRING:
            emit_array(w, n->as.template_str.parts, n->as.template_str.part_count);
            return true;

        /* ---- Other operators ---- */
        case AST_TERNARY:
            emit(w, n->as.ternary.condition);
            emit(w, n->as.ternary.true_expr);
            emit(w, n->as.ternary.false_expr);
            return true;
        case AST_OPTIONAL_CHAIN:
            emit(w, n->as.optional_chain.object);
            emit(w, n->as.optional_chain.index);
            return true;
        case AST_RANGE:
            emit(w, n->as.range.start);
            emit(w, n->as.range.end);
            return true;
        case AST_IS_EXPR:
            emit(w, n->as.is_expr.expr);
            emit_type_ref(w, n->as.is_expr.type);
            return true;
        case AST_AS_EXPR:
            emit(w, n->as.as_expr.expr);
            emit_type_ref(w, n->as.as_expr.type);
            return true;

        /* ---- Functions and OOP ---- */
        case AST_FUNCTION_DECL:
        case AST_FUNCTION_EXPR:
            emit_params(w, n->as.function_decl.params, n->as.function_decl.param_count);
            emit_type_ref(w, n->as.function_decl.return_type);
            emit_type_refs(w, n->as.function_decl.throws_types, n->as.function_decl.throws_count);
            emit(w, n->as.function_decl.body);
            return true;
        case AST_CLASS_DECL:
        case AST_STRUCT_DECL:
        case AST_UNION_DECL:
            emit_type_refs(w, n->as.class_decl.interfaces, n->as.class_decl.interface_count);
            emit_array(w, n->as.class_decl.fields, n->as.class_decl.field_count);
            emit_array(w, n->as.class_decl.methods, n->as.class_decl.method_count);
            return true;
        case AST_FIELD_DECL:
            emit_type_ref(w, n->as.field_decl.field_type);
            emit(w, n->as.field_decl.initializer);
            return true;
        case AST_METHOD_DECL:
            emit_params(w, n->as.method_decl.params, n->as.method_decl.param_count);
            emit_type_ref(w, n->as.method_decl.return_type);
            emit_array(w, n->as.method_decl.base_args, n->as.method_decl.base_arg_count);
            emit(w, n->as.method_decl.body);
            return true;
        case AST_INTERFACE_DECL:
            emit_type_refs(w, n->as.interface_decl.extends, n->as.interface_decl.extends_count);
            emit_array(w, n->as.interface_decl.methods, n->as.interface_decl.method_count);
            emit_array(w, n->as.interface_decl.properties, n->as.interface_decl.property_count);
            return true;
        case AST_INTERFACE_METHOD:
            emit_params(w, n->as.interface_method.params, n->as.interface_method.param_count);
            emit_type_ref(w, n->as.interface_method.return_type);
            return true;
        case AST_INTERFACE_PROPERTY:
            emit_type_ref(w, n->as.interface_property.prop_type);
            return true;

        /* ---- Enums ---- */
        case AST_ENUM_DECL:
            emit_type_refs(w, n->as.enum_decl.interfaces, n->as.enum_decl.interface_count);
            emit_array(w, n->as.enum_decl.members, n->as.enum_decl.member_count);
            emit_array(w, n->as.enum_decl.methods, n->as.enum_decl.method_count);
            return true;
        case AST_ENUM_MEMBER:
            emit_type_refs(w, n->as.enum_member.payload_types, n->as.enum_member.payload_count);
            return true;
        case AST_ENUM_INDEX:
            emit(w, n->as.enum_index.collection);
            emit(w, n->as.enum_index.index_expr);
            return true;

        /* ---- Modules ---- */
        case AST_EXPORT_STMT:
            return true;
        case AST_TYPE_ALIAS:
            emit_type_refs(w, n->as.type_alias.field_types, n->as.type_alias.field_count);
            emit_type_ref(w, n->as.type_alias.resolved_type);
            return true;

        /* ---- Match and patterns ---- */
        case AST_MATCH_EXPR:
            emit(w, n->as.match_expr.expr);
            emit_array(w, n->as.match_expr.arms, n->as.match_expr.arm_count);
            return true;
        case AST_MATCH_ARM:
            emit(w, n->as.match_arm.pattern);
            emit(w, n->as.match_arm.guard);
            emit(w, n->as.match_arm.body);
            return true;
        case AST_PATTERN_RANGE:
            emit(w, n->as.pattern_range.start);
            emit(w, n->as.pattern_range.end);
            return true;
        case AST_PATTERN_MULTI:
            emit_array(w, n->as.pattern_multi.patterns, n->as.pattern_multi.count);
            return true;
        case AST_PATTERN_TUPLE:
            emit_array(w, n->as.pattern_tuple.patterns, n->as.pattern_tuple.count);
            return true;
        case AST_PATTERN_OBJECT:
            emit_array(w, n->as.pattern_object.patterns, n->as.pattern_object.count);
            return true;
        case AST_PATTERN_ARRAY:
            emit_array(w, n->as.pattern_array.patterns, n->as.pattern_array.count);
            return true;
        case AST_PATTERN_ADT:
            emit(w, n->as.pattern_adt.variant);
            emit_array(w, n->as.pattern_adt.patterns, n->as.pattern_adt.count);
            return true;
        case AST_PATTERN_TYPE:
            emit_type_ref(w, n->as.pattern_type.type);
            return true;

        /* ---- Coroutines ---- */
        case AST_GO_EXPR:
            emit(w, n->as.go_expr.expr);
            return true;
        case AST_AWAIT_EXPR:
            emit(w, n->as.await_expr.expr);
            emit(w, n->as.await_expr.timeout);
            emit(w, n->as.await_expr.into);
            return true;
        case AST_SELECT_STMT:
            emit_array(w, n->as.select_stmt.cases, n->as.select_stmt.case_count);
            return true;
        case AST_SELECT_CASE:
            emit(w, n->as.select_case.channel);
            emit(w, n->as.select_case.value);
            emit(w, n->as.select_case.body);
            return true;
        case AST_SCOPE_BLOCK:
            emit(w, n->as.scope_block.body);
            return true;

        default:
            return false;
    }
}

bool xr_ast_for_each_child(const AstNode *node, XrAstChildFn fn, void *user_data) {
    if (!node)
        return false;
    ChildWalk w = {.fn = fn, .user_data = user_data, .stopped = false};
    if (!walk_children(node, &w))
        return false;
    return !w.stopped;
}

bool xr_ast_node_is_known(const AstNode *node) {
    if (!node)
        return false;
    ChildWalk w = {.fn = NULL, .user_data = NULL, .stopped = false};
    return walk_children(node, &w);
}

/* ====================================================================== */
/* Canonical payload signature                                             */
/* ====================================================================== */

// Bounded appender: every writer below goes through this, so an overflow is
// detected once instead of at ~90 call sites.
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool overflow;
} SigBuf;

static void sig_add(SigBuf *s, const char *fmt, ...) XR_PRINTF_FMT(2, 3);

static void sig_add(SigBuf *s, const char *fmt, ...) {
    if (s->overflow || s->len >= s->cap)
        return;
    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(s->buf + s->len, s->cap - s->len, fmt, ap);
    va_end(ap);
    if (written < 0 || (size_t) written >= s->cap - s->len) {
        s->overflow = true;
        return;
    }
    s->len += (size_t) written;
}

// NULL and "" must not collide: a dropped name is a real difference.
static void sig_name(SigBuf *s, const char *label, const char *name) {
    sig_add(s, " %s=%s", label, name ? name : "<null>");
}

static void sig_type_ref(SigBuf *s, const XrTypeRef *ref) {
    if (!ref) {
        sig_add(s, "<null>");
        return;
    }
    sig_add(s, "T%u", (unsigned) ref->kind);
    if (ref->name)
        sig_add(s, ":%s", ref->name);
    if (ref->scalar_rep)
        sig_add(s, "/s%u", (unsigned) ref->scalar_rep);
    if (ref->builtin_spelling)
        sig_add(s, "/b%u", (unsigned) ref->builtin_spelling);
    if (ref->extensible)
        sig_add(s, "/ext");
    if (ref->requires_nothrow)
        sig_add(s, "/nothrow");
    if (ref->fixed_length)
        sig_add(s, "/len%d", ref->fixed_length);
    for (int i = 0; i < (int) ref->nchildren; i++) {
        sig_add(s, i == 0 ? "<" : ",");
        sig_type_ref(s, ref->children ? ref->children[i] : NULL);
    }
    if (ref->nchildren)
        sig_add(s, ">");
}

static void sig_params(SigBuf *s, XrParamNode **params, int count) {
    sig_add(s, " params[%d]", count);
    for (int i = 0; i < count; i++) {
        const XrParamNode *p = params ? params[i] : NULL;
        sig_add(s, " (");
        sig_add(s, "%s", p && p->name ? p->name : "<null>");
        sig_add(s, ",mode%u,rest%d,", (unsigned) (p ? p->passing_mode : 0),
                p && p->is_rest ? 1 : 0);
        sig_type_ref(s, p ? p->type : NULL);
        sig_add(s, ")");
    }
}

static void sig_attributes(SigBuf *s, XrAttribute **attrs, int count) {
    if (count <= 0)
        return;
    sig_add(s, " attrs[%d]", count);
    for (int i = 0; i < count; i++) {
        const XrAttribute *a = attrs ? attrs[i] : NULL;
        sig_add(s, " (k%u,t%d,d%u,%s)", (unsigned) (a ? a->kind : 0), a ? a->timeout : 0,
                (unsigned) (a ? a->derive_flags : 0), a && a->str_arg ? a->str_arg : "");
    }
}

static void sig_generic_params(SigBuf *s, XrGenericParam **tps, int count) {
    if (count <= 0)
        return;
    sig_add(s, " tparams[%d]", count);
    for (int i = 0; i < count; i++) {
        const XrGenericParam *g = tps ? tps[i] : NULL;
        sig_add(s, " %s{", g && g->name ? g->name : "<null>");
        for (int c = 0; g && c < g->constraint_count; c++) {
            sig_add(s, c == 0 ? "" : ",");
            sig_type_ref(s, g->constraints ? g->constraints[c] : NULL);
        }
        sig_add(s, "}");
    }
}

static void sig_destructure(SigBuf *s, const XrDestructurePattern *p) {
    if (!p) {
        sig_add(s, "<null>");
        return;
    }
    sig_add(s, "P%u", (unsigned) p->type);
    switch (p->type) {
        // PATTERN_TUPLE shares the `array` union arm — positional elements,
        // different surface syntax. Reading it as `identifier` would walk a
        // count as if it were a pointer.
        case PATTERN_ARRAY:
        case PATTERN_TUPLE:
            sig_add(s, "[%d]", p->as.array.element_count);
            for (int i = 0; i < p->as.array.element_count; i++)
                sig_destructure(s, p->as.array.elements ? p->as.array.elements[i] : NULL);
            break;
        case PATTERN_SKIP:
            break;
        case PATTERN_OBJECT:
            sig_add(s, "{%d,sh%d}", p->as.object.field_count, p->as.object.use_shorthand ? 1 : 0);
            for (int i = 0; i < p->as.object.field_count; i++) {
                sig_add(s, " %s:",
                        p->as.object.field_names && p->as.object.field_names[i]
                            ? p->as.object.field_names[i]
                            : "<null>");
                sig_destructure(s, p->as.object.patterns ? p->as.object.patterns[i] : NULL);
            }
            break;
        case PATTERN_IDENTIFIER:
            sig_name(s, "id", p->as.identifier.name);
            sig_add(s, ":");
            sig_type_ref(s, p->as.identifier.type);
            break;
        default:
            // Fail-closed: an unrecognised pattern kind must not be read
            // through an arbitrary union arm.
            sig_add(s, "<unknown-pattern>");
            break;
    }
}

// Returns false only for an unrecognised node type.
static bool write_payload(const AstNode *n, SigBuf *s) {
    switch (n->type) {
        /* ---- Literals: the value IS the payload ---- */
        case AST_LITERAL_INT:
            sig_add(s, " bits=%llu ovf=%d", (unsigned long long) n->as.literal.int_bits,
                    n->as.literal.int_overflows_i64 ? 1 : 0);
            return true;
        case AST_LITERAL_FLOAT:
            sig_add(s, " f=%.17g", n->as.literal.raw_value.float_val);
            return true;
        case AST_LITERAL_BIGINT:
            sig_name(s, "big", n->as.literal.raw_value.bigint_val);
            return true;
        case AST_LITERAL_STRING:
            sig_name(s, "str", n->as.literal.raw_value.string_val);
            sig_add(s, " esc=%u form=%u chunk=%d", (unsigned) n->as.literal.escape_mode,
                    (unsigned) n->as.literal.source_form, n->as.literal.is_template_chunk ? 1 : 0);
            return true;
        case AST_LITERAL_RUNE:
            sig_add(s, " rune=%u", (unsigned) n->as.literal.raw_value.rune_val);
            return true;
        case AST_LITERAL_REGEX:
            sig_name(s, "pat", n->as.literal.raw_value.regex.pattern);
            sig_name(s, "flags", n->as.literal.raw_value.regex.flags);
            return true;
        case AST_LITERAL_NULL:
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
        case AST_THIS_EXPR:
        case AST_PATTERN_WILDCARD:
        case AST_CANCELLED_EXPR:
            return true;
        case AST_FIXED_BYTES_LITERAL:
            sig_add(s, " bytes=%zu nul=%d esc=%u form=%u", n->as.fixed_bytes_literal.payload_length,
                    n->as.fixed_bytes_literal.append_nul ? 1 : 0,
                    (unsigned) n->as.fixed_bytes_literal.escape_mode,
                    (unsigned) n->as.fixed_bytes_literal.source_form);
            for (size_t i = 0; i < n->as.fixed_bytes_literal.payload_length; i++)
                sig_add(s, "%02x",
                        n->as.fixed_bytes_literal.payload ? n->as.fixed_bytes_literal.payload[i]
                                                          : 0);
            return true;

        /* ---- Operator shapes: the node type already carries the operator ---- */
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
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_BINARY_AND:
        case AST_BINARY_OR:
        case AST_NULLISH_COALESCE:
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
        case AST_FORCE_UNWRAP:
        case AST_GROUPING:
        case AST_EXPR_STMT:
        case AST_SPREAD_EXPR:
        case AST_COMPTIME_EXPR:
        case AST_MOVE_EXPR:
        case AST_UNSAFE_EXPR:
        case AST_THROW_STMT:
        case AST_DEFER_STMT:
        case AST_YIELD_STMT:
        case AST_CHANNEL_NEW:
        case AST_PATTERN_LITERAL:
        case AST_TERNARY:
        case AST_INDEX_GET:
        case AST_INDEX_SET:
        case AST_SLICE_EXPR:
        case AST_ENUM_INDEX:
        case AST_PROGRAM:
            return true;

        case AST_VARIABLE:
            sig_name(s, "name", n->as.variable.name);
            return true;
        case AST_BLOCK:
            sig_add(s, " defercap=%d", n->as.block.is_synthetic_defer_capture ? 1 : 0);
            return true;
        case AST_PRINT_STMT:
            sig_add(s, " skipnull=%d n=%d", n->as.print_stmt.skip_null ? 1 : 0,
                    n->as.print_stmt.expr_count);
            return true;
        case AST_RETURN_STMT:
            sig_add(s, " n=%d", n->as.return_stmt.value_count);
            return true;

        case AST_VAR_DECL:
        case AST_CONST_DECL:
            sig_name(s, "name", n->as.var_decl.name);
            sig_add(s, " const=%d", n->as.var_decl.is_const ? 1 : 0);
            sig_attributes(s, n->as.var_decl.attributes, n->as.var_decl.attr_count);
            return true;
        case AST_ASSIGNMENT:
            sig_name(s, "name", n->as.assignment.name);
            return true;
        case AST_COMPOUND_ASSIGNMENT:
            sig_name(s, "name", n->as.compound_assignment.name);
            sig_add(s, " op=%d", (int) n->as.compound_assignment.op);
            return true;
        case AST_INC:
            sig_name(s, "name", n->as.inc.name);
            return true;
        case AST_DEC:
            sig_name(s, "name", n->as.dec.name);
            return true;
        case AST_DESTRUCTURE_DECL:
            sig_add(s, " const=%d ", n->as.destructure_decl.is_const ? 1 : 0);
            sig_destructure(s, n->as.destructure_decl.pattern);
            return true;
        case AST_DESTRUCTURE_ASSIGN:
            sig_add(s, " ");
            sig_destructure(s, n->as.destructure_assign.pattern);
            return true;

        case AST_IF_STMT:
            return true;
        case AST_WHILE_STMT:
            sig_name(s, "label", n->as.while_stmt.label);
            return true;
        case AST_FOR_STMT:
            sig_name(s, "label", n->as.for_stmt.label);
            return true;
        case AST_FOR_IN_STMT:
            sig_name(s, "label", n->as.for_in_stmt.label);
            sig_name(s, "item", n->as.for_in_stmt.item_name);
            sig_name(s, "value", n->as.for_in_stmt.value_name);
            sig_add(s, " kv=%d", n->as.for_in_stmt.is_keyvalue ? 1 : 0);
            return true;
        case AST_BREAK_STMT:
            sig_name(s, "label", n->as.break_stmt.label);
            return true;
        case AST_CONTINUE_STMT:
            sig_name(s, "label", n->as.continue_stmt.label);
            return true;

        case AST_TRY_CATCH:
            sig_add(s, " catches=%d", n->as.try_catch.catch_count);
            for (int i = 0; i < n->as.try_catch.catch_count; i++) {
                const XrCatchClause *c =
                    n->as.try_catch.catch_clauses ? n->as.try_catch.catch_clauses[i] : NULL;
                sig_name(s, "var", c ? c->var_name : NULL);
                sig_add(s, " panic=%d", c && c->is_panic ? 1 : 0);
            }
            return true;

        case AST_CALL_EXPR:
            sig_add(s, " args=%d targs=%d", n->as.call_expr.arg_count,
                    n->as.call_expr.type_arg_count);
            for (int i = 0; i < n->as.call_expr.arg_count; i++)
                sig_add(s, " a%u",
                        (unsigned) (n->as.call_expr.arg_accesses ? n->as.call_expr.arg_accesses[i]
                                                                 : 0));
            return true;
        case AST_NEW_EXPR:
            sig_name(s, "module", n->as.new_expr.module_name);
            sig_name(s, "class", n->as.new_expr.class_name);
            sig_add(s, " args=%d ns=%d", n->as.new_expr.arg_count,
                    n->as.new_expr.is_type_namespace ? 1 : 0);
            return true;
        case AST_SUPER_CALL:
            sig_name(s, "method", n->as.super_call.method_name);
            sig_add(s, " args=%d", n->as.super_call.arg_count);
            return true;
        case AST_MEMBER_ACCESS:
            sig_name(s, "name", n->as.member_access.name);
            return true;
        case AST_MEMBER_SET:
            sig_name(s, "member", n->as.member_set.member);
            return true;
        case AST_ARRAY_LITERAL:
            sig_add(s, " n=%d repeat=%d", n->as.array_literal.count,
                    n->as.array_literal.is_repeat ? 1 : 0);
            return true;
        case AST_TUPLE_LITERAL:
            sig_add(s, " n=%d", n->as.tuple_literal.count);
            return true;
        case AST_SET_LITERAL:
            sig_add(s, " n=%d", n->as.set_literal.count);
            return true;
        case AST_OBJECT_LITERAL:
            sig_add(s, " n=%d", n->as.object_literal.count);
            for (int i = 0; i < n->as.object_literal.count; i++)
                sig_add(s, " c%d",
                        n->as.object_literal.computed && n->as.object_literal.computed[i] ? 1 : 0);
            return true;
        case AST_MAP_LITERAL:
            sig_add(s, " n=%d", n->as.map_literal.count);
            return true;
        case AST_STRUCT_LITERAL:
            sig_name(s, "struct", n->as.struct_literal.struct_name);
            sig_add(s, " fields=%d", n->as.struct_literal.field_count);
            for (int i = 0; i < n->as.struct_literal.field_count; i++)
                sig_name(s, "f",
                         n->as.struct_literal.field_names ? n->as.struct_literal.field_names[i]
                                                          : NULL);
            return true;
        case AST_TEMPLATE_STRING:
            sig_add(s, " parts=%d esc=%u form=%u", n->as.template_str.part_count,
                    (unsigned) n->as.template_str.escape_mode,
                    (unsigned) n->as.template_str.source_form);
            return true;
        case AST_OPTIONAL_CHAIN:
            sig_name(s, "name", n->as.optional_chain.name);
            sig_add(s, " kind=%d", n->as.optional_chain.chain_type);
            return true;
        case AST_RANGE:
            sig_add(s, " inclusive=%d", n->as.range.inclusive_end ? 1 : 0);
            return true;
        case AST_IS_EXPR:
            return true;
        case AST_AS_EXPR:
            sig_add(s, " safe=%d", n->as.as_expr.is_safe ? 1 : 0);
            return true;

        case AST_FUNCTION_DECL:
        case AST_FUNCTION_EXPR:
            sig_name(s, "name", n->as.function_decl.name);
            sig_add(s, " gen=%d extern=%d req=%d", n->as.function_decl.is_generator ? 1 : 0,
                    n->as.function_decl.is_extern ? 1 : 0, n->as.function_decl.required_count);
            sig_name(s, "abi", n->as.function_decl.extern_abi);
            sig_add(s, " exported=%d", n->is_exported ? 1 : 0);
            sig_params(s, n->as.function_decl.params, n->as.function_decl.param_count);
            sig_generic_params(s, n->as.function_decl.type_params,
                               n->as.function_decl.type_param_count);
            sig_attributes(s, n->as.function_decl.attributes, n->as.function_decl.attr_count);
            return true;
        case AST_CLASS_DECL:
        case AST_STRUCT_DECL:
        case AST_UNION_DECL:
            sig_name(s, "name", n->as.class_decl.name);
            sig_name(s, "super", n->as.class_decl.super_name);
            sig_name(s, "supermod", n->as.class_decl.super_module);
            sig_add(s, " final=%d packed=%d align=%u exported=%d",
                    n->as.class_decl.explicit_final ? 1 : 0, n->as.class_decl.is_packed ? 1 : 0,
                    (unsigned) n->as.class_decl.explicit_align, n->is_exported ? 1 : 0);
            sig_generic_params(s, n->as.class_decl.type_params, n->as.class_decl.type_param_count);
            sig_attributes(s, n->as.class_decl.attributes, n->as.class_decl.attr_count);
            return true;
        case AST_FIELD_DECL:
            sig_name(s, "name", n->as.field_decl.name);
            sig_add(s, " priv=%d prot=%d static=%d final=%d const=%d flex=%d",
                    n->as.field_decl.is_private ? 1 : 0, n->as.field_decl.is_protected ? 1 : 0,
                    n->as.field_decl.is_static ? 1 : 0, n->as.field_decl.is_final ? 1 : 0,
                    n->as.field_decl.is_const ? 1 : 0, n->as.field_decl.is_flexible ? 1 : 0);
            return true;
        case AST_METHOD_DECL:
            sig_name(s, "name", n->as.method_decl.name);
            sig_add(s,
                    " ctor=%d static=%d priv=%d prot=%d get=%d set=%d sctor=%d var=%d op=%d/%u"
                    " req=%d",
                    n->as.method_decl.is_constructor ? 1 : 0, n->as.method_decl.is_static ? 1 : 0,
                    n->as.method_decl.is_private ? 1 : 0, n->as.method_decl.is_protected ? 1 : 0,
                    n->as.method_decl.is_getter ? 1 : 0, n->as.method_decl.is_setter ? 1 : 0,
                    n->as.method_decl.is_static_constructor ? 1 : 0,
                    n->as.method_decl.is_variadic ? 1 : 0, n->as.method_decl.is_operator ? 1 : 0,
                    (unsigned) n->as.method_decl.op_type, n->as.method_decl.required_count);
            sig_params(s, n->as.method_decl.params, n->as.method_decl.param_count);
            sig_generic_params(s, n->as.method_decl.type_params,
                               n->as.method_decl.type_param_count);
            sig_attributes(s, n->as.method_decl.attributes, n->as.method_decl.attr_count);
            return true;
        case AST_INTERFACE_DECL:
            sig_name(s, "name", n->as.interface_decl.name);
            sig_add(s, " exported=%d", n->is_exported ? 1 : 0);
            sig_generic_params(s, n->as.interface_decl.type_params,
                               n->as.interface_decl.type_param_count);
            return true;
        case AST_INTERFACE_METHOD:
            sig_name(s, "name", n->as.interface_method.name);
            sig_params(s, n->as.interface_method.params, n->as.interface_method.param_count);
            sig_generic_params(s, n->as.interface_method.type_params,
                               n->as.interface_method.type_param_count);
            sig_attributes(s, n->as.interface_method.attributes, n->as.interface_method.attr_count);
            return true;
        case AST_INTERFACE_PROPERTY:
            sig_name(s, "name", n->as.interface_property.name);
            sig_add(s, " readonly=%d", n->as.interface_property.is_readonly ? 1 : 0);
            return true;

        case AST_ENUM_DECL:
            sig_name(s, "name", n->as.enum_decl.name);
            sig_add(s, " exported=%d", n->is_exported ? 1 : 0);
            sig_generic_params(s, n->as.enum_decl.type_params, n->as.enum_decl.type_param_count);
            sig_attributes(s, n->as.enum_decl.attributes, n->as.enum_decl.attr_count);
            return true;
        case AST_ENUM_MEMBER:
            sig_name(s, "name", n->as.enum_member.name);
            sig_add(s, " payload=%d", n->as.enum_member.payload_count);
            for (int i = 0; i < n->as.enum_member.payload_count; i++)
                sig_name(s, "p",
                         n->as.enum_member.payload_names ? n->as.enum_member.payload_names[i]
                                                         : NULL);
            return true;
        case AST_ENUM_ACCESS:
            sig_name(s, "enum", n->as.enum_access.enum_name);
            sig_name(s, "member", n->as.enum_access.member_name);
            return true;

        case AST_IMPORT_STMT:
            sig_name(s, "module", n->as.import_stmt.module_name);
            sig_name(s, "alias", n->as.import_stmt.alias);
            sig_add(s, " quoted=%d members=%d", n->as.import_stmt.is_quoted ? 1 : 0,
                    n->as.import_stmt.member_count);
            for (int i = 0; i < n->as.import_stmt.member_count; i++) {
                sig_name(s, "m",
                         n->as.import_stmt.members ? n->as.import_stmt.members[i].name : NULL);
                sig_name(s, "as",
                         n->as.import_stmt.members ? n->as.import_stmt.members[i].alias : NULL);
            }
            return true;
        case AST_EXPORT_STMT:
            sig_name(s, "from", n->as.export_stmt.from_path);
            sig_add(s, " quoted=%d all=%d n=%d", n->as.export_stmt.from_is_quoted ? 1 : 0,
                    n->as.export_stmt.is_reexport_all ? 1 : 0, n->as.export_stmt.reexport_count);
            for (int i = 0; i < n->as.export_stmt.reexport_count; i++) {
                sig_name(s, "m",
                         n->as.export_stmt.reexport_members
                             ? n->as.export_stmt.reexport_members[i].name
                             : NULL);
                sig_name(s, "as",
                         n->as.export_stmt.reexport_members
                             ? n->as.export_stmt.reexport_members[i].alias
                             : NULL);
            }
            return true;
        case AST_GLOBAL_ASM:
            sig_name(s, "asm", n->as.global_asm.text);
            return true;
        case AST_TYPE_ALIAS:
            sig_name(s, "name", n->as.type_alias.name);
            sig_add(s, " fields=%d exported=%d", n->as.type_alias.field_count,
                    n->is_exported ? 1 : 0);
            for (int i = 0; i < n->as.type_alias.field_count; i++) {
                sig_name(s, "f",
                         n->as.type_alias.field_names ? n->as.type_alias.field_names[i] : NULL);
                sig_add(s, " opt=%d",
                        n->as.type_alias.field_optional && n->as.type_alias.field_optional[i] ? 1
                                                                                              : 0);
            }
            sig_generic_params(s, n->as.type_alias.type_params, n->as.type_alias.type_param_count);
            return true;

        case AST_MATCH_EXPR:
            sig_add(s, " arms=%d", n->as.match_expr.arm_count);
            return true;
        case AST_MATCH_ARM:
            return true;
        case AST_PATTERN_RANGE:
            sig_add(s, " inclusive=%d", n->as.pattern_range.inclusive_end ? 1 : 0);
            return true;
        case AST_PATTERN_MULTI:
            sig_add(s, " n=%d", n->as.pattern_multi.count);
            return true;
        case AST_PATTERN_TUPLE:
            sig_add(s, " n=%d", n->as.pattern_tuple.count);
            return true;
        case AST_PATTERN_OBJECT:
            sig_add(s, " n=%d", n->as.pattern_object.count);
            for (int i = 0; i < n->as.pattern_object.count; i++)
                sig_name(s, "f",
                         n->as.pattern_object.field_names ? n->as.pattern_object.field_names[i]
                                                          : NULL);
            return true;
        case AST_PATTERN_ARRAY:
            sig_add(s, " n=%d rest=%d", n->as.pattern_array.count,
                    n->as.pattern_array.has_rest ? 1 : 0);
            sig_name(s, "restname", n->as.pattern_array.rest_name);
            return true;
        case AST_PATTERN_ADT:
            sig_add(s, " n=%d", n->as.pattern_adt.count);
            return true;
        case AST_PATTERN_TYPE:
            sig_name(s, "bind", n->as.pattern_type.binding_name);
            return true;

        case AST_GO_EXPR:
            sig_name(s, "name", n->as.go_expr.name);
            sig_add(s, " link=%u spawn=%u", (unsigned) n->as.go_expr.link_mode,
                    (unsigned) n->as.go_expr.spawn_kind);
            return true;
        case AST_AWAIT_EXPR:
            sig_add(s, " any=%d all=%d anysuccess=%d", n->as.await_expr.is_any ? 1 : 0,
                    n->as.await_expr.is_all ? 1 : 0, n->as.await_expr.is_any_success ? 1 : 0);
            return true;
        case AST_SELECT_STMT:
            sig_add(s, " cases=%d", n->as.select_stmt.case_count);
            return true;
        case AST_SELECT_CASE:
            sig_name(s, "var", n->as.select_case.var_name);
            sig_add(s, " send=%d default=%d timeout=%d", n->as.select_case.is_send ? 1 : 0,
                    n->as.select_case.is_default ? 1 : 0, n->as.select_case.is_timeout ? 1 : 0);
            return true;
        case AST_SCOPE_BLOCK:
            sig_add(s, " mode=%u", (unsigned) n->as.scope_block.scope_mode);
            return true;

        default:
            return false;
    }
}

bool xr_ast_node_signature(const AstNode *node, char *buf, size_t buf_size) {
    if (!node || !buf || buf_size == 0)
        return false;
    SigBuf s = {.buf = buf, .cap = buf_size, .len = 0, .overflow = false};
    buf[0] = '\0';
    sig_add(&s, "%s", xr_ast_typename(node->type));
    if (!write_payload(node, &s))
        return false;
    return !s.overflow;
}
