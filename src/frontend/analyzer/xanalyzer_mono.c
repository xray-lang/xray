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
 *   - Instance identity is the concrete type tuple. A duck-typed body resolves
 *     `x.foo()` against that concrete argument, so two ABI-equivalent
 *     instantiations are not interchangeable at this stage; merging them is an
 *     AOT decision made after resolution, with evidence.
 *   - Duck-typed: no trait bounds needed, errors reported at instantiation
 */

#include "xanalyzer_mono.h"
#include "xanalyzer.h"
#include "../../base/xlog.h"
#include "../../base/xchecks.h"
#include "../../runtime/value/xtype.h"
#include "../../runtime/xerror_codes.h"
#include "../../toolchain/xcompiler_session.h"
#include "../parser/xast_nodes.h"
#include "../parser/xtype_ref.h"
#include "xtype_ref_resolve.h"
#include "../../base/xmalloc.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========== Name Mangling ========== */

/* Rendering capacity for one qualified type-argument tag. Nested tags recurse
 * into their own smaller buffers, so this only bounds the outermost tag. */
#define XR_MONO_TYPE_TAG_CAP 256

/* User-facing display name for a concrete type argument.
 * Returns canonical names: "i64", "f64", "string", "bool", etc.
 * For named/generic types, returns the type's own name (e.g. "Array"). */
static const char *mono_type_display_name(XrTypeRef *t) {
    if (!t)
        return "unknown";
    switch ((XrTypeRefKind) t->kind) {
        case XR_TREF_SCALAR:
            return xr_scalar_rep_name(t->scalar_rep);
        case XR_TREF_BOOL:
            return "bool";
        case XR_TREF_RUNE:
            return "rune";
        case XR_TREF_STRING:
            return "string";
        case XR_TREF_NULL:
            return "null";
        case XR_TREF_UNIT:
            return "()";
        case XR_TREF_ERROR:
            return "<error>";
        case XR_TREF_NAMED:
        case XR_TREF_GENERIC:
            return t->name ? t->name : "object";
        case XR_TREF_CONST:
            return "const";
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
        case XR_TREF_SCALAR: {
            const char *name = xr_scalar_rep_name(t->scalar_rep);
            return name ? name : "scalar_unknown";
        }
        case XR_TREF_BOOL:
            return "bool";
        case XR_TREF_RUNE:
            return "rune";
        case XR_TREF_STRING:
            return "str";
        case XR_TREF_NULL:
            return "null";
        case XR_TREF_UNIT:
            return "unit";
        case XR_TREF_ERROR:
            return "err";
        case XR_TREF_NAMED:
        case XR_TREF_GENERIC:
            return t->name ? t->name : "obj";
        case XR_TREF_CONST:
            return "const";
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

/* Kinds whose identity lives in their children rather than in their own tag. */
static bool mono_tag_needs_children(uint8_t kind) {
    switch ((XrTypeRefKind) kind) {
        case XR_TREF_CONST:
        case XR_TREF_GENERIC:
        case XR_TREF_OPTIONAL:
        case XR_TREF_TUPLE:
        case XR_TREF_OBJECT:
            return true;
        default:
            return false;
    }
}

static uint64_t mono_hash_bytes(uint64_t hash, const void *data, size_t length) {
    const unsigned char *bytes = (const unsigned char *) data;
    for (size_t i = 0; i < length; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t mono_hash_cstr(uint64_t hash, const char *value) {
    uint64_t length = value ? (uint64_t) strlen(value) : 0;
    hash = mono_hash_bytes(hash, &length, sizeof(length));
    return value ? mono_hash_bytes(hash, value, (size_t) length) : hash;
}

/* Structural-object layout is part of specialization identity: field order
 * determines the direct ordinal selected in a clone. Keep its mangled tag
 * compact while hashing the complete, recursively qualified type reference;
 * unlike a fixed rendering buffer, this cannot collapse merely because a
 * structural type has many fields. */
static uint64_t mono_type_ref_hash64(const XrTypeRef *type) {
    uint64_t hash = UINT64_C(1469598103934665603);
    if (!type)
        return mono_hash_cstr(hash, "<null>");
    hash = mono_hash_bytes(hash, &type->kind, sizeof(type->kind));
    hash = mono_hash_bytes(hash, &type->nchildren, sizeof(type->nchildren));
    hash = mono_hash_bytes(hash, &type->scalar_rep, sizeof(type->scalar_rep));
    hash = mono_hash_bytes(hash, &type->fixed_length, sizeof(type->fixed_length));
    hash = mono_hash_cstr(hash, type->name);
    for (uint8_t i = 0; i < type->nchildren; i++) {
        if (type->kind == XR_TREF_OBJECT) {
            hash = mono_hash_cstr(hash, type->field_names ? type->field_names[i] : NULL);
            bool readonly = type->field_readonly && type->field_readonly[i];
            hash = mono_hash_bytes(hash, &readonly, sizeof(readonly));
        }
        uint64_t child_hash = mono_type_ref_hash64(type->children ? type->children[i] : NULL);
        hash = mono_hash_bytes(hash, &child_hash, sizeof(child_hash));
    }
    return hash;
}

static void mono_qualified_type_tag(XrTypeRef *t, char *buf, size_t cap) {
    if (!buf || cap == 0)
        return;
    buf[0] = '\0';
    if (!t) {
        snprintf(buf, cap, "unknown");
        return;
    }
    if (t->kind == XR_TREF_CONST) {
        char inner[192];
        mono_qualified_type_tag(t->children && t->nchildren > 0 ? t->children[0] : NULL, inner,
                                sizeof(inner));
        snprintf(buf, cap, "const_%s", inner);
        return;
    }
    if (t->kind == XR_TREF_OPTIONAL) {
        char inner[192];
        mono_qualified_type_tag(t->children && t->nchildren > 0 ? t->children[0] : NULL, inner,
                                sizeof(inner));
        snprintf(buf, cap, "opt_%s", inner);
        return;
    }
    if (t->kind == XR_TREF_OBJECT) {
        snprintf(buf, cap, "obj%u_%016" PRIx64, (unsigned) t->nchildren, mono_type_ref_hash64(t));
        return;
    }
    /* GENERIC carries its head name; TUPLE has none, so it needs an explicit
     * one or (int, string) and (string, int) would differ only by child order
     * in a name that also has to stay collision-free against a class literally
     * called "tup". The arity keeps a 2-tuple distinct from a 1-tuple whose
     * element tag happens to concatenate the same way. */
    if (t->kind == XR_TREF_GENERIC || t->kind == XR_TREF_TUPLE) {
        size_t used;
        if (t->kind == XR_TREF_TUPLE)
            used = (size_t) snprintf(buf, cap, "tup%u", (unsigned) t->nchildren);
        else
            used = (size_t) snprintf(buf, cap, "%s", t->name ? t->name : "obj");
        for (uint8_t i = 0; i < t->nchildren && used < cap; i++) {
            char child[128];
            mono_qualified_type_tag(t->children ? t->children[i] : NULL, child, sizeof(child));
            int written = snprintf(buf + used, cap - used, "_%s", child);
            if (written < 0)
                return;
            used += (size_t) written;
        }
        return;
    }
    snprintf(buf, cap, "%s", xr_mono_type_tag(t));
}

char *xr_mono_mangle(const char *name, XrTypeRef **type_args, int count) {
    if (!name || count <= 0 || !type_args)
        return xr_strdup(name ? name : "");

    /* Every type argument whose identity lives in its children needs a
     * recursive tag. A head-only tag collapses distinct instances into one, and
     * the second call site is then rewritten to a clone whose parameter types
     * belong to the first: "const" would merge const Array<int> with
     * const Map<K,V>, "Array" would merge Array<int> with Array<string>, and
     * "unknown"/"opt" would merge every tuple and every optional. Scalars carry
     * their whole identity in the tag itself and keep the flat form.
     *
     * The qualified buffers are heap-allocated rather than a fixed array: a
     * capped array had to fall back to the unmangled name past its bound,
     * which collapses every instance of that generic onto one symbol. Mangling
     * must never lose an argument -- the mangled name *is* instance identity. */
    char *qualified = NULL;
    const char **tags = (const char **) xr_calloc((size_t) count, sizeof(const char *));
    if (!tags)
        return xr_strdup(name);
    for (int i = 0; i < count; i++) {
        if (type_args[i] && mono_tag_needs_children(type_args[i]->kind)) {
            if (!qualified) {
                qualified = (char *) xr_calloc((size_t) count, XR_MONO_TYPE_TAG_CAP);
                if (!qualified) {
                    xr_free((void *) tags);
                    return xr_strdup(name);
                }
            }
            char *slot = qualified + (size_t) i * XR_MONO_TYPE_TAG_CAP;
            mono_qualified_type_tag(type_args[i], slot, XR_MONO_TYPE_TAG_CAP);
            tags[i] = slot;
        } else {
            tags[i] = xr_mono_type_tag(type_args[i]);
        }
    }

    // Calculate buffer size: name + '$' + tags joined by '_'
    size_t len = strlen(name) + 1;  // name + '$'
    for (int i = 0; i < count; i++) {
        len += strlen(tags[i]) + 1;  // tag + '_' separator
    }
    len += 1;  // null terminator

    char *buf = (char *) xr_malloc(len);
    if (!buf) {
        xr_free(qualified);
        xr_free((void *) tags);
        return xr_strdup(name);
    }

    char *p = buf;
    size_t remaining = len;
    int written = snprintf(p, remaining, "%s$", name);
    p += written;
    remaining -= written;

    for (int i = 0; i < count; i++) {
        const char *tag = tags[i];
        if (i > 0) {
            *p++ = '_';
            remaining--;
        }
        written = snprintf(p, remaining, "%s", tag);
        p += written;
        remaining -= written;
    }
    xr_free(qualified);
    xr_free((void *) tags);
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

typedef struct {
    XrCompilerSession *session;
    bool preserve_symbol_ids;
} XrAstCloneCtx;

static AstNode *xr_ast_clone_ctx(AstNode *node, XrMonoTypeMap *map, int mc,
                                 XrAstCloneCtx *clone_ctx);

static uint32_t clone_node_id(const AstNode *node, XrAstCloneCtx *clone_ctx) {
    if (clone_ctx && clone_ctx->session)
        return xr_compiler_session_next_ast_node_id(clone_ctx->session);
    return node ? node->node_id : 0;
}

static AstNode **clone_node_array(AstNode **arr, int count, XrMonoTypeMap *map, int map_count,
                                  XrAstCloneCtx *clone_ctx) {
    if (!arr || count <= 0)
        return NULL;
    AstNode **result = (AstNode **) xr_calloc(count, sizeof(AstNode *));
    for (int i = 0; i < count; i++) {
        result[i] = xr_ast_clone_ctx(arr[i], map, map_count, clone_ctx);
    }
    return result;
}

static XrCallArgAccess *clone_call_arg_accesses(XrCallArgAccess *arr, int count) {
    if (!arr || count <= 0)
        return NULL;
    XrCallArgAccess *result =
        (XrCallArgAccess *) xr_calloc((size_t) count, sizeof(XrCallArgAccess));
    for (int i = 0; i < count; i++)
        result[i] = xr_call_arg_access_is_valid(arr[i]) ? arr[i] : XR_CALL_ARG_PLAIN;
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

static XrParamNode **clone_params(XrParamNode **params, int count, XrMonoTypeMap *map, int mc,
                                  XrAstCloneCtx *clone_ctx) {
    if (!params || count <= 0)
        return NULL;
    XrParamNode **result = (XrParamNode **) xr_calloc(count, sizeof(XrParamNode *));
    for (int i = 0; i < count; i++) {
        XrParamNode *p = (XrParamNode *) xr_calloc(1, sizeof(XrParamNode));
        *p = *params[i];
        p->name = clone_str(params[i]->name);
        p->type = sub_tref(params[i]->type, map, mc);
        p->default_value = xr_ast_clone_ctx(params[i]->default_value, map, mc, clone_ctx);
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

/* Clone method-local generic params. Constraints go through sub_tref so a
 * bound that mentions an enclosing class type param (for example
 * `find<U: Comparable<T>>` inside `Box<T>`) lands on the substituted type. */
static XrGenericParam **clone_generic_params(XrGenericParam **arr, int count, XrMonoTypeMap *map,
                                             int mc) {
    if (!arr || count <= 0)
        return NULL;
    XrGenericParam **result = (XrGenericParam **) xr_calloc((size_t) count, sizeof(*result));
    for (int i = 0; i < count; i++) {
        if (!arr[i])
            continue;
        XrGenericParam *gp = (XrGenericParam *) xr_calloc(1, sizeof(XrGenericParam));
        gp->name = clone_str(arr[i]->name);
        gp->constraints = clone_tref_array(arr[i]->constraints, arr[i]->constraint_count, map, mc);
        gp->constraint_count = gp->constraints ? arr[i]->constraint_count : 0;
        result[i] = gp;
    }
    return result;
}

static AstNode *xr_ast_clone_ctx(AstNode *node, XrMonoTypeMap *map, int mc,
                                 XrAstCloneCtx *clone_ctx) {
    XR_DCHECK(map != NULL || mc == 0, "xr_ast_clone: map is NULL with non-zero mc");
    if (!node)
        return NULL;

    AstNode *n = (AstNode *) xr_calloc(1, sizeof(AstNode));
    n->type = node->type;
    n->node_id = clone_node_id(node, clone_ctx);
    n->line = node->line;
    n->column = node->column;
    n->end_line = node->end_line;
    n->end_column = node->end_column;
    n->is_exported = node->is_exported;
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
        case AST_LITERAL_RUNE:
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
        case AST_LITERAL_NULL:
            n->as.literal = node->as.literal;
            break;
        case AST_LITERAL_STRING:
            n->as.literal = node->as.literal;
            n->as.literal.raw_value.string_val = clone_str(node->as.literal.raw_value.string_val);
            break;
        case AST_FIXED_BYTES_LITERAL:
            n->as.fixed_bytes_literal = node->as.fixed_bytes_literal;
            if (node->as.fixed_bytes_literal.payload_length > 0) {
                uint8_t *payload =
                    (uint8_t *) xr_malloc(node->as.fixed_bytes_literal.payload_length);
                memcpy(payload, node->as.fixed_bytes_literal.payload,
                       node->as.fixed_bytes_literal.payload_length);
                n->as.fixed_bytes_literal.payload = payload;
            }
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
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_BINARY_AND:
        case AST_BINARY_OR:
        case AST_NULLISH_COALESCE:
            n->as.binary.left = xr_ast_clone_ctx(node->as.binary.left, map, mc, clone_ctx);
            n->as.binary.right = xr_ast_clone_ctx(node->as.binary.right, map, mc, clone_ctx);
            break;
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
            n->as.unary.operand = xr_ast_clone_ctx(node->as.unary.operand, map, mc, clone_ctx);
            break;

        // === Grouping / Expr stmt ===
        case AST_GROUPING:
            n->as.grouping = xr_ast_clone_ctx(node->as.grouping, map, mc, clone_ctx);
            break;
        case AST_COMPTIME_EXPR:
            n->as.comptime_expr.expr =
                xr_ast_clone_ctx(node->as.comptime_expr.expr, map, mc, clone_ctx);
            break;
        case AST_EXPR_STMT:
            n->as.expr_stmt = xr_ast_clone_ctx(node->as.expr_stmt, map, mc, clone_ctx);
            break;

        // === Print ===
        case AST_PRINT_STMT:
            n->as.print_stmt.expr_count = node->as.print_stmt.expr_count;
            n->as.print_stmt.exprs = clone_node_array(
                node->as.print_stmt.exprs, node->as.print_stmt.expr_count, map, mc, clone_ctx);
            break;

        // === Block ===
        case AST_BLOCK:
            n->as.block.count = node->as.block.count;
            n->as.block.capacity = node->as.block.count;
            n->as.block.statements = clone_node_array(node->as.block.statements,
                                                      node->as.block.count, map, mc, clone_ctx);
            break;

        // === Variable ===
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            n->as.var_decl.name = clone_str(node->as.var_decl.name);
            n->as.var_decl.initializer =
                xr_ast_clone_ctx(node->as.var_decl.initializer, map, mc, clone_ctx);
            n->as.var_decl.is_const = node->as.var_decl.is_const;
            n->as.var_decl.type_annotation = sub_tref(node->as.var_decl.type_annotation, map, mc);
            break;
        case AST_VARIABLE:
            n->as.variable.name = clone_str(node->as.variable.name);
            n->as.variable.symbol_id =
                clone_ctx && clone_ctx->preserve_symbol_ids ? node->as.variable.symbol_id : 0;
            break;
        case AST_ASSIGNMENT:
            n->as.assignment.name = clone_str(node->as.assignment.name);
            n->as.assignment.value =
                xr_ast_clone_ctx(node->as.assignment.value, map, mc, clone_ctx);
            break;
        case AST_COMPOUND_ASSIGNMENT:
            n->as.compound_assignment.name = clone_str(node->as.compound_assignment.name);
            n->as.compound_assignment.op = node->as.compound_assignment.op;
            n->as.compound_assignment.value =
                xr_ast_clone_ctx(node->as.compound_assignment.value, map, mc, clone_ctx);
            n->as.compound_assignment.object =
                xr_ast_clone_ctx(node->as.compound_assignment.object, map, mc, clone_ctx);
            break;
        case AST_INC:
        case AST_DEC:
            n->as.inc.name = clone_str(node->as.inc.name);
            break;

        // === Control flow ===
        case AST_IF_STMT:
            n->as.if_stmt.condition =
                xr_ast_clone_ctx(node->as.if_stmt.condition, map, mc, clone_ctx);
            n->as.if_stmt.then_branch =
                xr_ast_clone_ctx(node->as.if_stmt.then_branch, map, mc, clone_ctx);
            n->as.if_stmt.else_branch =
                xr_ast_clone_ctx(node->as.if_stmt.else_branch, map, mc, clone_ctx);
            break;
        case AST_WHILE_STMT:
            n->as.while_stmt.label = clone_str(node->as.while_stmt.label);
            n->as.while_stmt.condition =
                xr_ast_clone_ctx(node->as.while_stmt.condition, map, mc, clone_ctx);
            n->as.while_stmt.body = xr_ast_clone_ctx(node->as.while_stmt.body, map, mc, clone_ctx);
            break;
        case AST_FOR_STMT:
            n->as.for_stmt.label = clone_str(node->as.for_stmt.label);
            n->as.for_stmt.initializer =
                xr_ast_clone_ctx(node->as.for_stmt.initializer, map, mc, clone_ctx);
            n->as.for_stmt.condition =
                xr_ast_clone_ctx(node->as.for_stmt.condition, map, mc, clone_ctx);
            n->as.for_stmt.increment =
                xr_ast_clone_ctx(node->as.for_stmt.increment, map, mc, clone_ctx);
            n->as.for_stmt.body = xr_ast_clone_ctx(node->as.for_stmt.body, map, mc, clone_ctx);
            break;
        case AST_FOR_IN_STMT:
            n->as.for_in_stmt.label = clone_str(node->as.for_in_stmt.label);
            n->as.for_in_stmt.item_name = clone_str(node->as.for_in_stmt.item_name);
            n->as.for_in_stmt.value_name = clone_str(node->as.for_in_stmt.value_name);
            n->as.for_in_stmt.is_keyvalue = node->as.for_in_stmt.is_keyvalue;
            n->as.for_in_stmt.domain_kind = node->as.for_in_stmt.domain_kind;
            n->as.for_in_stmt.enum_symbol_id = node->as.for_in_stmt.enum_symbol_id;
            n->as.for_in_stmt.enum_variant_count = node->as.for_in_stmt.enum_variant_count;
            n->as.for_in_stmt.item_type = sub_tref(node->as.for_in_stmt.item_type, map, mc);
            n->as.for_in_stmt.collection =
                xr_ast_clone_ctx(node->as.for_in_stmt.collection, map, mc, clone_ctx);
            n->as.for_in_stmt.body =
                xr_ast_clone_ctx(node->as.for_in_stmt.body, map, mc, clone_ctx);
            break;
        case AST_BREAK_STMT:
            n->as.break_stmt.label = clone_str(node->as.break_stmt.label);
            break;
        case AST_CONTINUE_STMT:
            n->as.continue_stmt.label = clone_str(node->as.continue_stmt.label);
            break;

        // === Function ===
        case AST_FUNCTION_DECL:
        case AST_FUNCTION_EXPR: {
            FunctionDeclNode *src = &node->as.function_decl;
            FunctionDeclNode *dst = &n->as.function_decl;
            dst->name = clone_str(src->name);
            dst->params = clone_params(src->params, src->param_count, map, mc, clone_ctx);
            dst->param_count = src->param_count;
            dst->required_count = src->required_count;
            dst->return_type = sub_tref(src->return_type, map, mc);
            dst->body = xr_ast_clone_ctx(src->body, map, mc, clone_ctx);
            dst->is_generator = src->is_generator;
            dst->is_extern = src->is_extern;
            dst->extern_abi = clone_str(src->extern_abi);
            dst->attributes = NULL;  // Attributes not cloned for mono
            dst->attr_count = 0;
            dst->type_params = NULL;  // Cleared: mono version has no type params
            dst->type_param_count = 0;
            break;
        }

        // === Call ===
        case AST_CALL_EXPR:
            n->as.call_expr.callee =
                xr_ast_clone_ctx(node->as.call_expr.callee, map, mc, clone_ctx);
            n->as.call_expr.arg_count = node->as.call_expr.arg_count;
            n->as.call_expr.supplied_arg_count = node->as.call_expr.supplied_arg_count;
            n->as.call_expr.default_arg_count = node->as.call_expr.default_arg_count;
            n->as.call_expr.default_arg_param_count = node->as.call_expr.default_arg_param_count;
            n->as.call_expr.required_arg_count = node->as.call_expr.required_arg_count;
            n->as.call_expr.arguments = clone_node_array(
                node->as.call_expr.arguments, node->as.call_expr.arg_count, map, mc, clone_ctx);
            n->as.call_expr.arg_accesses = clone_call_arg_accesses(node->as.call_expr.arg_accesses,
                                                                   node->as.call_expr.arg_count);
            n->as.call_expr.type_args = clone_tref_array(
                node->as.call_expr.type_args, node->as.call_expr.type_arg_count, map, mc);
            n->as.call_expr.type_arg_count = node->as.call_expr.type_arg_count;
            n->as.call_expr.semantic_type_id = node->as.call_expr.semantic_type_id;
            n->as.call_expr.semantic_type_args =
                clone_tref_array(node->as.call_expr.semantic_type_args,
                                 node->as.call_expr.semantic_type_arg_count, map, mc);
            n->as.call_expr.semantic_type_arg_count = node->as.call_expr.semantic_type_arg_count;
            break;

        // === Return / Yield ===
        case AST_RETURN_STMT:
            n->as.return_stmt.value_count = node->as.return_stmt.value_count;
            n->as.return_stmt.values = clone_node_array(
                node->as.return_stmt.values, node->as.return_stmt.value_count, map, mc, clone_ctx);
            break;
        // === Type check ===
        case AST_IS_EXPR:
            n->as.is_expr.expr = xr_ast_clone_ctx(node->as.is_expr.expr, map, mc, clone_ctx);
            n->as.is_expr.type = sub_tref(node->as.is_expr.type, map, mc);
            break;
        case AST_AS_EXPR:
            n->as.as_expr.expr = xr_ast_clone_ctx(node->as.as_expr.expr, map, mc, clone_ctx);
            n->as.as_expr.type = sub_tref(node->as.as_expr.type, map, mc);
            n->as.as_expr.is_safe = node->as.as_expr.is_safe;
            break;

        // === Array / Index / Slice ===
        case AST_ARRAY_LITERAL:
            n->as.array_literal.count = node->as.array_literal.count;
            n->as.array_literal.is_repeat = node->as.array_literal.is_repeat;
            if (node->as.array_literal.is_repeat) {
                n->as.array_literal.elements = NULL;
                n->as.array_literal.repeat_value =
                    xr_ast_clone_ctx(node->as.array_literal.repeat_value, map, mc, clone_ctx);
                n->as.array_literal.repeat_count =
                    xr_ast_clone_ctx(node->as.array_literal.repeat_count, map, mc, clone_ctx);
            } else {
                n->as.array_literal.repeat_value = NULL;
                n->as.array_literal.repeat_count = NULL;
                n->as.array_literal.elements =
                    clone_node_array(node->as.array_literal.elements, node->as.array_literal.count,
                                     map, mc, clone_ctx);
            }
            break;
        case AST_INDEX_GET:
            n->as.index_get.array = xr_ast_clone_ctx(node->as.index_get.array, map, mc, clone_ctx);
            n->as.index_get.index = xr_ast_clone_ctx(node->as.index_get.index, map, mc, clone_ctx);
            break;
        case AST_INDEX_SET:
            n->as.index_set.array = xr_ast_clone_ctx(node->as.index_set.array, map, mc, clone_ctx);
            n->as.index_set.index = xr_ast_clone_ctx(node->as.index_set.index, map, mc, clone_ctx);
            n->as.index_set.value = xr_ast_clone_ctx(node->as.index_set.value, map, mc, clone_ctx);
            break;
        case AST_SLICE_EXPR:
            n->as.slice_expr.source =
                xr_ast_clone_ctx(node->as.slice_expr.source, map, mc, clone_ctx);
            n->as.slice_expr.start =
                xr_ast_clone_ctx(node->as.slice_expr.start, map, mc, clone_ctx);
            n->as.slice_expr.end = xr_ast_clone_ctx(node->as.slice_expr.end, map, mc, clone_ctx);
            break;

        // === Member access ===
        case AST_MEMBER_ACCESS:
            n->as.member_access.object =
                xr_ast_clone_ctx(node->as.member_access.object, map, mc, clone_ctx);
            n->as.member_access.name = clone_str(node->as.member_access.name);
            break;
        case AST_MEMBER_SET:
            n->as.member_set.object =
                xr_ast_clone_ctx(node->as.member_set.object, map, mc, clone_ctx);
            n->as.member_set.member = clone_str(node->as.member_set.member);
            n->as.member_set.value =
                xr_ast_clone_ctx(node->as.member_set.value, map, mc, clone_ctx);
            break;

        // === Template string ===
        case AST_TEMPLATE_STRING:
            n->as.template_str.part_count = node->as.template_str.part_count;
            n->as.template_str.parts = clone_node_array(
                node->as.template_str.parts, node->as.template_str.part_count, map, mc, clone_ctx);
            break;

        // === Object / Map / Set literals ===
        case AST_OBJECT_LITERAL:
            n->as.object_literal.count = node->as.object_literal.count;
            n->as.object_literal.keys = clone_node_array(
                node->as.object_literal.keys, node->as.object_literal.count, map, mc, clone_ctx);
            n->as.object_literal.values = clone_node_array(
                node->as.object_literal.values, node->as.object_literal.count, map, mc, clone_ctx);
            break;
        case AST_MAP_LITERAL:
            n->as.map_literal.count = node->as.map_literal.count;
            n->as.map_literal.keys = clone_node_array(
                node->as.map_literal.keys, node->as.map_literal.count, map, mc, clone_ctx);
            n->as.map_literal.values = clone_node_array(
                node->as.map_literal.values, node->as.map_literal.count, map, mc, clone_ctx);
            break;
        case AST_SET_LITERAL:
            n->as.set_literal.count = node->as.set_literal.count;
            n->as.set_literal.elements = clone_node_array(
                node->as.set_literal.elements, node->as.set_literal.count, map, mc, clone_ctx);
            break;

        // === Ternary / Range ===
        case AST_TERNARY:
            n->as.ternary.condition =
                xr_ast_clone_ctx(node->as.ternary.condition, map, mc, clone_ctx);
            n->as.ternary.true_expr =
                xr_ast_clone_ctx(node->as.ternary.true_expr, map, mc, clone_ctx);
            n->as.ternary.false_expr =
                xr_ast_clone_ctx(node->as.ternary.false_expr, map, mc, clone_ctx);
            break;
        case AST_RANGE:
            n->as.range.start = xr_ast_clone_ctx(node->as.range.start, map, mc, clone_ctx);
            n->as.range.end = xr_ast_clone_ctx(node->as.range.end, map, mc, clone_ctx);
            n->as.range.inclusive_end = node->as.range.inclusive_end;
            break;

        // === Optional chain / Force unwrap ===
        case AST_OPTIONAL_CHAIN:
            n->as.optional_chain.object =
                xr_ast_clone_ctx(node->as.optional_chain.object, map, mc, clone_ctx);
            n->as.optional_chain.name = clone_str(node->as.optional_chain.name);
            n->as.optional_chain.index =
                xr_ast_clone_ctx(node->as.optional_chain.index, map, mc, clone_ctx);
            n->as.optional_chain.chain_type = node->as.optional_chain.chain_type;
            break;
        case AST_FORCE_UNWRAP:
            n->as.unary.operand = xr_ast_clone_ctx(node->as.unary.operand, map, mc, clone_ctx);
            break;

        // === Try-catch / Throw ===
        case AST_TRY_CATCH: {
            TryCatchNode *src_tc = &node->as.try_catch;
            TryCatchNode *dst_tc = &n->as.try_catch;
            dst_tc->try_body = xr_ast_clone_ctx(src_tc->try_body, map, mc, clone_ctx);
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
                    dc->pattern = xr_ast_clone_ctx(sc->pattern, map, mc, clone_ctx);
                    dc->body = xr_ast_clone_ctx(sc->body, map, mc, clone_ctx);
                    dc->symbol_id = 0;
                    dc->is_panic = sc->is_panic;
                    dst_tc->catch_clauses[ci] = dc;
                }
            }
            break;
        }
        case AST_THROW_STMT:
            n->as.throw_stmt.expression =
                xr_ast_clone_ctx(node->as.throw_stmt.expression, map, mc, clone_ctx);
            break;

        // === new expression ===
        case AST_NEW_EXPR:
            n->as.new_expr.module_name = clone_str(node->as.new_expr.module_name);
            n->as.new_expr.class_name = clone_str(node->as.new_expr.class_name);
            /* The clone is analyzed in its destination scope and receives a
             * destination-owned symbol identity there. */
            n->as.new_expr.class_symbol_id = 0;
            n->as.new_expr.arg_count = node->as.new_expr.arg_count;
            n->as.new_expr.arguments = clone_node_array(
                node->as.new_expr.arguments, node->as.new_expr.arg_count, map, mc, clone_ctx);
            n->as.new_expr.arg_accesses = clone_call_arg_accesses(node->as.new_expr.arg_accesses,
                                                                  node->as.new_expr.arg_count);
            n->as.new_expr.type_args = clone_tref_array(node->as.new_expr.type_args,
                                                        node->as.new_expr.type_arg_count, map, mc);
            n->as.new_expr.type_arg_count = node->as.new_expr.type_arg_count;
            n->as.new_expr.is_type_namespace = node->as.new_expr.is_type_namespace;
            break;
        case AST_THIS_EXPR:
            break;

        // === Super call ===
        case AST_SUPER_CALL:
            n->as.super_call.method_name = clone_str(node->as.super_call.method_name);
            n->as.super_call.arg_count = node->as.super_call.arg_count;
            n->as.super_call.arguments = clone_node_array(
                node->as.super_call.arguments, node->as.super_call.arg_count, map, mc, clone_ctx);
            n->as.super_call.arg_accesses = clone_call_arg_accesses(
                node->as.super_call.arg_accesses, node->as.super_call.arg_count);
            break;

        // === Match expression ===
        case AST_MATCH_EXPR:
            n->as.match_expr.expr = xr_ast_clone_ctx(node->as.match_expr.expr, map, mc, clone_ctx);
            n->as.match_expr.arm_count = node->as.match_expr.arm_count;
            n->as.match_expr.arms = clone_node_array(
                node->as.match_expr.arms, node->as.match_expr.arm_count, map, mc, clone_ctx);
            break;
        case AST_MATCH_ARM:
            n->as.match_arm.pattern =
                xr_ast_clone_ctx(node->as.match_arm.pattern, map, mc, clone_ctx);
            n->as.match_arm.guard = xr_ast_clone_ctx(node->as.match_arm.guard, map, mc, clone_ctx);
            n->as.match_arm.body = xr_ast_clone_ctx(node->as.match_arm.body, map, mc, clone_ctx);
            break;

        // === Pattern nodes ===
        case AST_PATTERN_LITERAL:
            n->as.pattern_literal.value =
                xr_ast_clone_ctx(node->as.pattern_literal.value, map, mc, clone_ctx);
            break;
        case AST_PATTERN_RANGE:
            n->as.pattern_range.start =
                xr_ast_clone_ctx(node->as.pattern_range.start, map, mc, clone_ctx);
            n->as.pattern_range.end =
                xr_ast_clone_ctx(node->as.pattern_range.end, map, mc, clone_ctx);
            n->as.pattern_range.inclusive_end = node->as.pattern_range.inclusive_end;
            break;
        case AST_PATTERN_WILDCARD:
            break;
        case AST_PATTERN_MULTI:
            n->as.pattern_multi.count = node->as.pattern_multi.count;
            n->as.pattern_multi.patterns = clone_node_array(
                node->as.pattern_multi.patterns, node->as.pattern_multi.count, map, mc, clone_ctx);
            break;
        case AST_PATTERN_TUPLE:
            n->as.pattern_tuple.count = node->as.pattern_tuple.count;
            n->as.pattern_tuple.patterns = clone_node_array(
                node->as.pattern_tuple.patterns, node->as.pattern_tuple.count, map, mc, clone_ctx);
            break;
        case AST_PATTERN_OBJECT:
            n->as.pattern_object.count = node->as.pattern_object.count;
            n->as.pattern_object.field_names =
                clone_str_array(node->as.pattern_object.field_names, node->as.pattern_object.count);
            n->as.pattern_object.patterns =
                clone_node_array(node->as.pattern_object.patterns, node->as.pattern_object.count,
                                 map, mc, clone_ctx);
            break;
        case AST_PATTERN_ARRAY:
            n->as.pattern_array.count = node->as.pattern_array.count;
            n->as.pattern_array.patterns = clone_node_array(
                node->as.pattern_array.patterns, node->as.pattern_array.count, map, mc, clone_ctx);
            n->as.pattern_array.has_rest = node->as.pattern_array.has_rest;
            n->as.pattern_array.rest_name = clone_str(node->as.pattern_array.rest_name);
            n->as.pattern_array.rest_symbol_id = node->as.pattern_array.rest_symbol_id;
            break;
        case AST_PATTERN_TYPE:
            n->as.pattern_type.type = node->as.pattern_type.type;
            n->as.pattern_type.binding_name = clone_str(node->as.pattern_type.binding_name);
            n->as.pattern_type.symbol_id = node->as.pattern_type.symbol_id;
            break;

        // === Coroutine nodes ===
        case AST_GO_EXPR:
            n->as.go_expr.expr = xr_ast_clone_ctx(node->as.go_expr.expr, map, mc, clone_ctx);
            n->as.go_expr.name = clone_str(node->as.go_expr.name);
            n->as.go_expr.link_mode = node->as.go_expr.link_mode;
            n->as.go_expr.spawn_kind = node->as.go_expr.spawn_kind;
            break;
        case AST_AWAIT_EXPR:
            n->as.await_expr.expr = xr_ast_clone_ctx(node->as.await_expr.expr, map, mc, clone_ctx);
            n->as.await_expr.timeout =
                xr_ast_clone_ctx(node->as.await_expr.timeout, map, mc, clone_ctx);
            n->as.await_expr.into = xr_ast_clone_ctx(node->as.await_expr.into, map, mc, clone_ctx);
            n->as.await_expr.is_any = node->as.await_expr.is_any;
            n->as.await_expr.is_all = node->as.await_expr.is_all;
            n->as.await_expr.is_any_success = node->as.await_expr.is_any_success;
            break;
        case AST_UNSAFE_EXPR:
            n->as.unsafe_expr.operand =
                xr_ast_clone_ctx(node->as.unsafe_expr.operand, map, mc, clone_ctx);
            break;
        case AST_CHANNEL_NEW:
            n->as.channel_new.buffer_size =
                xr_ast_clone_ctx(node->as.channel_new.buffer_size, map, mc, clone_ctx);
            break;
        case AST_DEFER_STMT:
            n->as.defer_stmt.body = xr_ast_clone_ctx(node->as.defer_stmt.body, map, mc, clone_ctx);
            break;
        case AST_SCOPE_BLOCK:
            n->as.scope_block.body =
                xr_ast_clone_ctx(node->as.scope_block.body, map, mc, clone_ctx);
            n->as.scope_block.scope_mode = node->as.scope_block.scope_mode;
            break;
        case AST_YIELD_STMT:
            n->as.yield_stmt.value =
                xr_ast_clone_ctx(node->as.yield_stmt.value, map, mc, clone_ctx);
            break;
        case AST_CANCELLED_EXPR:
            break;

        // === Enum nodes ===
        case AST_ENUM_ACCESS:
            n->as.enum_access.enum_name = clone_str(node->as.enum_access.enum_name);
            n->as.enum_access.member_name = clone_str(node->as.enum_access.member_name);
            break;
        case AST_ENUM_INDEX:
            n->as.enum_index.collection =
                xr_ast_clone_ctx(node->as.enum_index.collection, map, mc, clone_ctx);
            n->as.enum_index.index_expr =
                xr_ast_clone_ctx(node->as.enum_index.index_expr, map, mc, clone_ctx);
            break;

        // === Class/struct declaration (deep clone for mono) ===
        case AST_UNION_DECL:
        case AST_STRUCT_DECL:
        case AST_CLASS_DECL: {
            ClassDeclNode *src = (node->type == AST_CLASS_DECL)    ? &node->as.class_decl
                                 : (node->type == AST_STRUCT_DECL) ? &node->as.struct_decl
                                                                   : &node->as.union_decl;
            ClassDeclNode *dst = (n->type == AST_CLASS_DECL)    ? &n->as.class_decl
                                 : (n->type == AST_STRUCT_DECL) ? &n->as.struct_decl
                                                                : &n->as.union_decl;
            dst->name = clone_str(src->name);
            dst->super_name = clone_str(src->super_name);
            dst->super_module = clone_str(src->super_module);
            dst->interface_count = src->interface_count;
            dst->interfaces = clone_tref_array(src->interfaces, src->interface_count, map, mc);
            dst->field_count = src->field_count;
            dst->fields = clone_node_array(src->fields, src->field_count, map, mc, clone_ctx);
            dst->method_count = src->method_count;
            dst->methods = clone_node_array(src->methods, src->method_count, map, mc, clone_ctx);
            dst->explicit_final = src->explicit_final;
            dst->is_packed = src->is_packed;
            dst->explicit_align = src->explicit_align;
            dst->attributes = src->attributes;
            dst->attr_count = src->attr_count;
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
            dst->required_count = src->required_count;
            dst->is_variadic = src->is_variadic;
            dst->params = clone_params(src->params, src->param_count, map, mc, clone_ctx);
            dst->return_type = sub_tref(src->return_type, map, mc);
            dst->body = xr_ast_clone_ctx(src->body, map, mc, clone_ctx);
            dst->is_constructor = src->is_constructor;
            dst->is_static = src->is_static;
            dst->is_private = src->is_private;
            dst->is_protected = src->is_protected;
            dst->is_getter = src->is_getter;
            dst->is_setter = src->is_setter;
            dst->is_static_constructor = src->is_static_constructor;
            dst->attributes = src->attributes;
            dst->attr_count = src->attr_count;
            dst->is_operator = src->is_operator;
            dst->op_type = src->op_type;
            dst->base_arg_count = src->base_arg_count;
            dst->base_args =
                clone_node_array(src->base_args, src->base_arg_count, map, mc, clone_ctx);
            // Class monomorphization substitutes the enclosing class type
            // params (for example T in Box<T>) but must preserve method-local
            // params (for example U in map<U>). Clearing them makes the
            // post-mono analyzer treat U as an ordinary unresolved type name.
            dst->type_params =
                clone_generic_params(src->type_params, src->type_param_count, map, mc);
            dst->type_param_count = src->type_param_count;
            break;
        }

        // === Field declaration (deep clone for mono) ===
        case AST_FIELD_DECL: {
            FieldDeclNode *src = &node->as.field_decl;
            FieldDeclNode *dst = &n->as.field_decl;
            dst->name = clone_str(src->name);
            dst->field_type = sub_tref(src->field_type, map, mc);
            dst->is_private = src->is_private;
            dst->is_protected = src->is_protected;
            dst->is_static = src->is_static;
            dst->is_final = src->is_final;
            dst->is_const = src->is_const;
            dst->is_flexible = src->is_flexible;
            dst->initializer = xr_ast_clone_ctx(src->initializer, map, mc, clone_ctx);
            break;
        }

        // === Struct literal (deep clone for mono) ===
        case AST_STRUCT_LITERAL: {
            StructLiteralNode *src = &node->as.struct_literal;
            StructLiteralNode *dst = &n->as.struct_literal;
            dst->struct_name = clone_str(src->struct_name);
            dst->field_count = src->field_count;
            dst->field_names = clone_str_array(src->field_names, src->field_count);
            dst->field_values =
                clone_node_array(src->field_values, src->field_count, map, mc, clone_ctx);
            dst->type_args = clone_tref_array(src->type_args, src->type_arg_count, map, mc);
            dst->type_arg_count = src->type_arg_count;
            break;
        }

        // === Nodes not typically inside generic bodies (shallow copy) ===
        case AST_INTERFACE_DECL:
        case AST_ENUM_DECL:
        case AST_IMPORT_STMT:
        case AST_EXPORT_STMT:
        case AST_GLOBAL_ASM:
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

AstNode *xr_ast_clone(AstNode *node, XrMonoTypeMap *map, int mc) {
    return xr_ast_clone_ctx(node, map, mc, NULL);
}

AstNode *xr_ast_clone_session(AstNode *node, XrCompilerSession *session) {
    XrAstCloneCtx clone_ctx = {.session = session, .preserve_symbol_ids = true};
    return xr_ast_clone_ctx(node, NULL, 0, &clone_ctx);
}

/* ========== Mono Collector ========== */

void xa_mono_collector_init(XaMonoCollector *c) {
    XR_DCHECK(c != NULL, "xa_mono_collector_init: NULL collector");
    c->instances = NULL;
    c->count = 0;
    c->capacity = 0;
    c->analyzer = NULL;
    c->tref_rewrite_count = 0;
    c->expanding = -1;
    c->budget_reported = false;
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
    c->analyzer = NULL;
    c->expanding = -1;
    c->budget_reported = false;
}

static char *mono_effect_mangle(const char *generic_name, XrTypeRef **type_args, int type_arg_count,
                                XaMonoThrowEffect throw_effect) {
    char *base = xr_mono_mangle(generic_name, type_args, type_arg_count);
    if (!base || throw_effect != XA_MONO_EFFECT_NO_THROW)
        return base;
    size_t len = strlen(base) + sizeof("$nothrow");
    char *result = (char *) xr_malloc(len);
    if (!result) {
        xr_free(base);
        return NULL;
    }
    snprintf(result, len, "%s$nothrow", base);
    xr_free(base);
    return result;
}

/* Render the chain that led to `parent` as "a<i64> -> b<Box<i64>> -> ...".
 * Without it an E0388 names only the deepest type, which is a type the user
 * never wrote and cannot search for. */
static void mono_render_chain(const XaMonoCollector *c, int parent, char *buf, size_t cap) {
    if (!buf || cap == 0)
        return;
    buf[0] = '\0';
    if (parent < 0)
        return;

    /* Walk to the root, then print root-first. The chain is bounded by
     * XR_MONO_MAX_DEPTH, so a fixed index array is exact, not a guess. */
    int chain[XR_MONO_MAX_DEPTH + 1];
    int n = 0;
    for (int i = parent; i >= 0 && n <= XR_MONO_MAX_DEPTH; i = c->instances[i].parent)
        chain[n++] = i;

    /* A chain at the limit is far too long to print whole, and the middle is
     * the least informative part: what the reader needs is where the expansion
     * started and what it is doing now. Keep both ends and elide the rest. */
    const int edge = 6;
    bool elide = n > 2 * edge + 1;

    size_t used = 0;
    for (int pos = 0; pos < n && used < cap; pos++) {
        int i = n - 1 - pos; /* root-first */
        if (elide && pos == edge) {
            int written = snprintf(buf + used, cap - used, " -> ... (%d more) ...", n - 2 * edge);
            if (written < 0)
                return;
            used += (size_t) written;
        }
        if (elide && pos >= edge && pos < n - edge)
            continue;
        int written = snprintf(buf + used, cap - used, "%s%s", used ? " -> " : "",
                               c->instances[chain[i]].mangled_name);
        if (written < 0)
            return;
        used += (size_t) written;
    }
}

static void mono_report(XaMonoCollector *c, int code, const char *message, const XrLocation *loc) {
    /* One budget diagnostic per compile: every later instantiation would repeat
     * the same exhausted budget and bury the first, most actionable one. The
     * flag also tells the pass to fail, so the count of suppressed duplicates
     * never changes the outcome. */
    if (c->budget_reported)
        return;
    c->budget_reported = true;
    XrLocation at = loc ? *loc : (XrLocation) {0};
    xa_analyzer_add_diagnostic(c->analyzer, XR_DIAG_SEV_ERROR, code, message, &at);
}

static const char *xa_mono_collector_add_effect(XaMonoCollector *c, const char *generic_name,
                                                XrTypeRef **type_args, int type_arg_count,
                                                bool is_class_generic,
                                                XaMonoThrowEffect throw_effect,
                                                const XrLocation *loc) {
    if (!c || !generic_name)
        return NULL;

    char *candidate_mangled =
        mono_effect_mangle(generic_name, type_args, type_arg_count, throw_effect);
    if (!candidate_mangled)
        return NULL;

    // Concrete type arguments define instance identity. ABI-equivalent instances
    // may share code only through explicit verified plans, not collector dedup.
    for (int i = 0; i < c->count; i++) {
        if (strcmp(c->instances[i].generic_name, generic_name) == 0 &&
            strcmp(c->instances[i].mangled_name, candidate_mangled) == 0) {
            xr_free(candidate_mangled);
            return c->instances[i].mangled_name;  // Already registered
        }
    }

    int parent = c->expanding;
    int depth = parent >= 0 ? c->instances[parent].depth + 1 : 0;

    /* Depth guard: a specialized body instantiating an ever-larger type has no
     * finite expansion. Dedup cannot catch it -- every round is a new tuple. */
    if (depth > XR_MONO_MAX_DEPTH) {
        char chain[512];
        mono_render_chain(c, parent, chain, sizeof(chain));
        char msg[896];
        snprintf(msg, sizeof(msg),
                 "generic instantiation of '%s' nested deeper than %d levels\n"
                 "  instantiated through: %s -> %s\n"
                 "  note: a generic that instantiates itself at a larger type (f<T> requesting "
                 "f<Box<T>>) has no finite specialization and always reaches this limit",
                 generic_name, XR_MONO_MAX_DEPTH, chain, candidate_mangled);
        mono_report(c, XR_ERR_ANALYZE_MONO_DEPTH, msg, loc);
        xr_free(candidate_mangled);
        return NULL;
    }

    /* Breadth guard: a compile-time memory backstop, not a language rule. */
    if (c->count >= XR_MONO_MAX_INSTANCES) {
        char msg[320];
        snprintf(msg, sizeof(msg),
                 "program exceeds the monomorphization budget of %d generic instances "
                 "(reached while instantiating '%s')",
                 XR_MONO_MAX_INSTANCES, generic_name);
        mono_report(c, XR_ERR_ANALYZE_MONO_BUDGET, msg, loc);
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
    inst->mangled_name = candidate_mangled;
    inst->is_class_generic = is_class_generic;
    inst->throw_effect = throw_effect;
    inst->parent = parent;
    inst->depth = depth;
    return inst->mangled_name;
}

const char *xa_mono_collector_add(XaMonoCollector *c, const char *generic_name,
                                  XrTypeRef **type_args, int type_arg_count, bool is_class_generic,
                                  const XrLocation *loc) {
    return xa_mono_collector_add_effect(c, generic_name, type_args, type_arg_count,
                                        is_class_generic, XA_MONO_EFFECT_NONE, loc);
}

// Lookup the exact concrete instance.
static const char *xa_mono_collector_lookup(XaMonoCollector *c, const char *generic_name,
                                            XrTypeRef **type_args, int type_arg_count,
                                            XaMonoThrowEffect throw_effect) {
    if (!c || !generic_name)
        return NULL;
    char *candidate_mangled =
        mono_effect_mangle(generic_name, type_args, type_arg_count, throw_effect);
    const char *result = NULL;
    for (int i = 0; i < c->count; i++) {
        if (strcmp(c->instances[i].generic_name, generic_name) != 0)
            continue;
        if (candidate_mangled && strcmp(c->instances[i].mangled_name, candidate_mangled) == 0) {
            result = c->instances[i].mangled_name;
            break;
        }
    }
    xr_free(candidate_mangled);
    return result;
}

// task-221 gap C: rewrite a type annotation naming a monomorphized generic
// instance (e.g. RouteMatch<int>) to its mangled name (RouteMatch$i64), so a
// specialized method/function's declared return/param/var types match the
// specialized values its body constructs. Recurses into nested type arguments
// first. The mangled name is owned by the collector and lives for the compile.
static void mono_rewrite_type_ref(XrTypeRef *tref, XaMonoCollector *collector) {
    if (!tref)
        return;
    for (int i = 0; i < tref->nchildren; i++)
        mono_rewrite_type_ref(tref->children[i], collector);
    if (tref->kind == XR_TREF_GENERIC && tref->name && tref->nchildren > 0) {
        const char *mangled = xa_mono_collector_lookup(collector, tref->name, tref->children,
                                                       tref->nchildren, XA_MONO_EFFECT_NONE);
        if (mangled) {
            // The collector's mangled_name is freed when the mono pass ends, but
            // this type ref must survive into post-monomorphization analysis and
            // cgen; copy it (compile-lifetime, matching inject_mono_decls' clone
            // naming via xr_strdup).
            tref->kind = XR_TREF_NAMED;
            tref->name = xr_strdup(mangled);
            tref->children = NULL;
            tref->nchildren = 0;
            collector->tref_rewrite_count++;
        }
    }
}

/* ========== Mono Pass Collect + Instantiate + Rewrite ========== */

// Generic declaration registry: maps generic name → AST node
typedef struct {
    const char *name;
    AstNode *node;  // AST_FUNCTION_DECL or AST_CLASS_DECL
    XrGenericParam **type_params;
    int type_param_count;
    bool is_external;
    bool inject_clone;
    bool rewrite_member_access;
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

static void registry_add_ex(XaGenericRegistry *r, const char *name, AstNode *node,
                            XrGenericParam **tp, int tp_count, bool is_external, bool inject_clone,
                            bool rewrite_member_access) {
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
    d->is_external = is_external;
    d->inject_clone = inject_clone;
    d->rewrite_member_access = rewrite_member_access;
}

static void registry_add_local(XaGenericRegistry *r, const char *name, AstNode *node,
                               XrGenericParam **tp, int tp_count) {
    registry_add_ex(r, name, node, tp, tp_count, false, true, false);
}

static void registry_add_external(XaGenericRegistry *r, const char *name, AstNode *node,
                                  XrGenericParam **tp, int tp_count, bool inject_clone,
                                  bool rewrite_member_access) {
    registry_add_ex(r, name, node, tp, tp_count, true, inject_clone, rewrite_member_access);
}

static XaGenericDecl *registry_find(XaGenericRegistry *r, const char *name) {
    for (int i = 0; i < r->count; i++) {
        if (r->decls[i].name && name && strcmp(r->decls[i].name, name) == 0)
            return &r->decls[i];
    }
    return NULL;
}

// External modules contribute generic value-struct templates to the using
// module, and generic class/function names for namespace-call rewriting. Class
// and function bodies are injected only into their defining module by scanning
// cross-module instantiation roots with that module's local registry.
static void collect_external_generic_decls(AstNode *root, XaGenericRegistry *registry) {
    if (!root || root->type != AST_PROGRAM)
        return;

    ProgramNode *prog = &root->as.program;
    for (int i = 0; i < prog->count; i++) {
        AstNode *stmt = prog->statements[i];
        if (!stmt)
            continue;
        if (stmt->type == AST_FUNCTION_DECL && stmt->as.function_decl.type_param_count > 0) {
            if (!registry_find(registry, stmt->as.function_decl.name)) {
                registry_add_external(registry, stmt->as.function_decl.name, stmt,
                                      stmt->as.function_decl.type_params,
                                      stmt->as.function_decl.type_param_count, false, true);
            }
            continue;
        }
        if (stmt->type == AST_CLASS_DECL && stmt->as.class_decl.type_param_count > 0) {
            if (!registry_find(registry, stmt->as.class_decl.name)) {
                registry_add_external(registry, stmt->as.class_decl.name, stmt,
                                      stmt->as.class_decl.type_params,
                                      stmt->as.class_decl.type_param_count, false, true);
            }
            continue;
        }
        if (stmt->type == AST_STRUCT_DECL && stmt->as.struct_decl.type_param_count > 0) {
            if (!registry_find(registry, stmt->as.struct_decl.name)) {
                registry_add_external(registry, stmt->as.struct_decl.name, stmt,
                                      stmt->as.struct_decl.type_params,
                                      stmt->as.struct_decl.type_param_count, true, false);
            }
            continue;
        }
    }
}

typedef struct {
    const char **names;
    int count;
    int capacity;
} XaMonoImportAliases;

static void mono_import_aliases_init(XaMonoImportAliases *aliases) {
    if (!aliases)
        return;
    aliases->names = NULL;
    aliases->count = 0;
    aliases->capacity = 0;
}

static void mono_import_aliases_free(XaMonoImportAliases *aliases) {
    if (!aliases)
        return;
    xr_free(aliases->names);
    aliases->names = NULL;
    aliases->count = 0;
    aliases->capacity = 0;
}

static bool mono_import_aliases_contains(const XaMonoImportAliases *aliases, const char *name) {
    if (!aliases || !name)
        return false;
    for (int i = 0; i < aliases->count; i++) {
        if (aliases->names[i] && strcmp(aliases->names[i], name) == 0)
            return true;
    }
    return false;
}

static void mono_import_aliases_add(XaMonoImportAliases *aliases, const char *name) {
    if (!aliases || !name || mono_import_aliases_contains(aliases, name))
        return;
    if (aliases->count >= aliases->capacity) {
        int new_cap = aliases->capacity ? aliases->capacity * 2 : 4;
        const char **new_names =
            (const char **) xr_realloc(aliases->names, (size_t) new_cap * sizeof(const char *));
        if (!new_names)
            return;
        aliases->names = new_names;
        aliases->capacity = new_cap;
    }
    aliases->names[aliases->count++] = name;
}

static void collect_import_aliases(AstNode *root, XaMonoImportAliases *aliases) {
    if (!root || root->type != AST_PROGRAM || !aliases)
        return;
    ProgramNode *prog = &root->as.program;
    for (int i = 0; i < prog->count; i++) {
        AstNode *stmt = prog->statements[i];
        if (!stmt || stmt->type != AST_IMPORT_STMT)
            continue;
        ImportStmtNode *import = &stmt->as.import_stmt;
        if (import->member_count != 0)
            continue;
        mono_import_aliases_add(aliases, import->alias ? import->alias : import->module_name);
    }
}

static bool mono_call_is_import_member_generic(const CallExprNode *call,
                                               const XaMonoImportAliases *aliases,
                                               const char **out_member_name) {
    if (out_member_name)
        *out_member_name = NULL;
    if (!call || call->type_arg_count <= 0 || !call->callee ||
        call->callee->type != AST_MEMBER_ACCESS)
        return false;
    AstNode *object = call->callee->as.member_access.object;
    if (!object || object->type != AST_VARIABLE ||
        !mono_import_aliases_contains(aliases, object->as.variable.name))
        return false;
    if (out_member_name)
        *out_member_name = call->callee->as.member_access.name;
    return call->callee->as.member_access.name != NULL;
}

/* Compute the single aggregate effect argument for a generic HOF call.  Only
 * unqualified function parameters are effect-polymorphic. A compiler-inferred
 * fixed constraint does not create another body dimension. Unknown actuals
 * choose MAY_THROW (fail closed). */
static XaMonoThrowEffect mono_call_throw_effect(const XaGenericDecl *decl, const CallExprNode *call,
                                                const XaMonoCollector *collector) {
    if (!decl || !decl->node || decl->node->type != AST_FUNCTION_DECL || !call)
        return XA_MONO_EFFECT_NONE;
    const FunctionDeclNode *fn = &decl->node->as.function_decl;
    bool has_poly_callback = false;
    bool all_no_throw = true;
    int limit = fn->param_count < call->arg_count ? fn->param_count : call->arg_count;
    for (int i = 0; i < fn->param_count; i++) {
        const XrParamNode *param = fn->params ? fn->params[i] : NULL;
        const XrTypeRef *type = param ? param->type : NULL;
        if (!type || type->kind != XR_TREF_FUNCTION || type->requires_nothrow)
            continue;
        has_poly_callback = true;
        if (i >= limit || !collector || !collector->analyzer) {
            all_no_throw = false;
            continue;
        }
        XrType *arg_type = xa_analyzer_get_node_type(collector->analyzer, call->arguments[i]);
        if (!xr_type_function_is_no_throw(arg_type))
            all_no_throw = false;
    }
    if (!has_poly_callback)
        return XA_MONO_EFFECT_NONE;
    return all_no_throw ? XA_MONO_EFFECT_NO_THROW : XA_MONO_EFFECT_MAY_THROW;
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
                registry_add_local(registry, stmt->as.function_decl.name, stmt,
                                   stmt->as.function_decl.type_params,
                                   stmt->as.function_decl.type_param_count);
            }
            // Generic class: class Box<T> { ... }
            if (stmt->type == AST_CLASS_DECL && stmt->as.class_decl.type_param_count > 0) {
                registry_add_local(registry, stmt->as.class_decl.name, stmt,
                                   stmt->as.class_decl.type_params,
                                   stmt->as.class_decl.type_param_count);
            }
            // Generic struct: struct Pair<T, U> { ... }
            if (stmt->type == AST_STRUCT_DECL && stmt->as.struct_decl.type_param_count > 0) {
                registry_add_local(registry, stmt->as.struct_decl.name, stmt,
                                   stmt->as.struct_decl.type_params,
                                   stmt->as.struct_decl.type_param_count);
            }
        }
    }
}

/* Source location of the instantiation site, so a budget diagnostic points at
 * the code the user wrote rather than at the generic's declaration. */
static XrLocation mono_node_loc(const AstNode *node) {
    XrLocation loc = {0};
    if (!node)
        return loc;
    loc.line = (uint32_t) (node->line > 0 ? node->line : 0);
    loc.column = (uint32_t) (node->column > 0 ? node->column : 0);
    loc.end_line = (uint32_t) (node->end_line > 0 ? node->end_line : 0);
    loc.end_column = (uint32_t) (node->end_column > 0 ? node->end_column : 0);
    return loc;
}

// Phase 2: Walk AST to find generic call sites (CallExpr with type_args)
static void collect_instantiation_sites(AstNode *node, XaGenericRegistry *registry,
                                        XaMonoCollector *collector,
                                        const XaMonoImportAliases *import_aliases,
                                        bool local_only) {
    if (!node)
        return;

    // Check call expression with explicit type arguments
    if (node->type == AST_CALL_EXPR) {
        CallExprNode *call = &node->as.call_expr;
        XrLocation loc = mono_node_loc(node);
        if (call->type_arg_count > 0 && call->callee && call->callee->type == AST_VARIABLE) {
            const char *fn_name = call->callee->as.variable.name;
            XaGenericDecl *decl = registry_find(registry, fn_name);
            if (decl && (!local_only || !decl->is_external) &&
                decl->type_param_count == call->type_arg_count) {
                bool is_cls =
                    (decl->node->type == AST_CLASS_DECL || decl->node->type == AST_STRUCT_DECL);
                XaMonoThrowEffect effect = mono_call_throw_effect(decl, call, collector);
                xa_mono_collector_add_effect(collector, fn_name, call->type_args,
                                             call->type_arg_count, is_cls, effect, &loc);
            }
        }
        const char *member_name = NULL;
        if (mono_call_is_import_member_generic(call, import_aliases, &member_name)) {
            XaGenericDecl *decl = registry_find(registry, member_name);
            if (decl && (!local_only || !decl->is_external) &&
                decl->type_param_count == call->type_arg_count) {
                bool is_cls =
                    (decl->node->type == AST_CLASS_DECL || decl->node->type == AST_STRUCT_DECL);
                XaMonoThrowEffect effect = mono_call_throw_effect(decl, call, collector);
                xa_mono_collector_add_effect(collector, member_name, call->type_args,
                                             call->type_arg_count, is_cls, effect, &loc);
            }
        }
        // Recurse into callee and arguments
        collect_instantiation_sites(call->callee, registry, collector, import_aliases, local_only);
        for (int i = 0; i < call->arg_count; i++)
            collect_instantiation_sites(call->arguments[i], registry, collector, import_aliases,
                                        local_only);
        return;
    }

    // Check new expression with type arguments
    if (node->type == AST_NEW_EXPR) {
        NewExprNode *ne = &node->as.new_expr;
        if (ne->class_name &&
            (strcmp(ne->class_name, "Ptr") == 0 || strcmp(ne->class_name, "MutPtr") == 0))
            return;
        if (ne->type_arg_count > 0) {
            XaGenericDecl *decl = registry_find(registry, ne->class_name);
            if (decl && (!local_only || !decl->is_external) &&
                decl->type_param_count == ne->type_arg_count) {
                XrLocation loc = mono_node_loc(node);
                xa_mono_collector_add(collector, ne->class_name, ne->type_args, ne->type_arg_count,
                                      true, &loc);
            }
        }
        for (int i = 0; i < ne->arg_count; i++)
            collect_instantiation_sites(ne->arguments[i], registry, collector, import_aliases,
                                        local_only);
        return;
    }

    // Check struct literal with type arguments: Pair<int, string>{...}
    if (node->type == AST_STRUCT_LITERAL) {
        StructLiteralNode *sl = &node->as.struct_literal;
        if (sl->type_arg_count > 0 && sl->struct_name) {
            XaGenericDecl *decl = registry_find(registry, sl->struct_name);
            if (decl && (!local_only || !decl->is_external) &&
                decl->type_param_count == sl->type_arg_count) {
                XrLocation loc = mono_node_loc(node);
                xa_mono_collector_add(collector, sl->struct_name, sl->type_args, sl->type_arg_count,
                                      true, &loc);
            }
        }
        for (int i = 0; i < sl->field_count; i++)
            collect_instantiation_sites(sl->field_values[i], registry, collector, import_aliases,
                                        local_only);
        return;
    }

    // Generic recursive walk for all other node types
    switch (node->type) {
        case AST_PROGRAM:
            for (int i = 0; i < node->as.program.count; i++)
                collect_instantiation_sites(node->as.program.statements[i], registry, collector,
                                            import_aliases, local_only);
            break;
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count; i++)
                collect_instantiation_sites(node->as.block.statements[i], registry, collector,
                                            import_aliases, local_only);
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
        case AST_NULLISH_COALESCE:
            collect_instantiation_sites(node->as.binary.left, registry, collector, import_aliases,
                                        local_only);
            collect_instantiation_sites(node->as.binary.right, registry, collector, import_aliases,
                                        local_only);
            break;
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
        case AST_FORCE_UNWRAP:
            collect_instantiation_sites(node->as.unary.operand, registry, collector, import_aliases,
                                        local_only);
            break;
        case AST_EXPR_STMT:
            collect_instantiation_sites(node->as.expr_stmt, registry, collector, import_aliases,
                                        local_only);
            break;
        case AST_GROUPING:
            collect_instantiation_sites(node->as.grouping, registry, collector, import_aliases,
                                        local_only);
            break;
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            collect_instantiation_sites(node->as.var_decl.initializer, registry, collector,
                                        import_aliases, local_only);
            break;
        case AST_ASSIGNMENT:
            collect_instantiation_sites(node->as.assignment.value, registry, collector,
                                        import_aliases, local_only);
            break;
        case AST_COMPOUND_ASSIGNMENT:
            collect_instantiation_sites(node->as.compound_assignment.value, registry, collector,
                                        import_aliases, local_only);
            collect_instantiation_sites(node->as.compound_assignment.object, registry, collector,
                                        import_aliases, local_only);
            break;
        case AST_IF_STMT:
            collect_instantiation_sites(node->as.if_stmt.condition, registry, collector,
                                        import_aliases, local_only);
            collect_instantiation_sites(node->as.if_stmt.then_branch, registry, collector,
                                        import_aliases, local_only);
            collect_instantiation_sites(node->as.if_stmt.else_branch, registry, collector,
                                        import_aliases, local_only);
            break;
        case AST_WHILE_STMT:
            collect_instantiation_sites(node->as.while_stmt.condition, registry, collector,
                                        import_aliases, local_only);
            collect_instantiation_sites(node->as.while_stmt.body, registry, collector,
                                        import_aliases, local_only);
            break;
        case AST_FOR_STMT:
            collect_instantiation_sites(node->as.for_stmt.initializer, registry, collector,
                                        import_aliases, local_only);
            collect_instantiation_sites(node->as.for_stmt.condition, registry, collector,
                                        import_aliases, local_only);
            collect_instantiation_sites(node->as.for_stmt.increment, registry, collector,
                                        import_aliases, local_only);
            collect_instantiation_sites(node->as.for_stmt.body, registry, collector, import_aliases,
                                        local_only);
            break;
        case AST_FOR_IN_STMT:
            collect_instantiation_sites(node->as.for_in_stmt.collection, registry, collector,
                                        import_aliases, local_only);
            collect_instantiation_sites(node->as.for_in_stmt.body, registry, collector,
                                        import_aliases, local_only);
            break;
        case AST_RETURN_STMT:
            for (int i = 0; i < node->as.return_stmt.value_count; i++)
                collect_instantiation_sites(node->as.return_stmt.values[i], registry, collector,
                                            import_aliases, local_only);
            break;
        case AST_FUNCTION_DECL:
        case AST_FUNCTION_EXPR:
            collect_instantiation_sites(node->as.function_decl.body, registry, collector,
                                        import_aliases, local_only);
            break;
        case AST_METHOD_DECL:
            collect_instantiation_sites(node->as.method_decl.body, registry, collector,
                                        import_aliases, local_only);
            for (int i = 0; i < node->as.method_decl.base_arg_count; i++)
                collect_instantiation_sites(node->as.method_decl.base_args[i], registry, collector,
                                            import_aliases, local_only);
            break;
        case AST_CLASS_DECL: {
            // task-221 gap C: recurse into class method/field bodies so generic
            // constructions inside methods are collected. Only descend into
            // non-generic classes and already-monomorphized clones
            // (type_param_count == 0); a generic skeleton's method bodies still
            // reference the class type params (e.g. RouteMatch<T>), which must be
            // specialized via the enclosing class's clone, not collected as bare
            // type-parameter instantiations.
            ClassDeclNode *cd = &node->as.class_decl;
            if (cd->type_param_count == 0) {
                for (int i = 0; i < cd->method_count; i++)
                    collect_instantiation_sites(cd->methods[i], registry, collector, import_aliases,
                                                local_only);
                for (int i = 0; i < cd->field_count; i++)
                    collect_instantiation_sites(cd->fields[i], registry, collector, import_aliases,
                                                local_only);
            }
            break;
        }
        case AST_PRINT_STMT:
            for (int i = 0; i < node->as.print_stmt.expr_count; i++)
                collect_instantiation_sites(node->as.print_stmt.exprs[i], registry, collector,
                                            import_aliases, local_only);
            break;
        case AST_ARRAY_LITERAL:
            if (node->as.array_literal.is_repeat) {
                collect_instantiation_sites(node->as.array_literal.repeat_value, registry,
                                            collector, import_aliases, local_only);
                collect_instantiation_sites(node->as.array_literal.repeat_count, registry,
                                            collector, import_aliases, local_only);
            } else {
                for (int i = 0; i < node->as.array_literal.count; i++)
                    collect_instantiation_sites(node->as.array_literal.elements[i], registry,
                                                collector, import_aliases, local_only);
            }
            break;
        case AST_INDEX_GET:
            collect_instantiation_sites(node->as.index_get.array, registry, collector,
                                        import_aliases, local_only);
            collect_instantiation_sites(node->as.index_get.index, registry, collector,
                                        import_aliases, local_only);
            break;
        case AST_INDEX_SET:
            collect_instantiation_sites(node->as.index_set.array, registry, collector,
                                        import_aliases, local_only);
            collect_instantiation_sites(node->as.index_set.index, registry, collector,
                                        import_aliases, local_only);
            collect_instantiation_sites(node->as.index_set.value, registry, collector,
                                        import_aliases, local_only);
            break;
        case AST_MEMBER_ACCESS:
            collect_instantiation_sites(node->as.member_access.object, registry, collector,
                                        import_aliases, local_only);
            break;
        case AST_MEMBER_SET:
            collect_instantiation_sites(node->as.member_set.object, registry, collector,
                                        import_aliases, local_only);
            collect_instantiation_sites(node->as.member_set.value, registry, collector,
                                        import_aliases, local_only);
            break;
        case AST_TERNARY:
            collect_instantiation_sites(node->as.ternary.condition, registry, collector,
                                        import_aliases, local_only);
            collect_instantiation_sites(node->as.ternary.true_expr, registry, collector,
                                        import_aliases, local_only);
            collect_instantiation_sites(node->as.ternary.false_expr, registry, collector,
                                        import_aliases, local_only);
            break;
        case AST_TEMPLATE_STRING:
            for (int i = 0; i < node->as.template_str.part_count; i++)
                collect_instantiation_sites(node->as.template_str.parts[i], registry, collector,
                                            import_aliases, local_only);
            break;
        case AST_TRY_CATCH:
            collect_instantiation_sites(node->as.try_catch.try_body, registry, collector,
                                        import_aliases, local_only);
            for (int ci = 0; ci < node->as.try_catch.catch_count; ci++) {
                XrCatchClause *cc = node->as.try_catch.catch_clauses[ci];
                if (cc)
                    collect_instantiation_sites(cc->body, registry, collector, import_aliases,
                                                local_only);
            }
            break;
        case AST_THROW_STMT:
            collect_instantiation_sites(node->as.throw_stmt.expression, registry, collector,
                                        import_aliases, local_only);
            break;
        case AST_EXPORT_STMT:
            /* Re-exports contain no local generic body. */
            break;
        case AST_MATCH_EXPR:
            collect_instantiation_sites(node->as.match_expr.expr, registry, collector,
                                        import_aliases, local_only);
            for (int i = 0; i < node->as.match_expr.arm_count; i++)
                collect_instantiation_sites(node->as.match_expr.arms[i], registry, collector,
                                            import_aliases, local_only);
            break;
        case AST_MATCH_ARM:
            collect_instantiation_sites(node->as.match_arm.guard, registry, collector,
                                        import_aliases, local_only);
            collect_instantiation_sites(node->as.match_arm.body, registry, collector,
                                        import_aliases, local_only);
            break;
        case AST_IS_EXPR:
            collect_instantiation_sites(node->as.is_expr.expr, registry, collector, import_aliases,
                                        local_only);
            break;
        case AST_AS_EXPR:
            collect_instantiation_sites(node->as.as_expr.expr, registry, collector, import_aliases,
                                        local_only);
            break;
        case AST_COMPTIME_EXPR:
            collect_instantiation_sites(node->as.comptime_expr.expr, registry, collector,
                                        import_aliases, local_only);
            break;
        case AST_GO_EXPR:
            collect_instantiation_sites(node->as.go_expr.expr, registry, collector, import_aliases,
                                        local_only);
            break;
        case AST_AWAIT_EXPR:
            collect_instantiation_sites(node->as.await_expr.expr, registry, collector,
                                        import_aliases, local_only);
            collect_instantiation_sites(node->as.await_expr.into, registry, collector,
                                        import_aliases, local_only);
            break;
        case AST_UNSAFE_EXPR:
            collect_instantiation_sites(node->as.unsafe_expr.operand, registry, collector,
                                        import_aliases, local_only);
            break;
        case AST_SCOPE_BLOCK:
            collect_instantiation_sites(node->as.scope_block.body, registry, collector,
                                        import_aliases, local_only);
            break;
        case AST_DEFER_STMT:
            collect_instantiation_sites(node->as.defer_stmt.body, registry, collector,
                                        import_aliases, local_only);
            break;
        case AST_YIELD_STMT:
            collect_instantiation_sites(node->as.yield_stmt.value, registry, collector,
                                        import_aliases, local_only);
            break;
        default:
            break;
    }
}

// Phase 3: Rewrite call sites — replace callee name with mangled name
static void rewrite_call_sites(AstNode *node, XaGenericRegistry *registry,
                               XaMonoCollector *collector,
                               const XaMonoImportAliases *import_aliases) {
    if (!node)
        return;

    if (node->type == AST_CALL_EXPR) {
        CallExprNode *call = &node->as.call_expr;
        if (call->type_arg_count > 0 && call->callee && call->callee->type == AST_VARIABLE) {
            const char *fn_name = call->callee->as.variable.name;
            XaGenericDecl *decl = registry_find(registry, fn_name);
            if (decl) {
                XaMonoThrowEffect effect = mono_call_throw_effect(decl, call, collector);
                const char *mangled = xa_mono_collector_lookup(collector, fn_name, call->type_args,
                                                               call->type_arg_count, effect);
                if (mangled) {
                    // Replace callee variable name.
                    // Note: old name is arena-allocated, do not free.
                    call->callee->as.variable.name = xr_strdup(mangled);
                    if (decl->inject_clone)
                        call->callee->as.variable.symbol_id = 0;
                    // Clear type args (no longer generic call)
                    call->type_args = NULL;
                    call->type_arg_count = 0;
                }
            }
        }
        const char *member_name = NULL;
        if (mono_call_is_import_member_generic(call, import_aliases, &member_name)) {
            XaGenericDecl *decl = registry_find(registry, member_name);
            if (decl && decl->rewrite_member_access) {
                XaMonoThrowEffect effect = mono_call_throw_effect(decl, call, collector);
                const char *mangled = xa_mono_collector_lookup(
                    collector, member_name, call->type_args, call->type_arg_count, effect);
                if (mangled) {
                    call->callee->as.member_access.name = xr_strdup(mangled);
                    call->type_args = NULL;
                    call->type_arg_count = 0;
                }
            }
        }
        // Recurse
        rewrite_call_sites(call->callee, registry, collector, import_aliases);
        for (int i = 0; i < call->arg_count; i++)
            rewrite_call_sites(call->arguments[i], registry, collector, import_aliases);
        return;
    }

    // Rewrite new ClassName<T>(...) → new MangledName(...)
    if (node->type == AST_NEW_EXPR) {
        NewExprNode *ne = &node->as.new_expr;
        if (ne->class_name &&
            (strcmp(ne->class_name, "Ptr") == 0 || strcmp(ne->class_name, "MutPtr") == 0))
            return;
        if (ne->type_arg_count > 0 && ne->class_name) {
            XaGenericDecl *decl = registry_find(registry, ne->class_name);
            if (decl) {
                const char *mangled =
                    xa_mono_collector_lookup(collector, ne->class_name, ne->type_args,
                                             ne->type_arg_count, XA_MONO_EFFECT_NONE);
                if (mangled) {
                    // Old class_name is arena-allocated, do not free.
                    ne->class_name = xr_strdup(mangled);
                    // Keep type_args/type_arg_count for display and diagnostics.
                }
            }
        }
        for (int i = 0; i < ne->arg_count; i++)
            rewrite_call_sites(ne->arguments[i], registry, collector, import_aliases);
        return;
    }

    // Rewrite StructName<T>{...} → MangledName{...}
    if (node->type == AST_STRUCT_LITERAL) {
        StructLiteralNode *sl = &node->as.struct_literal;
        if (sl->type_arg_count > 0 && sl->struct_name) {
            XaGenericDecl *decl = registry_find(registry, sl->struct_name);
            if (decl) {
                const char *mangled =
                    xa_mono_collector_lookup(collector, sl->struct_name, sl->type_args,
                                             sl->type_arg_count, XA_MONO_EFFECT_NONE);
                if (mangled) {
                    // Old struct_name is arena-allocated, do not free.
                    sl->struct_name = xr_strdup(mangled);
                    sl->type_args = NULL;
                    sl->type_arg_count = 0;
                }
            }
        }
        for (int i = 0; i < sl->field_count; i++)
            rewrite_call_sites(sl->field_values[i], registry, collector, import_aliases);
        return;
    }

    // Recursive walk (same structure as collect_instantiation_sites)
    switch (node->type) {
        case AST_PROGRAM:
            for (int i = 0; i < node->as.program.count; i++)
                rewrite_call_sites(node->as.program.statements[i], registry, collector,
                                   import_aliases);
            break;
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count; i++)
                rewrite_call_sites(node->as.block.statements[i], registry, collector,
                                   import_aliases);
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
        case AST_NULLISH_COALESCE:
            rewrite_call_sites(node->as.binary.left, registry, collector, import_aliases);
            rewrite_call_sites(node->as.binary.right, registry, collector, import_aliases);
            break;
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
        case AST_FORCE_UNWRAP:
            rewrite_call_sites(node->as.unary.operand, registry, collector, import_aliases);
            break;
        case AST_EXPR_STMT:
            rewrite_call_sites(node->as.expr_stmt, registry, collector, import_aliases);
            break;
        case AST_GROUPING:
            rewrite_call_sites(node->as.grouping, registry, collector, import_aliases);
            break;
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            mono_rewrite_type_ref(node->as.var_decl.type_annotation, collector);
            rewrite_call_sites(node->as.var_decl.initializer, registry, collector, import_aliases);
            break;
        case AST_ASSIGNMENT:
            rewrite_call_sites(node->as.assignment.value, registry, collector, import_aliases);
            break;
        case AST_IF_STMT:
            rewrite_call_sites(node->as.if_stmt.condition, registry, collector, import_aliases);
            rewrite_call_sites(node->as.if_stmt.then_branch, registry, collector, import_aliases);
            rewrite_call_sites(node->as.if_stmt.else_branch, registry, collector, import_aliases);
            break;
        case AST_WHILE_STMT:
            rewrite_call_sites(node->as.while_stmt.condition, registry, collector, import_aliases);
            rewrite_call_sites(node->as.while_stmt.body, registry, collector, import_aliases);
            break;
        case AST_FOR_STMT:
            rewrite_call_sites(node->as.for_stmt.initializer, registry, collector, import_aliases);
            rewrite_call_sites(node->as.for_stmt.condition, registry, collector, import_aliases);
            rewrite_call_sites(node->as.for_stmt.increment, registry, collector, import_aliases);
            rewrite_call_sites(node->as.for_stmt.body, registry, collector, import_aliases);
            break;
        case AST_FOR_IN_STMT:
            rewrite_call_sites(node->as.for_in_stmt.collection, registry, collector,
                               import_aliases);
            rewrite_call_sites(node->as.for_in_stmt.body, registry, collector, import_aliases);
            break;
        case AST_RETURN_STMT:
            for (int i = 0; i < node->as.return_stmt.value_count; i++)
                rewrite_call_sites(node->as.return_stmt.values[i], registry, collector,
                                   import_aliases);
            break;
        case AST_FUNCTION_DECL:
        case AST_FUNCTION_EXPR:
            mono_rewrite_type_ref(node->as.function_decl.return_type, collector);
            for (int i = 0; i < node->as.function_decl.param_count; i++)
                if (node->as.function_decl.params[i])
                    mono_rewrite_type_ref(node->as.function_decl.params[i]->type, collector);
            rewrite_call_sites(node->as.function_decl.body, registry, collector, import_aliases);
            break;
        case AST_METHOD_DECL:
            mono_rewrite_type_ref(node->as.method_decl.return_type, collector);
            for (int i = 0; i < node->as.method_decl.param_count; i++)
                if (node->as.method_decl.params[i])
                    mono_rewrite_type_ref(node->as.method_decl.params[i]->type, collector);
            rewrite_call_sites(node->as.method_decl.body, registry, collector, import_aliases);
            for (int i = 0; i < node->as.method_decl.base_arg_count; i++)
                rewrite_call_sites(node->as.method_decl.base_args[i], registry, collector,
                                   import_aliases);
            break;
        case AST_CLASS_DECL:
        case AST_STRUCT_DECL: {
            // task-221 gap C: rewrite generic call sites inside class method/field
            // bodies (e.g. a monomorphized Router$i64.wrap constructing
            // RouteMatch<int> must become RouteMatch$i64). Mirror the collect
            // pass: only descend into non-generic classes and monomorphized
            // clones (type_param_count == 0); generic skeletons are not emitted.
            // AST_STRUCT_DECL shares ClassDeclNode and the same rule.
            ClassDeclNode *cd =
                node->type == AST_CLASS_DECL ? &node->as.class_decl : &node->as.struct_decl;
            if (cd->type_param_count == 0) {
                uint32_t rewrites_before = collector->tref_rewrite_count;
                for (int i = 0; i < cd->method_count; i++)
                    rewrite_call_sites(cd->methods[i], registry, collector, import_aliases);
                for (int i = 0; i < cd->field_count; i++)
                    rewrite_call_sites(cd->fields[i], registry, collector, import_aliases);
                // Member signatures changed under this declaration, so the class
                // info the first analysis pass collected (which still names the
                // pre-mono Box<int>) is stale. The post-mono pass re-collects
                // declarations carrying this flag.
                if (collector->tref_rewrite_count != rewrites_before)
                    cd->mono_types_rewritten = true;
            }
            break;
        }
        case AST_FIELD_DECL:
            mono_rewrite_type_ref(node->as.field_decl.field_type, collector);
            rewrite_call_sites(node->as.field_decl.initializer, registry, collector,
                               import_aliases);
            break;
        case AST_ENUM_DECL: {
            EnumDeclNode *ed = &node->as.enum_decl;
            if (ed->type_param_count == 0) {
                for (int i = 0; i < ed->member_count; i++)
                    rewrite_call_sites(ed->members[i], registry, collector, import_aliases);
                for (int i = 0; i < ed->method_count; i++)
                    rewrite_call_sites(ed->methods[i], registry, collector, import_aliases);
            }
            break;
        }
        case AST_ENUM_MEMBER: {
            EnumMemberNode *em = &node->as.enum_member;
            for (int i = 0; i < em->payload_count; i++)
                if (em->payload_types)
                    mono_rewrite_type_ref(em->payload_types[i], collector);
            break;
        }
        case AST_PRINT_STMT:
            for (int i = 0; i < node->as.print_stmt.expr_count; i++)
                rewrite_call_sites(node->as.print_stmt.exprs[i], registry, collector,
                                   import_aliases);
            break;
        case AST_ARRAY_LITERAL:
            if (node->as.array_literal.is_repeat) {
                rewrite_call_sites(node->as.array_literal.repeat_value, registry, collector,
                                   import_aliases);
                rewrite_call_sites(node->as.array_literal.repeat_count, registry, collector,
                                   import_aliases);
            } else {
                for (int i = 0; i < node->as.array_literal.count; i++)
                    rewrite_call_sites(node->as.array_literal.elements[i], registry, collector,
                                       import_aliases);
            }
            break;
        case AST_INDEX_GET:
            rewrite_call_sites(node->as.index_get.array, registry, collector, import_aliases);
            rewrite_call_sites(node->as.index_get.index, registry, collector, import_aliases);
            break;
        case AST_INDEX_SET:
            rewrite_call_sites(node->as.index_set.array, registry, collector, import_aliases);
            rewrite_call_sites(node->as.index_set.index, registry, collector, import_aliases);
            rewrite_call_sites(node->as.index_set.value, registry, collector, import_aliases);
            break;
        case AST_MEMBER_ACCESS:
            rewrite_call_sites(node->as.member_access.object, registry, collector, import_aliases);
            break;
        case AST_MEMBER_SET:
            rewrite_call_sites(node->as.member_set.object, registry, collector, import_aliases);
            rewrite_call_sites(node->as.member_set.value, registry, collector, import_aliases);
            break;
        case AST_TERNARY:
            rewrite_call_sites(node->as.ternary.condition, registry, collector, import_aliases);
            rewrite_call_sites(node->as.ternary.true_expr, registry, collector, import_aliases);
            rewrite_call_sites(node->as.ternary.false_expr, registry, collector, import_aliases);
            break;
        case AST_TEMPLATE_STRING:
            for (int i = 0; i < node->as.template_str.part_count; i++)
                rewrite_call_sites(node->as.template_str.parts[i], registry, collector,
                                   import_aliases);
            break;
        case AST_TRY_CATCH:
            rewrite_call_sites(node->as.try_catch.try_body, registry, collector, import_aliases);
            for (int ci = 0; ci < node->as.try_catch.catch_count; ci++) {
                XrCatchClause *cc = node->as.try_catch.catch_clauses[ci];
                if (cc)
                    rewrite_call_sites(cc->body, registry, collector, import_aliases);
            }
            break;
        case AST_THROW_STMT:
            rewrite_call_sites(node->as.throw_stmt.expression, registry, collector, import_aliases);
            break;
        case AST_EXPORT_STMT:
            /* Re-exports contain no local calls. */
            break;
        case AST_MATCH_EXPR:
            rewrite_call_sites(node->as.match_expr.expr, registry, collector, import_aliases);
            for (int i = 0; i < node->as.match_expr.arm_count; i++)
                rewrite_call_sites(node->as.match_expr.arms[i], registry, collector,
                                   import_aliases);
            break;
        case AST_MATCH_ARM:
            rewrite_call_sites(node->as.match_arm.guard, registry, collector, import_aliases);
            rewrite_call_sites(node->as.match_arm.body, registry, collector, import_aliases);
            break;
        case AST_IS_EXPR:
            rewrite_call_sites(node->as.is_expr.expr, registry, collector, import_aliases);
            break;
        case AST_AS_EXPR:
            rewrite_call_sites(node->as.as_expr.expr, registry, collector, import_aliases);
            break;
        case AST_GO_EXPR:
            rewrite_call_sites(node->as.go_expr.expr, registry, collector, import_aliases);
            break;
        case AST_AWAIT_EXPR:
            rewrite_call_sites(node->as.await_expr.expr, registry, collector, import_aliases);
            rewrite_call_sites(node->as.await_expr.into, registry, collector, import_aliases);
            break;
        case AST_UNSAFE_EXPR:
            rewrite_call_sites(node->as.unsafe_expr.operand, registry, collector, import_aliases);
            break;
        case AST_COMPTIME_EXPR:
            rewrite_call_sites(node->as.comptime_expr.expr, registry, collector, import_aliases);
            break;
        case AST_SCOPE_BLOCK:
            rewrite_call_sites(node->as.scope_block.body, registry, collector, import_aliases);
            break;
        case AST_DEFER_STMT:
            rewrite_call_sites(node->as.defer_stmt.body, registry, collector, import_aliases);
            break;
        case AST_YIELD_STMT:
            rewrite_call_sites(node->as.yield_stmt.value, registry, collector, import_aliases);
            break;
        default:
            break;
    }
}

static void qualify_mono_hof_callback_params(AstNode *cloned, const AstNode *origin,
                                             XaMonoThrowEffect effect) {
    if (!cloned || !origin || cloned->type != AST_FUNCTION_DECL ||
        origin->type != AST_FUNCTION_DECL || effect != XA_MONO_EFFECT_NO_THROW)
        return;
    FunctionDeclNode *dst = &cloned->as.function_decl;
    const FunctionDeclNode *src = &origin->as.function_decl;
    int limit = dst->param_count < src->param_count ? dst->param_count : src->param_count;
    for (int i = 0; i < limit; i++) {
        XrParamNode *dst_param = dst->params ? dst->params[i] : NULL;
        const XrParamNode *src_param = src->params ? src->params[i] : NULL;
        if (!dst_param || !dst_param->type || !src_param || !src_param->type ||
            src_param->type->kind != XR_TREF_FUNCTION || src_param->type->requires_nothrow)
            continue;
        /* Type refs are otherwise immutable and may be shared with the origin.
         * Copy just this node before adding the inferred specialization bit. */
        XrTypeRef *qualified = (XrTypeRef *) xr_calloc(1, sizeof(XrTypeRef));
        if (!qualified)
            continue;
        *qualified = *dst_param->type;
        qualified->requires_nothrow = true;
        dst_param->type = qualified;
    }
}

// Inject monomorphized function declarations into the program AST
static void inject_mono_decls(AstNode *root, XaGenericRegistry *registry,
                              XaMonoCollector *collector, const XaMonoImportAliases *import_aliases,
                              XrVMRuntime *isolate) {
    if (!root || root->type != AST_PROGRAM || collector->count == 0)
        return;

    ProgramNode *prog = &root->as.program;
    XrAstCloneCtx clone_ctx = {.session = xr_compiler_session_current_for_isolate(isolate)};

    // prog->statements starts as arena-allocated (from xr_ast_program_add).
    // Once we copy it to the heap for growth, heap_owned becomes true and
    // subsequent grows can safely xr_free the old buffer.
    bool heap_owned = false;

    for (int i = 0; i < collector->count; i++) {
        XaMonoInstance *inst = &collector->instances[i];
        XaGenericDecl *decl = registry_find(registry, inst->generic_name);
        if (!decl || !decl->node)
            continue;
        if (!decl->inject_clone)
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
        AstNode *cloned = xr_ast_clone_ctx(decl->node, map, map_count, &clone_ctx);
        xr_free(map);

        if (!cloned)
            continue;

        // Rename cloned function/class to mangled name
        if (cloned->type == AST_FUNCTION_DECL) {
            xr_free(cloned->as.function_decl.name);
            cloned->as.function_decl.name = xr_strdup(inst->mangled_name);
            cloned->as.function_decl.type_param_count = 0;
            cloned->as.function_decl.type_params = NULL;
            qualify_mono_hof_callback_params(cloned, decl->node, inst->throw_effect);
        } else if (cloned->type == AST_CLASS_DECL) {
            xr_free(cloned->as.class_decl.name);
            cloned->as.class_decl.name = xr_strdup(inst->mangled_name);
            cloned->as.class_decl.type_param_count = 0;
            cloned->as.class_decl.type_params = NULL;
            cloned->as.class_decl.is_monomorphized = true;
            cloned->as.class_decl.generic_origin_name = xr_strdup(inst->generic_name);
            cloned->as.class_decl.display_name = xr_strdup(inst->generic_name);
            /* Store concrete type arg display names for cold typename/debug metadata. */
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

        // task-221 gap C (nested monomorphization fixpoint): a specialized clone
        // may itself construct other generics parameterized by the now-concrete
        // type args (e.g. Router<int>.wrap building RouteMatch<int>). Collect
        // those nested instantiations so this loop — which runs while
        // collector->count keeps growing — injects them too.
        //
        // Dedup alone does NOT make this terminate: polymorphic recursion
        // (`f<T>` instantiating `f<Box<T>>`) produces a distinct mangled name
        // every round, so nothing repeats and the loop diverges. `expanding`
        // attributes what this clone requests to instance i, giving those
        // instances depth i+1 and letting the depth budget cut the chain.
        if (import_aliases) {
            int saved_expanding = collector->expanding;
            collector->expanding = i;
            collect_instantiation_sites(cloned, registry, collector, import_aliases, false);
            collector->expanding = saved_expanding;
        }
    }
}

/* ========== Public API ========== */

static bool xa_mono_pass_internal(AstNode *root, AstNode **external_roots, int external_root_count,
                                  XrVMRuntime *isolate, XaAnalyzer *analyzer) {
    if (!root || root->type != AST_PROGRAM)
        return true;

    bool ok = true;
    XaGenericRegistry registry;
    registry_init(&registry);

    XaMonoCollector collector;
    xa_mono_collector_init(&collector);
    collector.analyzer = analyzer;

    XaMonoImportAliases root_imports;
    mono_import_aliases_init(&root_imports);
    collect_import_aliases(root, &root_imports);

    // Phase 1: collect local generic declarations and external templates/names.
    collect_generic_decls(root, &registry);
    for (int i = 0; external_roots && i < external_root_count; i++) {
        if (external_roots[i] && external_roots[i] != root)
            collect_external_generic_decls(external_roots[i], &registry);
    }
    if (registry.count == 0)
        goto cleanup;

    // Phase 2: collect this module's own instantiations. External generic
    // class/function declarations are rewrite-only in the using module; their
    // clones are injected by their defining module when it scans cross-module
    // roots below.
    collect_instantiation_sites(root, &registry, &collector, &root_imports, false);

    // Phase 2b: collect external modules' uses of declarations defined in
    // this root, so e.g. sys owns the ThreadLocal<int> clone while an importer
    // later rewrites sys.ThreadLocal<int> to sys.ThreadLocal$i64.
    for (int i = 0; external_roots && i < external_root_count; i++) {
        AstNode *external = external_roots[i];
        if (!external || external == root)
            continue;
        XaMonoImportAliases external_imports;
        mono_import_aliases_init(&external_imports);
        collect_import_aliases(external, &external_imports);
        collect_instantiation_sites(external, &registry, &collector, &external_imports, true);
        mono_import_aliases_free(&external_imports);
    }

    if (collector.count == 0)
        goto cleanup;

    // Phase 3: Clone + substitute + inject monomorphized versions. Passing the
    // import aliases lets injection collect nested generic instantiations from
    // each specialized clone (fixpoint), so generics constructed inside generic
    // methods/functions are specialized too.
    inject_mono_decls(root, &registry, &collector, &root_imports, isolate);

    // Phase 4: Rewrite call sites to use mangled names
    rewrite_call_sites(root, &registry, &collector, &root_imports);

    // Debug: print mono stats if XRAY_MONO_DEBUG is set
#if XR_DEBUG
    if (getenv("XRAY_MONO_DEBUG")) {
        xr_log_debug("mono", "%d generic decls, %d mono instances", registry.count,
                     collector.count);
    }
#endif

cleanup:
    mono_import_aliases_free(&root_imports);
    ok = !collector.budget_reported;
    xa_mono_collector_free(&collector);
    xr_free(registry.decls);
    return ok;
}

bool xa_mono_pass(AstNode *root, AstNode **external_roots, int external_root_count,
                  XrVMRuntime *isolate, XaAnalyzer *analyzer) {
    XR_DCHECK(analyzer != NULL, "xa_mono_pass: NULL analyzer; budgets need a diagnostic sink");
    return xa_mono_pass_internal(root, external_roots, external_root_count, isolate, analyzer);
}
