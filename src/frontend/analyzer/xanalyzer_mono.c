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
#include "xa_node_table.h"
#include "xa_selection.h"
#include "../../base/xarena.h"
#include "../../base/xlog.h"
#include "../../base/xchecks.h"
#include "../../runtime/value/xtype.h"
#include "../../runtime/xerror_codes.h"
#include "../../toolchain/xcompiler_session.h"
#include "../../module/xmodule_graph.h"
#include "../parser/xast_nodes.h"
#include "../parser/xtype_ref.h"
#include "xtype_ref_resolve.h"
#include "../../base/xmalloc.h"

#include <inttypes.h>
#include <limits.h>
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

static uint64_t mono_hash_u64(uint64_t hash, uint64_t value) {
    for (uint32_t shift = 0u; shift < 64u; shift += 8u) {
        hash ^= (uint8_t) (value >> shift);
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
    if (count > 0 && !type_args)
        return NULL;
    if (!name || count <= 0)
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
        return NULL;
    for (int i = 0; i < count; i++) {
        if (type_args[i] && mono_tag_needs_children(type_args[i]->kind)) {
            if (!qualified) {
                qualified = (char *) xr_calloc((size_t) count, XR_MONO_TYPE_TAG_CAP);
                if (!qualified) {
                    xr_free((void *) tags);
                    return NULL;
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
        return NULL;
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

char *xr_mono_mangle_in_analyzer(XaAnalyzer *analyzer, const char *name, XrTypeRef **type_args,
                                 int count) {
    char *base = xr_mono_mangle(name, type_args, count);
    if (!base || !analyzer || !type_args || count <= 0)
        return base;
    uint64_t tuple_key = UINT64_C(1469598103934665603);
    tuple_key = mono_hash_u64(tuple_key, (uint64_t) count);
    bool has_nominal = false;
    for (int i = 0; i < count; ++i) {
        uint64_t type_key = 0u;
        bool type_has_nominal = false;
        if (!xr_tref_exact_semantic_key(analyzer, type_args[i], &type_key, &type_has_nominal)) {
            xr_free(base);
            return NULL;
        }
        tuple_key = mono_hash_u64(tuple_key, type_key);
        has_nominal = has_nominal || type_has_nominal;
    }
    if (!has_nominal)
        return base;
    size_t length = strlen(base) + sizeof("$x0123456789abcdef");
    char *qualified = (char *) xr_malloc(length);
    if (!qualified) {
        xr_free(base);
        return NULL;
    }
    snprintf(qualified, length, "%s$x%016" PRIx64, base, tuple_key);
    xr_free(base);
    return qualified;
}

/* ========== Type Substitution ========== */

static XrTypeRef *mono_type_substitute(XaAnalyzer *analyzer, XrTypeRef *type, XrMonoTypeMap *map,
                                       int map_count) {
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
            return NULL;
        for (int i = 0; i < type->nchildren; i++) {
            new_children[i] = mono_type_substitute(analyzer, type->children[i], map, map_count);
            if (!new_children[i]) {
                xr_free(new_children);
                return NULL;
            }
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
            return NULL;
        }
        *result = *type;
        result->children = new_children;
        if (analyzer) {
            XrType *semantic = xa_analyzer_get_type_ref_type(analyzer, type);
            const char *stack_names[16];
            XrType *stack_types[16];
            const char **names = map_count <= 16
                                     ? stack_names
                                     : (const char **) xr_malloc(sizeof(*names) * map_count);
            XrType **types =
                map_count <= 16 ? stack_types : (XrType **) xr_malloc(sizeof(*types) * map_count);
            if (!semantic || !names || !types) {
                if (names != stack_names)
                    xr_free(names);
                if (types != stack_types)
                    xr_free(types);
                xr_free(new_children);
                xr_free(result);
                return NULL;
            }
            bool complete = true;
            for (int i = 0; i < map_count; ++i) {
                names[i] = map[i].param_name;
                types[i] = map[i].concrete_semantic_type
                               ? map[i].concrete_semantic_type
                               : xa_analyzer_get_type_ref_type(analyzer, map[i].concrete_type);
                if (!names[i] || !types[i])
                    complete = false;
            }
            XrType *substituted =
                complete ? xr_type_substitute(analyzer->isolate, semantic, names, types, map_count)
                         : NULL;
            if (names != stack_names)
                xr_free(names);
            if (types != stack_types)
                xr_free(types);
            if (!substituted || !xa_analyzer_bind_type_ref_type(analyzer, result, substituted)) {
                xr_free(new_children);
                xr_free(result);
                return NULL;
            }
        }
        return result;
    }

    return type;
}

XrTypeRef *xr_mono_type_substitute(XrTypeRef *type, XrMonoTypeMap *map, int map_count) {
    return mono_type_substitute(NULL, type, map, map_count);
}

XrTypeRef *xr_mono_type_substitute_in_analyzer(XaAnalyzer *analyzer, XrTypeRef *type,
                                               XrMonoTypeMap *map, int map_count) {
    return mono_type_substitute(analyzer, type, map, map_count);
}

/* ========== AST Clone ========== */

static char *clone_str(const char *s) {
    return s ? xr_strdup(s) : NULL;
}

typedef struct {
    XrCompilerSession *session;
    XaAnalyzer *analyzer;
    bool type_substitution_failed;
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
    if (!result) {
        if (clone_ctx)
            clone_ctx->type_substitution_failed = true;
        return NULL;
    }
    for (int i = 0; i < count; i++) {
        result[i] = xr_ast_clone_ctx(arr[i], map, map_count, clone_ctx);
        if (arr[i] && !result[i] && clone_ctx)
            clone_ctx->type_substitution_failed = true;
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

static AstBorrowOriginRef *clone_borrow_origins(const AstBorrowOriginRef *origins, int count) {
    if (!origins || count <= 0)
        return NULL;
    AstBorrowOriginRef *result = (AstBorrowOriginRef *) xr_calloc((size_t) count, sizeof(*result));
    if (!result)
        return NULL;
    for (int i = 0; i < count; i++) {
        result[i] = origins[i];
        result[i].name = clone_str(origins[i].name);
    }
    return result;
}

/* Substitute type parameters in an XrTypeRef tree.
 * Returns a new XrTypeRef if substitution occurred,
 * or the original pointer unchanged. */
static XrTypeRef *sub_tref(XrTypeRef *t, XrMonoTypeMap *map, int mc, XrAstCloneCtx *clone_ctx) {
    if (!t || !map || mc <= 0)
        return t;
    XrTypeRef *result = clone_ctx && clone_ctx->analyzer
                            ? xr_mono_type_substitute_in_analyzer(clone_ctx->analyzer, t, map, mc)
                            : xr_mono_type_substitute(t, map, mc);
    if (!result && clone_ctx)
        clone_ctx->type_substitution_failed = true;
    return result;
}

static XrTypeRef **clone_tref_array(XrTypeRef **arr, int count, XrMonoTypeMap *map, int mc,
                                    XrAstCloneCtx *clone_ctx) {
    if (!arr || count <= 0)
        return NULL;
    XrTypeRef **result = (XrTypeRef **) xr_calloc((size_t) count, sizeof(XrTypeRef *));
    if (!result) {
        if (clone_ctx)
            clone_ctx->type_substitution_failed = true;
        return NULL;
    }
    for (int i = 0; i < count; i++) {
        result[i] = sub_tref(arr[i], map, mc, clone_ctx);
        if (arr[i] && !result[i] && clone_ctx)
            clone_ctx->type_substitution_failed = true;
    }
    return result;
}

static XrParamNode **clone_params(XrParamNode **params, int count, XrMonoTypeMap *map, int mc,
                                  XrAstCloneCtx *clone_ctx) {
    if (!params || count <= 0)
        return NULL;
    XrParamNode **result = (XrParamNode **) xr_calloc(count, sizeof(XrParamNode *));
    if (!result) {
        if (clone_ctx)
            clone_ctx->type_substitution_failed = true;
        return NULL;
    }
    for (int i = 0; i < count; i++) {
        if (!params[i])
            continue;
        XrParamNode *p = (XrParamNode *) xr_calloc(1, sizeof(XrParamNode));
        if (!p) {
            if (clone_ctx)
                clone_ctx->type_substitution_failed = true;
            return result;
        }
        *p = *params[i];
        p->name = clone_str(params[i]->name);
        p->type = sub_tref(params[i]->type, map, mc, clone_ctx);
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

static XrNameSpan *clone_name_spans(const XrNameSpan *spans, int count) {
    if (!spans || count <= 0)
        return NULL;
    XrNameSpan *result = (XrNameSpan *) xr_malloc((size_t) count * sizeof(*result));
    if (result)
        memcpy(result, spans, (size_t) count * sizeof(*result));
    return result;
}

/* Clone method-local generic params. Constraints go through sub_tref so a
 * bound that mentions an enclosing class type param (for example
 * `find<U: Comparable<T>>` inside `Box<T>`) lands on the substituted type. */
static XrGenericParam **clone_generic_params(XrGenericParam **arr, int count, XrMonoTypeMap *map,
                                             int mc, XrAstCloneCtx *clone_ctx) {
    if (!arr || count <= 0)
        return NULL;
    XrGenericParam **result = (XrGenericParam **) xr_calloc((size_t) count, sizeof(*result));
    if (!result) {
        if (clone_ctx)
            clone_ctx->type_substitution_failed = true;
        return NULL;
    }
    for (int i = 0; i < count; i++) {
        if (!arr[i])
            continue;
        XrGenericParam *gp = (XrGenericParam *) xr_calloc(1, sizeof(XrGenericParam));
        if (!gp) {
            if (clone_ctx)
                clone_ctx->type_substitution_failed = true;
            return result;
        }
        gp->name = clone_str(arr[i]->name);
        gp->constraints =
            clone_tref_array(arr[i]->constraints, arr[i]->constraint_count, map, mc, clone_ctx);
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
    if (!n) {
        if (clone_ctx)
            clone_ctx->type_substitution_failed = true;
        return NULL;
    }
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
            n->as.var_decl.type_annotation =
                sub_tref(node->as.var_decl.type_annotation, map, mc, clone_ctx);
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
            n->as.for_in_stmt.item_type =
                sub_tref(node->as.for_in_stmt.item_type, map, mc, clone_ctx);
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
            dst->return_type = sub_tref(src->return_type, map, mc, clone_ctx);
            dst->borrow_origin_syntax = src->borrow_origin_syntax;
            dst->borrow_origin_count = src->borrow_origin_count;
            dst->borrow_origins =
                clone_borrow_origins(src->borrow_origins, src->borrow_origin_count);
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
            n->as.call_expr.type_args =
                clone_tref_array(node->as.call_expr.type_args, node->as.call_expr.type_arg_count,
                                 map, mc, clone_ctx);
            n->as.call_expr.type_arg_count = node->as.call_expr.type_arg_count;
            n->as.call_expr.semantic_type_id = node->as.call_expr.semantic_type_id;
            n->as.call_expr.semantic_type_args =
                clone_tref_array(node->as.call_expr.semantic_type_args,
                                 node->as.call_expr.semantic_type_arg_count, map, mc, clone_ctx);
            n->as.call_expr.semantic_type_arg_count = node->as.call_expr.semantic_type_arg_count;
            if (clone_ctx && clone_ctx->analyzer) {
                XaGenericSpecializationFact fact;
                if (xa_analyzer_get_generic_specialization(clone_ctx->analyzer, node, &fact)) {
                    XrTypeRef **receiver_type_args =
                        clone_tref_array(fact.receiver_type_args,
                                         (int) fact.receiver_type_arg_count, map, mc, clone_ctx);
                    if (!fact.generic_decl || fact.declaration_type_arg_count == 0u ||
                        fact.declaration_type_arg_count !=
                            (uint32_t) n->as.call_expr.type_arg_count ||
                        !n->as.call_expr.type_args ||
                        (fact.receiver_type_arg_count > 0u && !receiver_type_args)) {
                        clone_ctx->type_substitution_failed = true;
                    } else {
                        fact.receiver_type_args = receiver_type_args;
                        fact.declaration_type_args = n->as.call_expr.type_args;
                        if (!xa_analyzer_set_generic_specialization(clone_ctx->analyzer, n, &fact))
                            clone_ctx->type_substitution_failed = true;
                    }
                }
            }
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
            n->as.is_expr.type = sub_tref(node->as.is_expr.type, map, mc, clone_ctx);
            break;
        case AST_AS_EXPR:
            n->as.as_expr.expr = xr_ast_clone_ctx(node->as.as_expr.expr, map, mc, clone_ctx);
            n->as.as_expr.type = sub_tref(node->as.as_expr.type, map, mc, clone_ctx);
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
                    dc->type = sub_tref(sc->type, map, mc, clone_ctx);
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
            /* A mono clone stays in its declaration-owning source module. The
             * graph
             * analyzer's symbol IDs are session-global, so preserving
             * this identity
             * is both exact and required before rewriting. */
            n->as.new_expr.class_symbol_id = node->as.new_expr.class_symbol_id;
            n->as.new_expr.arg_count = node->as.new_expr.arg_count;
            n->as.new_expr.arguments = clone_node_array(
                node->as.new_expr.arguments, node->as.new_expr.arg_count, map, mc, clone_ctx);
            n->as.new_expr.arg_accesses = clone_call_arg_accesses(node->as.new_expr.arg_accesses,
                                                                  node->as.new_expr.arg_count);
            n->as.new_expr.type_args = clone_tref_array(
                node->as.new_expr.type_args, node->as.new_expr.type_arg_count, map, mc, clone_ctx);
            n->as.new_expr.type_arg_count = node->as.new_expr.type_arg_count;
            n->as.new_expr.is_type_namespace = node->as.new_expr.is_type_namespace;
            if (clone_ctx && clone_ctx->analyzer && n->as.new_expr.type_arg_count > 0) {
                XaGenericSpecializationFact fact;
                if (xa_analyzer_get_generic_specialization(clone_ctx->analyzer, node, &fact)) {
                    if (!fact.generic_decl || fact.owner_decl ||
                        fact.declaration_type_arg_count !=
                            (uint32_t) n->as.new_expr.type_arg_count ||
                        !n->as.new_expr.type_args) {
                        clone_ctx->type_substitution_failed = true;
                    } else {
                        fact.declaration_type_args = n->as.new_expr.type_args;
                        if (!xa_analyzer_set_generic_specialization(clone_ctx->analyzer, n, &fact))
                            clone_ctx->type_substitution_failed = true;
                    }
                }
            }
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
        case AST_PATTERN_ADT:
            n->as.pattern_adt.variant =
                xr_ast_clone_ctx(node->as.pattern_adt.variant, map, mc, clone_ctx);
            n->as.pattern_adt.count = node->as.pattern_adt.count;
            n->as.pattern_adt.field_names =
                clone_str_array(node->as.pattern_adt.field_names, node->as.pattern_adt.count);
            n->as.pattern_adt.field_name_spans =
                clone_name_spans(node->as.pattern_adt.field_name_spans, node->as.pattern_adt.count);
            n->as.pattern_adt.patterns = clone_node_array(
                node->as.pattern_adt.patterns, node->as.pattern_adt.count, map, mc, clone_ctx);
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
        case AST_ENUM_CONSTRUCT:
            n->as.enum_construct.variant_path =
                xr_ast_clone_ctx(node->as.enum_construct.variant_path, map, mc, clone_ctx);
            n->as.enum_construct.field_count = node->as.enum_construct.field_count;
            n->as.enum_construct.field_names = clone_str_array(node->as.enum_construct.field_names,
                                                               node->as.enum_construct.field_count);
            n->as.enum_construct.field_name_spans = clone_name_spans(
                node->as.enum_construct.field_name_spans, node->as.enum_construct.field_count);
            n->as.enum_construct.field_values =
                clone_node_array(node->as.enum_construct.field_values,
                                 node->as.enum_construct.field_count, map, mc, clone_ctx);
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
            dst->interfaces =
                clone_tref_array(src->interfaces, src->interface_count, map, mc, clone_ctx);
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
            dst->return_type = sub_tref(src->return_type, map, mc, clone_ctx);
            dst->borrow_origin_syntax = src->borrow_origin_syntax;
            dst->borrow_origin_count = src->borrow_origin_count;
            dst->borrow_origins =
                clone_borrow_origins(src->borrow_origins, src->borrow_origin_count);
            dst->body = xr_ast_clone_ctx(src->body, map, mc, clone_ctx);
            dst->is_constructor = src->is_constructor;
            dst->is_static = src->is_static;
            dst->is_private = src->is_private;
            dst->is_protected = src->is_protected;
            dst->is_getter = src->is_getter;
            dst->is_setter = src->is_setter;
            dst->is_static_constructor = src->is_static_constructor;
            dst->receiver_mode = src->receiver_mode;
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
                clone_generic_params(src->type_params, src->type_param_count, map, mc, clone_ctx);
            dst->type_param_count = src->type_param_count;
            break;
        }

        // === Field declaration (deep clone for mono) ===
        case AST_FIELD_DECL: {
            FieldDeclNode *src = &node->as.field_decl;
            FieldDeclNode *dst = &n->as.field_decl;
            dst->name = clone_str(src->name);
            dst->field_type = sub_tref(src->field_type, map, mc, clone_ctx);
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
            dst->type_path = xr_ast_clone_ctx(src->type_path, map, mc, clone_ctx);
            dst->struct_name = clone_str(src->struct_name);
            dst->field_count = src->field_count;
            dst->field_names = clone_str_array(src->field_names, src->field_count);
            dst->field_values =
                clone_node_array(src->field_values, src->field_count, map, mc, clone_ctx);
            dst->type_args =
                clone_tref_array(src->type_args, src->type_arg_count, map, mc, clone_ctx);
            dst->type_arg_count = src->type_arg_count;
            if (clone_ctx && clone_ctx->analyzer) {
                XaGenericSpecializationFact fact;
                if (xa_analyzer_get_generic_specialization(clone_ctx->analyzer, node, &fact)) {
                    if (!fact.generic_decl || fact.owner_decl ||
                        fact.declaration_type_arg_count != (uint32_t) dst->type_arg_count ||
                        !dst->type_args) {
                        clone_ctx->type_substitution_failed = true;
                    } else {
                        fact.declaration_type_args = dst->type_args;
                        if (!xa_analyzer_set_generic_specialization(clone_ctx->analyzer, n,
                                                                    &fact)) {
                            clone_ctx->type_substitution_failed = true;
                        } else if (src->type_path && dst->type_path &&
                                   src->type_path->type == dst->type_path->type) {
                            if (src->type_path->type == AST_VARIABLE) {
                                dst->type_path->as.variable.symbol_id =
                                    src->type_path->as.variable.symbol_id;
                            } else if (src->type_path->type == AST_MEMBER_ACCESS &&
                                       src->type_path->as.member_access.object &&
                                       dst->type_path->as.member_access.object &&
                                       src->type_path->as.member_access.object->type ==
                                           AST_VARIABLE &&
                                       dst->type_path->as.member_access.object->type ==
                                           AST_VARIABLE) {
                                dst->type_path->as.member_access.object->as.variable.symbol_id =
                                    src->type_path->as.member_access.object->as.variable.symbol_id;
                            }
                        }
                    }
                }
            }
            break;
        }

        case AST_ENUM_MEMBER:
            n->as.enum_member.name = clone_str(node->as.enum_member.name);
            n->as.enum_member.payload_count = node->as.enum_member.payload_count;
            n->as.enum_member.payload_names = clone_str_array(node->as.enum_member.payload_names,
                                                              node->as.enum_member.payload_count);
            n->as.enum_member.payload_name_spans = clone_name_spans(
                node->as.enum_member.payload_name_spans, node->as.enum_member.payload_count);
            n->as.enum_member.payload_types =
                clone_tref_array(node->as.enum_member.payload_types,
                                 node->as.enum_member.payload_count, map, mc, clone_ctx);
            break;

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
    c->rewrite_root = NULL;
    c->tref_rewrite_count = 0;
    c->expanding = -1;
    c->max_depth = XR_MONO_MAX_DEPTH;
    c->max_instances = XR_MONO_MAX_INSTANCES;
    c->instance_offset = 0u;
    c->max_observed_depth = 0u;
    c->record_only = false;
    c->budget_reported = false;
    c->rewrite_failed = false;
}

void xa_mono_collector_free(XaMonoCollector *c) {
    XR_DCHECK(c != NULL, "xa_mono_collector_free: NULL collector");
    for (int i = 0; i < c->count; i++) {
        xr_free((void *) c->instances[i].generic_name);
        xr_free(c->instances[i].identity.receiver_type_args);
        xr_free(c->instances[i].identity.declaration_type_args);
        xr_free((void *) c->instances[i].mangled_name);
    }
    xr_free(c->instances);
    c->instances = NULL;
    c->count = 0;
    c->capacity = 0;
    c->analyzer = NULL;
    c->rewrite_root = NULL;
    c->expanding = -1;
    c->record_only = false;
    c->budget_reported = false;
    c->rewrite_failed = false;
}

static char *mono_effect_mangle(XaMonoCollector *collector, const char *generic_name,
                                XrTypeRef **type_args, int type_arg_count,
                                XaGenericSpecializationEffect effect) {
    char *base = xr_mono_mangle_in_analyzer(collector ? collector->analyzer : NULL, generic_name,
                                            type_args, type_arg_count);
    if (!base || effect != XA_GENERIC_SPECIALIZATION_EFFECT_NO_THROW)
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
    for (int i = parent; i >= 0 && n <= (int) c->max_depth; i = c->instances[i].parent)
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

static void mono_report_exact_type_identity(XaMonoCollector *collector, const char *generic_name,
                                            const XrLocation *location) {
    if (!collector || collector->rewrite_failed)
        return;
    collector->rewrite_failed = true;
    char message[256];
    snprintf(message, sizeof(message),
             "generic specialization '%s' has an incomplete concrete type identity",
             generic_name ? generic_name : "<unknown>");
    XrLocation at = location ? *location : (XrLocation) {0};
    if (!at.file && collector->analyzer)
        at.file = collector->analyzer->current_file;
    xa_analyzer_add_diagnostic(collector->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_MISSING_TYPE,
                               message, &at);
}

static bool mono_type_ref_contains_open_param(XaAnalyzer *analyzer, const XrTypeRef *type) {
    if (!type)
        return false;
    if (type->kind == XR_TREF_TYPE_PARAM)
        return true;
    XrType *semantic = analyzer ? xa_analyzer_get_type_ref_type(analyzer, type) : NULL;
    if (semantic && semantic->kind == XR_KIND_TYPE_PARAM)
        return true;
    for (uint8_t i = 0; i < type->nchildren; ++i) {
        if (mono_type_ref_contains_open_param(analyzer, type->children ? type->children[i] : NULL))
            return true;
    }
    return false;
}

static char *mono_specialization_mangle(XaMonoCollector *collector, const char *generic_name,
                                        const XaGenericSpecializationFact *identity) {
    if (!collector || !generic_name || !identity)
        return NULL;
    char *declaration_name =
        mono_effect_mangle(collector, generic_name, identity->declaration_type_args,
                           (int) identity->declaration_type_arg_count, identity->effect);
    if (!declaration_name || identity->receiver_type_arg_count == 0u)
        return declaration_name;
    char *receiver_name =
        xr_mono_mangle_in_analyzer(collector->analyzer, "receiver", identity->receiver_type_args,
                                   (int) identity->receiver_type_arg_count);
    if (!receiver_name) {
        xr_free(declaration_name);
        return NULL;
    }
    size_t length = strlen(declaration_name) + strlen(receiver_name) + sizeof("$on$");
    char *result = (char *) xr_malloc(length);
    if (result)
        snprintf(result, length, "%s$on$%s", declaration_name, receiver_name);
    xr_free(receiver_name);
    xr_free(declaration_name);
    return result;
}

static bool mono_specialization_contains_open_type(XaMonoCollector *collector,
                                                   const XaGenericSpecializationFact *identity) {
    if (!identity)
        return true;
    for (uint32_t i = 0u; i < identity->receiver_type_arg_count; ++i) {
        if (mono_type_ref_contains_open_param(collector ? collector->analyzer : NULL,
                                              identity->receiver_type_args[i]))
            return true;
    }
    for (uint32_t i = 0u; i < identity->declaration_type_arg_count; ++i) {
        if (mono_type_ref_contains_open_param(collector ? collector->analyzer : NULL,
                                              identity->declaration_type_args[i]))
            return true;
    }
    return false;
}

static XrTypeRef **mono_copy_type_ref_tuple(XrTypeRef **source, uint32_t count) {
    if (count == 0u)
        return NULL;
    XrTypeRef **copy = (XrTypeRef **) xr_malloc(sizeof(*copy) * (size_t) count);
    if (copy)
        memcpy(copy, source, sizeof(*copy) * (size_t) count);
    return copy;
}

static const char *mono_generic_owner_name(const AstNode *owner_decl) {
    if (!owner_decl)
        return NULL;
    if (owner_decl->type == AST_CLASS_DECL)
        return owner_decl->as.class_decl.name;
    if (owner_decl->type == AST_STRUCT_DECL)
        return owner_decl->as.struct_decl.name;
    return NULL;
}

static bool mono_specialization_supported(const XaGenericSpecializationFact *identity) {
    if (!identity || !identity->generic_decl || identity->generic_decl->type != AST_METHOD_DECL)
        return true;
    if (!identity->owner_decl || (identity->owner_decl->type != AST_CLASS_DECL &&
                                  identity->owner_decl->type != AST_STRUCT_DECL))
        return false;
    const ClassDeclNode *owner = identity->owner_decl->type == AST_CLASS_DECL
                                     ? &identity->owner_decl->as.class_decl
                                     : &identity->owner_decl->as.struct_decl;
    if (owner->type_param_count == 0)
        return identity->receiver_type_arg_count == 0u && !owner->is_generic_skeleton &&
               !owner->is_monomorphized;
    /* The first executable generic-receiver slice is a value struct. Generic
     * classes need
     * separate heap identity and owned-field lifetime work; do
     * not admit them through the
     * value path. */
    return identity->owner_decl->type == AST_STRUCT_DECL &&
           identity->receiver_type_arg_count == (uint32_t) owner->type_param_count &&
           !owner->is_monomorphized;
}

static const char *xa_mono_collector_add_exact(XaMonoCollector *c, const char *generic_name,
                                               const XaGenericSpecializationFact *identity,
                                               const XrLocation *loc) {
    if (!c || !generic_name || !xa_generic_specialization_fact_valid(identity) ||
        !mono_specialization_supported(identity)) {
        mono_report_exact_type_identity(c, generic_name, loc);
        return NULL;
    }

    /* An instantiation nested in a generic template is not a concrete root yet.
     * Defer it
     * until the enclosing clone substitutes every open parameter; the
     * injection fixpoint
     * will then collect the concrete nested root. */
    if (mono_specialization_contains_open_type(c, identity))
        return NULL;

    /* A method on a generic receiver is executable only on the exact concrete
     * aggregate.
     * Register that aggregate before the method so injection can
     * attach the specialized body
     * through an AST identity, even when the
     * receiver reaches the call through a parameter
     * rather than a literal. */
    if (identity->generic_decl->type == AST_METHOD_DECL && identity->receiver_type_arg_count > 0u) {
        const char *owner_name = mono_generic_owner_name(identity->owner_decl);
        XaGenericSpecializationFact owner_identity = {
            .generic_decl = identity->owner_decl,
            .declaration_type_args = identity->receiver_type_args,
            .declaration_type_arg_count = identity->receiver_type_arg_count,
            .effect = XA_GENERIC_SPECIALIZATION_EFFECT_NONE,
        };
        if (!owner_name || !xa_mono_collector_add_exact(c, owner_name, &owner_identity, loc)) {
            mono_report_exact_type_identity(c, generic_name, loc);
            return NULL;
        }
    }

    char *candidate_mangled = mono_specialization_mangle(c, generic_name, identity);
    if (!candidate_mangled) {
        mono_report_exact_type_identity(c, generic_name, loc);
        return NULL;
    }

    // Concrete type arguments define instance identity. ABI-equivalent instances
    // may share code only through explicit verified plans, not collector dedup.
    for (int i = 0; i < c->count; i++) {
        if (c->instances[i].identity.generic_decl == identity->generic_decl &&
            c->instances[i].identity.owner_decl == identity->owner_decl &&
            c->instances[i].identity.effect == identity->effect &&
            strcmp(c->instances[i].generic_name, generic_name) == 0 &&
            strcmp(c->instances[i].mangled_name, candidate_mangled) == 0) {
            xr_free(candidate_mangled);
            return c->instances[i].mangled_name;  // Already registered
        }
    }

    int parent = c->expanding;
    int depth = parent >= 0 ? c->instances[parent].depth + 1 : 0;

    /* Depth guard: a specialized body instantiating an ever-larger type has no
     * finite expansion. Dedup cannot catch it -- every round is a new tuple. */
    if (depth > (int) c->max_depth) {
        char chain[512];
        mono_render_chain(c, parent, chain, sizeof(chain));
        char msg[896];
        snprintf(msg, sizeof(msg),
                 "generic instantiation of '%s' nested deeper than %d levels\n"
                 "  instantiated through: %s -> %s\n"
                 "  note: a generic that instantiates itself at a larger type (f<T> requesting "
                 "f<Box<T>>) has no finite specialization and always reaches this limit",
                 generic_name, (int) c->max_depth, chain, candidate_mangled);
        mono_report(c, XR_ERR_ANALYZE_MONO_DEPTH, msg, loc);
        xr_free(candidate_mangled);
        return NULL;
    }
    if ((uint32_t) depth > c->max_observed_depth)
        c->max_observed_depth = (uint32_t) depth;

    /* Breadth guard: a compile-time memory backstop, not a language rule. */
    if (c->instance_offset + (uint32_t) c->count >= c->max_instances) {
        char msg[320];
        snprintf(msg, sizeof(msg),
                 "program exceeds the monomorphization budget of %d generic instances "
                 "(reached while instantiating '%s')",
                 (int) c->max_instances, generic_name);
        mono_report(c, XR_ERR_ANALYZE_MONO_BUDGET, msg, loc);
        xr_free(candidate_mangled);
        return NULL;
    }

    char *generic_name_copy = xr_strdup(generic_name);
    XrTypeRef **receiver_type_args =
        mono_copy_type_ref_tuple(identity->receiver_type_args, identity->receiver_type_arg_count);
    XrTypeRef **declaration_type_args = mono_copy_type_ref_tuple(
        identity->declaration_type_args, identity->declaration_type_arg_count);
    if (!generic_name_copy || (identity->receiver_type_arg_count > 0u && !receiver_type_args) ||
        (identity->declaration_type_arg_count > 0u && !declaration_type_args)) {
        xr_free(generic_name_copy);
        xr_free(receiver_type_args);
        xr_free(declaration_type_args);
        xr_free(candidate_mangled);
        mono_report_exact_type_identity(c, generic_name, loc);
        return NULL;
    }

    if (c->count >= c->capacity) {
        int new_capacity = c->capacity ? c->capacity * 2 : 8;
        XaMonoInstance *instances = (XaMonoInstance *) xr_realloc(
            c->instances, (size_t) new_capacity * sizeof(XaMonoInstance));
        if (!instances) {
            xr_free(generic_name_copy);
            xr_free(receiver_type_args);
            xr_free(declaration_type_args);
            xr_free(candidate_mangled);
            mono_report_exact_type_identity(c, generic_name, loc);
            return NULL;
        }
        c->instances = instances;
        c->capacity = new_capacity;
    }

    XaMonoInstance *inst = &c->instances[c->count];
    *inst = (XaMonoInstance) {
        .generic_name = generic_name_copy,
        .identity = *identity,
        .mangled_name = candidate_mangled,
        .parent = parent,
        .depth = depth,
    };
    inst->identity.receiver_type_args = receiver_type_args;
    inst->identity.declaration_type_args = declaration_type_args;
    c->count++;
    return inst->mangled_name;
}

const char *xa_mono_collector_add(XaMonoCollector *c, const char *generic_name,
                                  const XaGenericSpecializationFact *identity,
                                  const XrLocation *loc) {
    return xa_mono_collector_add_exact(c, generic_name, identity, loc);
}

// Lookup the exact concrete instance.
static const char *xa_mono_collector_lookup(XaMonoCollector *c, const char *generic_name,
                                            const XaGenericSpecializationFact *identity) {
    if (!c || !generic_name || !xa_generic_specialization_fact_valid(identity))
        return NULL;
    if (mono_specialization_contains_open_type(c, identity))
        return NULL;
    char *candidate_mangled = mono_specialization_mangle(c, generic_name, identity);
    if (!candidate_mangled) {
        mono_report_exact_type_identity(c, generic_name, NULL);
        return NULL;
    }
    const char *result = NULL;
    for (int i = 0; i < c->count; i++) {
        if (c->instances[i].identity.generic_decl != identity->generic_decl ||
            c->instances[i].identity.owner_decl != identity->owner_decl ||
            c->instances[i].identity.effect != identity->effect ||
            strcmp(c->instances[i].generic_name, generic_name) != 0)
            continue;
        if (candidate_mangled && strcmp(c->instances[i].mangled_name, candidate_mangled) == 0) {
            result = c->instances[i].mangled_name;
            break;
        }
    }
    xr_free(candidate_mangled);
    return result;
}

static const AstNode *mono_exact_generic_type_decl(XaMonoCollector *collector,
                                                   const XrTypeRef *type_ref) {
    if (!collector || !collector->analyzer || !type_ref)
        return NULL;
    XrType *semantic = xa_analyzer_get_type_ref_type(collector->analyzer, type_ref);
    XrClassInfo *nominal =
        semantic && XR_TYPE_IS_INSTANCE(semantic) ? semantic->instance.class_ref : NULL;
    XaSymbol *symbol = nominal ? nominal->declaration_symbol : NULL;
    XaSymbolLinks *links = symbol ? xa_analyzer_get_links(collector->analyzer, symbol) : NULL;
    const AstNode *decl = links ? links->nominal_decl_node : NULL;
    if (!decl || (decl->type != AST_CLASS_DECL && decl->type != AST_STRUCT_DECL))
        return NULL;
    return decl;
}

static XaMonoInstance *xa_mono_collector_find_type_instance(XaMonoCollector *collector,
                                                            const AstNode *generic_decl,
                                                            XrTypeRef **type_args,
                                                            int type_arg_count) {
    if (!collector || !generic_decl || !type_args || type_arg_count <= 0)
        return NULL;
    XaGenericSpecializationFact identity = {
        .generic_decl = generic_decl,
        .declaration_type_args = type_args,
        .declaration_type_arg_count = (uint32_t) type_arg_count,
        .effect = XA_GENERIC_SPECIALIZATION_EFFECT_NONE,
    };
    if (!xa_generic_specialization_fact_valid(&identity) ||
        mono_specialization_contains_open_type(collector, &identity))
        return NULL;
    for (int i = 0; i < collector->count; ++i) {
        const XaMonoInstance *instance = &collector->instances[i];
        if (instance->identity.generic_decl != generic_decl ||
            instance->identity.owner_decl != NULL ||
            instance->identity.effect != XA_GENERIC_SPECIALIZATION_EFFECT_NONE)
            continue;
        char *candidate = mono_specialization_mangle(collector, instance->generic_name, &identity);
        bool matches = candidate && strcmp(instance->mangled_name, candidate) == 0;
        xr_free(candidate);
        if (matches)
            return &collector->instances[i];
    }
    return NULL;
}

static const char *xa_mono_collector_lookup_type(XaMonoCollector *collector,
                                                 const AstNode *generic_decl, XrTypeRef **type_args,
                                                 int type_arg_count) {
    XaMonoInstance *instance =
        xa_mono_collector_find_type_instance(collector, generic_decl, type_args, type_arg_count);
    return instance ? instance->mangled_name : NULL;
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
        const AstNode *generic_decl = mono_exact_generic_type_decl(collector, tref);
        const char *mangled =
            xa_mono_collector_lookup_type(collector, generic_decl, tref->children, tref->nchildren);
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

// Generic declaration registry: exact declaration identities with display names
typedef struct {
    const char *name;
    AstNode *node;   // Exact generic function, method, class, or struct declaration
    AstNode *owner;  // Declaring class/struct for AST_METHOD_DECL; NULL otherwise
    AstNode *defining_root;
    int defining_spec_index;
    XrGenericParam **type_params;
    int type_param_count;
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

static bool registry_add(XaGenericRegistry *r, const char *name, AstNode *node, AstNode *owner,
                         AstNode *defining_root, int defining_spec_index, XrGenericParam **tp,
                         int tp_count, bool rewrite_member_access) {
    if (!r || !name || !node || !defining_root)
        return false;
    if (r->count >= r->capacity) {
        int new_cap = r->capacity ? r->capacity * 2 : 8;
        XaGenericDecl *_new_r_decls =
            (XaGenericDecl *) xr_realloc(r->decls, new_cap * sizeof(XaGenericDecl));
        if (!_new_r_decls)
            return false;
        r->decls = _new_r_decls;
        r->capacity = new_cap;
    }
    XaGenericDecl *d = &r->decls[r->count++];
    d->name = name;
    d->node = node;
    d->owner = owner;
    d->defining_root = defining_root;
    d->defining_spec_index = defining_spec_index;
    d->type_params = tp;
    d->type_param_count = tp_count;
    d->rewrite_member_access = rewrite_member_access;
    return true;
}

static bool registry_add_method(XaGenericRegistry *r, AstNode *root, int spec_index, AstNode *owner,
                                AstNode *method) {
    if (!r || !owner || !method || method->type != AST_METHOD_DECL ||
        method->as.method_decl.type_param_count <= 0)
        return false;
    return registry_add(r, method->as.method_decl.name, method, owner, root, spec_index,
                        method->as.method_decl.type_params, method->as.method_decl.type_param_count,
                        false);
}

static XaGenericDecl *registry_find_decl(XaGenericRegistry *r, const AstNode *node) {
    if (!r || !node)
        return NULL;
    for (int i = 0; i < r->count; i++) {
        if (r->decls[i].node == node)
            return &r->decls[i];
    }
    return NULL;
}

/* Resolve a generic aggregate construction through the analyzer's nominal
 * declaration identity.
 * Source spelling is only an import alias (and may also
 * collide with a local declaration), so it
 * is never a sound registry key. */
static XaGenericDecl *registry_find_nominal_site(XaGenericRegistry *registry, const AstNode *site,
                                                 XaAnalyzer *analyzer) {
    if (!registry || !site || !analyzer)
        return NULL;
    XaGenericSpecializationFact fact = {0};
    if (!xa_analyzer_get_generic_specialization(analyzer, site, &fact) ||
        !xa_generic_specialization_fact_valid(&fact) || fact.owner_decl)
        return NULL;
    return registry_find_decl(registry, fact.generic_decl);
}

static XaGenericDecl *registry_find_call(XaGenericRegistry *registry, const AstNode *call_node,
                                         XaAnalyzer *analyzer) {
    if (!registry || !call_node || call_node->type != AST_CALL_EXPR || !analyzer)
        return NULL;
    const CallExprNode *call = &call_node->as.call_expr;
    if (!call->callee)
        return NULL;

    XaSymbol *symbol = NULL;
    if (call->callee->type == AST_VARIABLE && call->callee->as.variable.symbol_id != 0) {
        symbol = xa_analyzer_symbol_by_id(analyzer, call->callee->as.variable.symbol_id);
    } else if (call->callee->type == AST_MEMBER_ACCESS) {
        const XaSelection *selection = xa_analyzer_get_selection(analyzer, call->callee);
        symbol = selection ? selection->target_symbol : NULL;
    }

    AstNode *declaration = symbol ? symbol->links.function_decl_node : NULL;
    if (!declaration && symbol)
        declaration = symbol->links.nominal_decl_node;
    XaGenericDecl *exact = registry_find_decl(registry, declaration);
    if (exact)
        return exact;
    if (declaration)
        return NULL;

    XaGenericSpecializationFact fact;
    if (!xa_analyzer_get_generic_specialization(analyzer, call_node, &fact))
        return NULL;
    XaGenericDecl *fact_decl = registry_find_decl(registry, fact.generic_decl);
    if (!fact_decl || !xa_generic_specialization_fact_valid(&fact) ||
        fact.owner_decl != (fact_decl->node->type == AST_METHOD_DECL ? fact_decl->owner : NULL))
        return NULL;
    return fact_decl;
}

static bool collect_owner_generic_methods(AstNode *root, int spec_index, AstNode *owner,
                                          XaGenericRegistry *registry) {
    if (!owner || !registry || (owner->type != AST_CLASS_DECL && owner->type != AST_STRUCT_DECL))
        return true;
    ClassDeclNode *decl =
        owner->type == AST_CLASS_DECL ? &owner->as.class_decl : &owner->as.struct_decl;
    for (int i = 0; decl->methods && i < decl->method_count; ++i) {
        AstNode *method = decl->methods[i];
        if (!method || method->type != AST_METHOD_DECL ||
            method->as.method_decl.type_param_count <= 0 || registry_find_decl(registry, method))
            continue;
        if (!registry_add_method(registry, root, spec_index, owner, method))
            return false;
    }
    return true;
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

typedef struct {
    AstNode *root;
    int spec_index;
    XaMonoImportAliases imports;
} XaMonoRootState;

static XaMonoRootState *mono_root_state_find(XaMonoRootState *states, int count,
                                             const AstNode *root) {
    if (!states || !root)
        return NULL;
    for (int i = 0; i < count; ++i) {
        if (states[i].root == root)
            return &states[i];
    }
    return NULL;
}

static int mono_root_spec_index(XaAnalyzer *analyzer, const AstNode *root) {
    XrModuleGraph *graph = analyzer ? (XrModuleGraph *) analyzer->graph : NULL;
    if (!graph || !root)
        return -1;
    for (int i = 0; i < graph->spec_count; ++i) {
        if (graph->specs[i].ast == root)
            return i;
    }
    return -1;
}

static const char *mono_root_source_path(XaAnalyzer *analyzer, int spec_index) {
    XrModuleGraph *graph = analyzer ? (XrModuleGraph *) analyzer->graph : NULL;
    return graph && spec_index >= 0 && spec_index < graph->spec_count
               ? graph->specs[spec_index].source_path
               : NULL;
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
static XaGenericSpecializationEffect
mono_call_specialization_effect(const XaGenericDecl *decl, const CallExprNode *call,
                                const XaMonoCollector *collector) {
    if (!decl || !decl->node || !call)
        return XA_GENERIC_SPECIALIZATION_EFFECT_NONE;
    XrParamNode **params = NULL;
    int param_count = 0;
    if (decl->node->type == AST_FUNCTION_DECL) {
        params = decl->node->as.function_decl.params;
        param_count = decl->node->as.function_decl.param_count;
    } else if (decl->node->type == AST_METHOD_DECL) {
        params = decl->node->as.method_decl.params;
        param_count = decl->node->as.method_decl.param_count;
    } else {
        return XA_GENERIC_SPECIALIZATION_EFFECT_NONE;
    }
    bool has_poly_callback = false;
    bool all_no_throw = true;
    int limit = param_count < call->arg_count ? param_count : call->arg_count;
    for (int i = 0; i < param_count; i++) {
        const XrParamNode *param = params ? params[i] : NULL;
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
        return XA_GENERIC_SPECIALIZATION_EFFECT_NONE;
    return all_no_throw ? XA_GENERIC_SPECIALIZATION_EFFECT_NO_THROW
                        : XA_GENERIC_SPECIALIZATION_EFFECT_MAY_THROW;
}

static XaGenericSpecializationFact
mono_specialization_identity(const XaGenericDecl *decl, XrTypeRef **type_args, int type_arg_count,
                             XaGenericSpecializationEffect effect) {
    if (!decl || type_arg_count < 0)
        return (XaGenericSpecializationFact) {0};
    return (XaGenericSpecializationFact) {
        .generic_decl = decl->node,
        .owner_decl = decl->node && decl->node->type == AST_METHOD_DECL ? decl->owner : NULL,
        .declaration_type_args = type_args,
        .declaration_type_arg_count = (uint32_t) type_arg_count,
        .effect = effect,
    };
}

// Phase 1: collect every generic declaration once, together with its absolute
// declaration-owning module root.  Ownership never changes with the module
// currently being scanned or rewritten.
static bool collect_generic_decls(AstNode *root, int spec_index, XaGenericRegistry *registry) {
    if (!root)
        return true;

    if (root->type == AST_PROGRAM) {
        ProgramNode *prog = &root->as.program;
        for (int i = 0; i < prog->count; i++) {
            AstNode *stmt = prog->statements[i];
            if (!stmt)
                continue;

            if (stmt->type == AST_FUNCTION_DECL && stmt->as.function_decl.type_param_count > 0) {
                if (!registry_add(registry, stmt->as.function_decl.name, stmt, NULL, root,
                                  spec_index, stmt->as.function_decl.type_params,
                                  stmt->as.function_decl.type_param_count, true))
                    return false;
            }
            // Generic class: class Box<T> { ... }
            if (stmt->type == AST_CLASS_DECL && stmt->as.class_decl.type_param_count > 0) {
                if (!registry_add(registry, stmt->as.class_decl.name, stmt, NULL, root, spec_index,
                                  stmt->as.class_decl.type_params,
                                  stmt->as.class_decl.type_param_count, true))
                    return false;
            }
            // Generic struct: struct Pair<T, U> { ... }
            if (stmt->type == AST_STRUCT_DECL && stmt->as.struct_decl.type_param_count > 0) {
                if (!registry_add(registry, stmt->as.struct_decl.name, stmt, NULL, root, spec_index,
                                  stmt->as.struct_decl.type_params,
                                  stmt->as.struct_decl.type_param_count, false))
                    return false;
            }
            if (!collect_owner_generic_methods(root, spec_index, stmt, registry))
                return false;
        }
    }
    return true;
}

/* Source location of the instantiation site, so a budget diagnostic points at
 * the code the user wrote rather than at the generic's declaration. */
static XrLocation mono_node_loc(const XaAnalyzer *analyzer, const AstNode *node) {
    XrLocation loc = {0};
    if (!node)
        return loc;
    loc.file = analyzer ? analyzer->current_file : NULL;
    loc.line = (uint32_t) (node->line > 0 ? node->line : 0);
    loc.column = (uint32_t) (node->column > 0 ? node->column : 0);
    loc.end_line = (uint32_t) (node->end_line > 0 ? node->end_line : 0);
    loc.end_column = (uint32_t) (node->end_column > 0 ? node->end_column : 0);
    return loc;
}

static bool mono_publish_specialization(XaMonoCollector *collector, const AstNode *node,
                                        const XaGenericSpecializationFact *identity);

static bool mono_call_specialization_identity(XaMonoCollector *collector, const AstNode *node,
                                              const XaGenericDecl *decl, const CallExprNode *call,
                                              XaGenericSpecializationEffect effect,
                                              XaGenericSpecializationFact *out_identity) {
    if (!collector || !collector->analyzer || !node || !decl || !call || !out_identity)
        return false;
    XaGenericSpecializationFact published = {0};
    if (xa_analyzer_get_generic_specialization(collector->analyzer, node, &published)) {
        if (!xa_generic_specialization_fact_valid(&published) ||
            published.generic_decl != decl->node ||
            published.owner_decl !=
                (decl->node && decl->node->type == AST_METHOD_DECL ? decl->owner : NULL)) {
            mono_report_exact_type_identity(collector, decl->name, NULL);
            return false;
        }
        /* Call inference publishes a conservative effect before the whole-body
         * effect
         * fixed point has closed.  Monomorphization runs after that fixed
         * point and owns
         * the exact executable specialization dimension.  Keep
         * every declaration/type
         * coordinate unchanged and finalize only this
         * dimension on the same call node.
         */
        if (published.effect != effect) {
            published.effect = effect;
            if (!mono_publish_specialization(collector, node, &published))
                return false;
            /* Publishing replaces the node-table-owned pointer arrays.  Read
             * the
             * replacement fact before returning it to the collector. */
            published = (XaGenericSpecializationFact) {0};
            if (!xa_analyzer_get_generic_specialization(collector->analyzer, node, &published) ||
                !xa_generic_specialization_fact_valid(&published) ||
                published.generic_decl != decl->node ||
                published.owner_decl !=
                    (decl->node && decl->node->type == AST_METHOD_DECL ? decl->owner : NULL) ||
                published.effect != effect) {
                mono_report_exact_type_identity(collector, decl->name, NULL);
                return false;
            }
        }
        *out_identity = published;
        return true;
    }

    if (decl->node && decl->node->type == AST_METHOD_DECL && decl->owner) {
        const ClassDeclNode *owner = decl->owner->type == AST_CLASS_DECL
                                         ? &decl->owner->as.class_decl
                                         : &decl->owner->as.struct_decl;
        if (owner->type_param_count > 0) {
            mono_report_exact_type_identity(collector, decl->name, NULL);
            return false;
        }
    }
    XaGenericSpecializationFact identity =
        mono_specialization_identity(decl, call->type_args, call->type_arg_count, effect);
    if (!mono_publish_specialization(collector, node, &identity))
        return false;
    *out_identity = identity;
    return true;
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
        XrLocation loc = mono_node_loc(collector ? collector->analyzer : NULL, node);
        if (call->type_arg_count > 0 && call->callee && call->callee->type == AST_VARIABLE) {
            XaGenericDecl *decl =
                registry_find_call(registry, node, collector ? collector->analyzer : NULL);
            if (decl && decl->type_param_count == call->type_arg_count) {
                XaGenericSpecializationFact identity = {0};
                XaGenericSpecializationEffect effect =
                    mono_call_specialization_effect(decl, call, collector);
                if (!mono_call_specialization_identity(collector, node, decl, call, effect,
                                                       &identity))
                    return;
                if (!collector->record_only)
                    xa_mono_collector_add_exact(collector, decl->name, &identity, &loc);
            }
        }
        const char *member_name = NULL;
        if (mono_call_is_import_member_generic(call, import_aliases, &member_name)) {
            XaGenericDecl *decl =
                registry_find_call(registry, node, collector ? collector->analyzer : NULL);
            if (decl && decl->type_param_count == call->type_arg_count) {
                XaGenericSpecializationFact identity = {0};
                XaGenericSpecializationEffect effect =
                    mono_call_specialization_effect(decl, call, collector);
                if (!mono_call_specialization_identity(collector, node, decl, call, effect,
                                                       &identity))
                    return;
                if (!collector->record_only)
                    xa_mono_collector_add_exact(collector, decl->name, &identity, &loc);
            }
        }
        if (call->type_arg_count > 0 && call->callee && call->callee->type == AST_MEMBER_ACCESS) {
            XaGenericDecl *decl =
                registry_find_call(registry, node, collector ? collector->analyzer : NULL);
            if (decl && decl->node && decl->node->type == AST_METHOD_DECL &&
                decl->type_param_count == call->type_arg_count) {
                XaGenericSpecializationFact identity = {0};
                XaGenericSpecializationEffect effect =
                    mono_call_specialization_effect(decl, call, collector);
                if (!mono_call_specialization_identity(collector, node, decl, call, effect,
                                                       &identity))
                    return;
                if (!collector->record_only)
                    xa_mono_collector_add_exact(collector, decl->name, &identity, &loc);
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
            XaGenericDecl *decl =
                registry_find_nominal_site(registry, node, collector ? collector->analyzer : NULL);
            if (decl && decl->type_param_count == ne->type_arg_count) {
                XrLocation loc = mono_node_loc(collector ? collector->analyzer : NULL, node);
                XaGenericSpecializationFact identity = {0};
                if (!xa_analyzer_get_generic_specialization(collector->analyzer, node, &identity) ||
                    !xa_generic_specialization_fact_valid(&identity) ||
                    identity.generic_decl != decl->node || identity.owner_decl ||
                    identity.declaration_type_arg_count != (uint32_t) ne->type_arg_count) {
                    mono_report_exact_type_identity(collector, decl->name, NULL);
                    return;
                }
                xa_mono_collector_add_exact(collector, decl->name, &identity, &loc);
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
            XaGenericDecl *decl =
                registry_find_nominal_site(registry, node, collector ? collector->analyzer : NULL);
            if (decl && decl->type_param_count == sl->type_arg_count) {
                XrLocation loc = mono_node_loc(collector ? collector->analyzer : NULL, node);
                XaGenericSpecializationFact identity = {0};
                if (!xa_analyzer_get_generic_specialization(collector->analyzer, node, &identity) ||
                    !xa_generic_specialization_fact_valid(&identity) ||
                    identity.generic_decl != decl->node || identity.owner_decl ||
                    identity.declaration_type_arg_count != (uint32_t) sl->type_arg_count) {
                    mono_report_exact_type_identity(collector, decl->name, NULL);
                    return;
                }
                xa_mono_collector_add_exact(collector, decl->name, &identity, &loc);
            }
        }
        for (int i = 0; i < sl->field_count; i++)
            collect_instantiation_sites(sl->field_values[i], registry, collector, import_aliases,
                                        local_only);
        return;
    }

    if (node->type == AST_ENUM_CONSTRUCT) {
        EnumConstructNode *construct = &node->as.enum_construct;
        for (int i = 0; i < construct->field_count; i++)
            collect_instantiation_sites(construct->field_values[i], registry, collector,
                                        import_aliases, local_only);
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
        case AST_CLASS_DECL:
        case AST_STRUCT_DECL: {
            ClassDeclNode *decl =
                node->type == AST_CLASS_DECL ? &node->as.class_decl : &node->as.struct_decl;
            bool saved_record_only = collector->record_only;
            if (decl->type_param_count > 0)
                collector->record_only = true;
            for (int i = 0; i < decl->method_count; i++)
                collect_instantiation_sites(decl->methods[i], registry, collector, import_aliases,
                                            local_only);
            for (int i = 0; i < decl->field_count; i++)
                collect_instantiation_sites(decl->fields[i], registry, collector, import_aliases,
                                            local_only);
            collector->record_only = saved_record_only;
            break;
        }
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

static bool mono_find_private_target(XaAnalyzer *analyzer,
                                     const XaGenericSpecializationFact *identity,
                                     const char *export_name, int *spec_index_out,
                                     AstNode **target_decl_out) {
    if (spec_index_out)
        *spec_index_out = -1;
    if (target_decl_out)
        *target_decl_out = NULL;
    XrModuleGraph *graph = analyzer ? (XrModuleGraph *) analyzer->graph : NULL;
    if (!graph || !xa_generic_specialization_fact_valid(identity) || !export_name ||
        !spec_index_out || !target_decl_out)
        return false;
    for (int spec_index = 0; spec_index < graph->spec_count; ++spec_index) {
        AstNode *root = graph->specs[spec_index].ast;
        if (!root || root->type != AST_PROGRAM)
            continue;
        if (!xr_module_spec_owns_top_level_decl(&graph->specs[spec_index], identity->generic_decl))
            continue;
        for (int i = 0; i < root->as.program.count; ++i) {
            AstNode *candidate = root->as.program.statements[i];
            const char *candidate_name = NULL;
            if (candidate && candidate->type == AST_FUNCTION_DECL)
                candidate_name = candidate->as.function_decl.name;
            else if (candidate && candidate->type == AST_CLASS_DECL)
                candidate_name = candidate->as.class_decl.name;
            else if (candidate && candidate->type == AST_STRUCT_DECL)
                candidate_name = candidate->as.struct_decl.name;
            if (!candidate_name || strcmp(candidate_name, export_name) != 0)
                continue;
            XaGenericSpecializationFact fact;
            if (!xa_analyzer_get_generic_specialization(analyzer, candidate, &fact) ||
                !xa_generic_specialization_fact_valid(&fact) ||
                fact.generic_decl != identity->generic_decl ||
                fact.owner_decl != identity->owner_decl || fact.effect != identity->effect ||
                fact.receiver_type_arg_count != identity->receiver_type_arg_count ||
                fact.declaration_type_arg_count != identity->declaration_type_arg_count)
                continue;
            *spec_index_out = spec_index;
            *target_decl_out = candidate;
            return true;
        }
        return false;
    }
    return false;
}

static uint32_t mono_imported_nominal_symbol_id(const StructLiteralNode *literal,
                                                const AstNode *generic_decl, XaAnalyzer *analyzer) {
    if (!literal || !literal->type_path || !generic_decl || !analyzer)
        return 0u;

    AstNode *path = literal->type_path;
    if (path->type == AST_VARIABLE && path->as.variable.symbol_id != 0u) {
        XaSymbol *symbol = xa_analyzer_symbol_by_id(analyzer, path->as.variable.symbol_id);
        XaSymbolLinks *links = symbol ? xa_analyzer_get_links(analyzer, symbol) : NULL;
        if (symbol && symbol->is_imported && links && links->nominal_decl_node == generic_decl)
            return path->as.variable.symbol_id;
        return 0u;
    }

    if (path->type != AST_MEMBER_ACCESS || !path->as.member_access.object ||
        path->as.member_access.object->type != AST_VARIABLE)
        return 0u;
    AstNode *namespace_object = path->as.member_access.object;
    XaSymbol *namespace_symbol =
        namespace_object->as.variable.symbol_id
            ? xa_analyzer_symbol_by_id(analyzer, namespace_object->as.variable.symbol_id)
            : NULL;
    if (!namespace_symbol || namespace_symbol->kind != XA_SYM_MODULE)
        return 0u;
    return namespace_object->as.variable.symbol_id;
}

static bool mono_program_append(AstNode *root, AstNode *statement) {
    if (!root || root->type != AST_PROGRAM || !statement || !root->as.program.arena)
        return false;
    ProgramNode *program = &root->as.program;
    if (program->count >= program->capacity) {
        int capacity = program->capacity > 0 ? program->capacity * 2 : 8;
        AstNode **statements =
            (AstNode **) xr_arena_alloc_array(program->arena, sizeof(AstNode *), (size_t) capacity);
        if (!statements)
            return false;
        if (program->statements && program->count > 0)
            memcpy(statements, program->statements, (size_t) program->count * sizeof(AstNode *));
        program->statements = statements;
        program->capacity = capacity;
    }
    program->statements[program->count++] = statement;
    return true;
}

static AstNode *mono_private_type_path(AstNode *root, XaAnalyzer *analyzer, const char *name,
                                       const AstNode *source) {
    XrArena *arena = root && root->type == AST_PROGRAM ? root->as.program.arena : NULL;
    if (!arena || !analyzer || !analyzer->compiler_session || !name || !source)
        return NULL;
    AstNode *path = (AstNode *) xr_arena_alloc(arena, sizeof(*path));
    if (!path)
        return NULL;
    memset(path, 0, sizeof(*path));
    path->type = AST_VARIABLE;
    path->node_id = xr_compiler_session_next_ast_node_id(analyzer->compiler_session);
    path->line = source->line;
    path->column = source->column;
    path->end_line = source->end_line;
    path->end_column = source->end_column;
    path->as.variable.name = xr_arena_strdup(arena, name);
    return path->as.variable.name ? path : NULL;
}

static const char *mono_append_specialized_import(AstNode *root, uint32_t imported_symbol_id,
                                                  const XaGenericDecl *decl,
                                                  const XaGenericSpecializationFact *identity,
                                                  const char *export_name, XaAnalyzer *analyzer) {
    if (!root || root->type != AST_PROGRAM || imported_symbol_id == 0 || !decl || !decl->node ||
        !identity || !export_name || !analyzer)
        return NULL;
    ProgramNode *program = &root->as.program;
    XrArena *arena = program->arena;
    if (!arena)
        return NULL;

    int target_spec_index = -1;
    AstNode *target_decl = NULL;
    if (!mono_find_private_target(analyzer, identity, export_name, &target_spec_index,
                                  &target_decl))
        return NULL;

    for (int stmt_index = 0; stmt_index < program->count; stmt_index++) {
        AstNode *stmt = program->statements[stmt_index];
        if (!stmt || stmt->type != AST_IMPORT_STMT)
            continue;
        ImportStmtNode *import = &stmt->as.import_stmt;
        for (int member_index = 0; member_index < import->member_count; member_index++) {
            ImportMember *member = &import->members[member_index];
            if (member->has_private_target && member->private_target_decl == target_decl &&
                member->alias)
                return member->alias;
        }
    }

    for (int stmt_index = 0; stmt_index < program->count; stmt_index++) {
        AstNode *stmt = program->statements[stmt_index];
        if (!stmt || stmt->type != AST_IMPORT_STMT)
            continue;
        ImportStmtNode *import = &stmt->as.import_stmt;
        int original_index = -1;
        for (int member_index = 0; member_index < import->member_count; member_index++) {
            ImportMember *member = &import->members[member_index];
            if (member->symbol_id == imported_symbol_id)
                original_index = member_index;
        }
        bool namespace_import =
            import->member_count == 0 && import->symbol_id == imported_symbol_id;
        if (original_index < 0 && !namespace_import)
            continue;

        size_t alias_size = strlen(export_name) + 64u;
        char *alias = (char *) xr_arena_alloc(arena, alias_size);
        if (!alias)
            return NULL;
        (void) snprintf(alias, alias_size, "__xr_mono_import_%" PRIu32 "_%d_%s", imported_symbol_id,
                        target_spec_index, export_name);

        ImportStmtNode *target_import = import;
        if (namespace_import) {
            AstNode *private_stmt = (AstNode *) xr_arena_alloc(arena, sizeof(AstNode));
            if (!private_stmt)
                return NULL;
            memset(private_stmt, 0, sizeof(*private_stmt));
            private_stmt->type = AST_IMPORT_STMT;
            private_stmt->node_id =
                xr_compiler_session_next_ast_node_id(analyzer->compiler_session);
            private_stmt->line = stmt->line;
            private_stmt->column = stmt->column;
            private_stmt->as.import_stmt.module_name = xr_arena_strdup(arena, import->module_name);
            private_stmt->as.import_stmt.is_quoted = import->is_quoted;
            private_stmt->as.import_stmt.members =
                (ImportMember *) xr_arena_alloc_array(arena, sizeof(ImportMember), 1u);
            if (!private_stmt->as.import_stmt.module_name ||
                !private_stmt->as.import_stmt.members || !mono_program_append(root, private_stmt))
                return NULL;
            private_stmt->as.import_stmt.member_count = 1;
            target_import = &private_stmt->as.import_stmt;
        } else {
            int grown_count = import->member_count + 1;
            ImportMember *grown =
                (ImportMember *) xr_arena_alloc_array(arena, sizeof(ImportMember), grown_count);
            if (!grown)
                return NULL;
            memcpy(grown, import->members, (size_t) import->member_count * sizeof(ImportMember));
            import->members = grown;
            import->member_count = grown_count;
        }
        ImportMember *specialized = &target_import->members[target_import->member_count - 1];
        memset(specialized, 0, sizeof(*specialized));
        specialized->name = xr_arena_strdup(arena, export_name);
        specialized->alias = alias;
        if (!specialized->name)
            return NULL;
        specialized->has_private_target = true;
        specialized->private_target_spec_index = target_spec_index;
        specialized->private_generic_decl = decl->node;
        specialized->private_target_decl = target_decl;
        return specialized->alias;
    }
    return NULL;
}

static void mono_report_rewrite_failure(XaMonoCollector *collector, const AstNode *node,
                                        const char *name) {
    if (!collector || collector->rewrite_failed)
        return;
    collector->rewrite_failed = true;
    XrLocation loc = mono_node_loc(collector ? collector->analyzer : NULL, node);
    char message[256];
    snprintf(message, sizeof(message),
             "compiler could not materialize the exact imported specialization for '%s'",
             name ? name : "<generic>");
    xa_analyzer_add_diagnostic(collector->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_INTERNAL, message,
                               &loc);
}

static bool mono_publish_specialization(XaMonoCollector *collector, const AstNode *node,
                                        const XaGenericSpecializationFact *identity) {
    if (!collector || !collector->analyzer || !node ||
        !xa_generic_specialization_fact_valid(identity)) {
        mono_report_exact_type_identity(collector, NULL, NULL);
        return false;
    }
    if (xa_analyzer_set_generic_specialization(collector->analyzer, node, identity))
        return true;
    mono_report_exact_type_identity(collector, NULL, NULL);
    return false;
}

static bool mono_get_specialization(XaMonoCollector *collector, const AstNode *node,
                                    const XaGenericDecl *decl,
                                    XaGenericSpecializationFact *identity) {
    if (!collector || !collector->analyzer || !node || !decl || !identity ||
        !xa_analyzer_get_generic_specialization(collector->analyzer, node, identity) ||
        !xa_generic_specialization_fact_valid(identity) || identity->generic_decl != decl->node ||
        identity->owner_decl !=
            (decl->node && decl->node->type == AST_METHOD_DECL ? decl->owner : NULL)) {
        mono_report_exact_type_identity(collector, decl ? decl->name : NULL, NULL);
        return false;
    }
    return true;
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
            XaGenericDecl *decl = registry_find_call(registry, node, collector->analyzer);
            if (decl) {
                XaGenericSpecializationFact identity = {0};
                if (!mono_get_specialization(collector, node, decl, &identity))
                    return;
                const char *mangled = xa_mono_collector_lookup(collector, decl->name, &identity);
                if (mangled) {
                    uint32_t original_symbol_id = call->callee->as.variable.symbol_id;
                    XaSymbol *symbol =
                        xa_analyzer_symbol_by_id(collector->analyzer, original_symbol_id);
                    bool imported_decl = decl->defining_root != collector->rewrite_root;
                    if (imported_decl && symbol && symbol->is_imported) {
                        const char *alias = mono_append_specialized_import(
                            collector->rewrite_root, original_symbol_id, decl, &identity, mangled,
                            collector->analyzer);
                        if (!alias) {
                            mono_report_rewrite_failure(collector, node, decl->name);
                            return;
                        }
                        call->callee->as.variable.name = (char *) alias;
                        call->callee->as.variable.symbol_id = 0;
                    } else {
                        char *name = xr_strdup(mangled);
                        if (!name) {
                            mono_report_rewrite_failure(collector, node, decl->name);
                            return;
                        }
                        call->callee->as.variable.name = name;
                        call->callee->as.variable.symbol_id = 0;
                    }
                    // Clear type args (no longer generic call)
                    call->type_args = NULL;
                    call->type_arg_count = 0;
                } else if (!mono_specialization_contains_open_type(collector, &identity)) {
                    mono_report_rewrite_failure(collector, node, decl->name);
                    return;
                }
            }
        }
        if (call->type_arg_count > 0 && call->callee && call->callee->type == AST_MEMBER_ACCESS) {
            XaGenericDecl *decl = registry_find_call(registry, node, collector->analyzer);
            if (decl && decl->node && decl->node->type == AST_METHOD_DECL) {
                XaGenericSpecializationFact identity = {0};
                if (!mono_get_specialization(collector, node, decl, &identity))
                    return;
                const char *mangled = xa_mono_collector_lookup(collector, decl->name, &identity);
                if (mangled) {
                    char *name = xr_strdup(mangled);
                    if (!name) {
                        mono_report_rewrite_failure(collector, node, decl->name);
                        return;
                    }
                    call->callee->as.member_access.name = name;
                    call->type_args = NULL;
                    call->type_arg_count = 0;
                } else if (!mono_specialization_contains_open_type(collector, &identity)) {
                    mono_report_rewrite_failure(collector, node, decl->name);
                    return;
                }
            }
        }
        const char *member_name = NULL;
        if (mono_call_is_import_member_generic(call, import_aliases, &member_name)) {
            XaGenericDecl *decl = registry_find_call(registry, node, collector->analyzer);
            if (decl && decl->rewrite_member_access) {
                XaGenericSpecializationFact identity = {0};
                if (!mono_get_specialization(collector, node, decl, &identity))
                    return;
                const char *mangled = xa_mono_collector_lookup(collector, decl->name, &identity);
                if (mangled) {
                    AstNode *object = call->callee->as.member_access.object;
                    uint32_t namespace_symbol_id =
                        object && object->type == AST_VARIABLE ? object->as.variable.symbol_id : 0u;
                    const char *alias = mono_append_specialized_import(
                        collector->rewrite_root, namespace_symbol_id, decl, &identity, mangled,
                        collector->analyzer);
                    if (!alias) {
                        mono_report_rewrite_failure(collector, node, decl->name);
                        return;
                    }
                    AstNode *callee = call->callee;
                    memset(&callee->as, 0, sizeof(callee->as));
                    callee->type = AST_VARIABLE;
                    callee->as.variable.name = (char *) alias;
                    call->type_args = NULL;
                    call->type_arg_count = 0;
                } else if (!mono_specialization_contains_open_type(collector, &identity)) {
                    mono_report_rewrite_failure(collector, node, decl->name);
                    return;
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
            XaGenericDecl *decl = registry_find_nominal_site(registry, node, collector->analyzer);
            if (decl) {
                XaGenericSpecializationFact identity = {0};
                if (!mono_get_specialization(collector, node, decl, &identity))
                    return;
                const char *mangled = xa_mono_collector_lookup(collector, decl->name, &identity);
                if (mangled) {
                    const char *name = mangled;
                    if (decl->defining_root != collector->rewrite_root) {
                        uint32_t symbol_id = ne->class_symbol_id;
                        if (ne->module_name && collector->rewrite_root &&
                            collector->rewrite_root->type == AST_PROGRAM) {
                            ProgramNode *program = &collector->rewrite_root->as.program;
                            for (int stmt_index = 0; stmt_index < program->count; stmt_index++) {
                                AstNode *stmt = program->statements[stmt_index];
                                if (!stmt || stmt->type != AST_IMPORT_STMT ||
                                    stmt->as.import_stmt.member_count != 0)
                                    continue;
                                const char *alias = stmt->as.import_stmt.alias
                                                        ? stmt->as.import_stmt.alias
                                                        : stmt->as.import_stmt.module_name;
                                if (alias && strcmp(alias, ne->module_name) == 0) {
                                    symbol_id = stmt->as.import_stmt.symbol_id;
                                    break;
                                }
                            }
                        }
                        name =
                            mono_append_specialized_import(collector->rewrite_root, symbol_id, decl,
                                                           &identity, mangled, collector->analyzer);
                    } else {
                        name = xr_strdup(mangled);
                    }
                    if (!name) {
                        mono_report_rewrite_failure(collector, node, decl->name);
                        return;
                    }
                    ne->module_name = NULL;
                    ne->class_name = (char *) name;
                    ne->class_symbol_id = 0u;
                    // Keep type_args/type_arg_count for display and diagnostics.
                } else if (!mono_specialization_contains_open_type(collector, &identity)) {
                    mono_report_rewrite_failure(collector, node, decl->name);
                    return;
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
            XaGenericDecl *decl = registry_find_nominal_site(registry, node, collector->analyzer);
            if (decl) {
                XaGenericSpecializationFact identity = {0};
                if (!mono_get_specialization(collector, node, decl, &identity))
                    return;
                const char *mangled = xa_mono_collector_lookup(collector, decl->name, &identity);
                if (mangled) {
                    const char *name = mangled;
                    if (decl->defining_root != collector->rewrite_root) {
                        uint32_t symbol_id =
                            mono_imported_nominal_symbol_id(sl, decl->node, collector->analyzer);
                        name =
                            mono_append_specialized_import(collector->rewrite_root, symbol_id, decl,
                                                           &identity, mangled, collector->analyzer);
                    } else {
                        name = xr_strdup(mangled);
                    }
                    if (!name) {
                        mono_report_rewrite_failure(collector, node, decl->name);
                        return;
                    }
                    sl->struct_name = (char *) name;
                    sl->type_path = mono_private_type_path(collector->rewrite_root,
                                                           collector->analyzer, name, node);
                    if (!sl->type_path) {
                        mono_report_rewrite_failure(collector, node, decl->name);
                        return;
                    }
                    sl->type_args = NULL;
                    sl->type_arg_count = 0;
                } else if (!mono_specialization_contains_open_type(collector, &identity)) {
                    mono_report_rewrite_failure(collector, node, decl->name);
                    return;
                }
            }
        }
        for (int i = 0; i < sl->field_count; i++)
            rewrite_call_sites(sl->field_values[i], registry, collector, import_aliases);
        return;
    }

    if (node->type == AST_ENUM_CONSTRUCT) {
        EnumConstructNode *construct = &node->as.enum_construct;
        for (int i = 0; i < construct->field_count; i++)
            rewrite_call_sites(construct->field_values[i], registry, collector, import_aliases);
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

static bool qualify_mono_hof_callback_params(AstNode *cloned, const AstNode *origin,
                                             XaGenericSpecializationEffect effect) {
    if (!cloned || !origin)
        return false;
    if (effect != XA_GENERIC_SPECIALIZATION_EFFECT_NO_THROW)
        return true;
    XrParamNode **dst_params = NULL;
    XrParamNode **src_params = NULL;
    int dst_count = 0;
    int src_count = 0;
    if (cloned->type == AST_FUNCTION_DECL && origin->type == AST_FUNCTION_DECL) {
        dst_params = cloned->as.function_decl.params;
        dst_count = cloned->as.function_decl.param_count;
        src_params = origin->as.function_decl.params;
        src_count = origin->as.function_decl.param_count;
    } else if (cloned->type == AST_METHOD_DECL && origin->type == AST_METHOD_DECL) {
        dst_params = cloned->as.method_decl.params;
        dst_count = cloned->as.method_decl.param_count;
        src_params = origin->as.method_decl.params;
        src_count = origin->as.method_decl.param_count;
    } else {
        return false;
    }
    int limit = dst_count < src_count ? dst_count : src_count;
    for (int i = 0; i < limit; i++) {
        XrParamNode *dst_param = dst_params ? dst_params[i] : NULL;
        const XrParamNode *src_param = src_params ? src_params[i] : NULL;
        if (!dst_param || !dst_param->type || !src_param || !src_param->type ||
            src_param->type->kind != XR_TREF_FUNCTION || src_param->type->requires_nothrow)
            continue;
        /* Type refs are otherwise immutable and may be shared with the origin.
         * Copy just this node before adding the inferred specialization bit. */
        XrTypeRef *qualified = (XrTypeRef *) xr_calloc(1, sizeof(XrTypeRef));
        if (!qualified)
            return false;
        *qualified = *dst_param->type;
        qualified->requires_nothrow = true;
        dst_param->type = qualified;
    }
    return true;
}

static bool mono_append_method_clone(XrArena *arena, AstNode *owner_node, AstNode *method) {
    if (!arena || !owner_node || !method || method->type != AST_METHOD_DECL ||
        (owner_node->type != AST_CLASS_DECL && owner_node->type != AST_STRUCT_DECL))
        return false;
    ClassDeclNode *owner = owner_node->type == AST_CLASS_DECL ? &owner_node->as.class_decl
                                                              : &owner_node->as.struct_decl;
    if (owner->method_count < 0 || owner->method_count == INT_MAX)
        return false;
    size_t count = (size_t) owner->method_count;
    if (count > 0u && !owner->methods)
        return false;
    AstNode **methods =
        (AstNode **) xr_arena_alloc_array(arena, sizeof(AstNode *), count + 1u);
    if (!methods)
        return false;
    if (owner->methods && count > 0u)
        memcpy(methods, owner->methods, count * sizeof(AstNode *));
    methods[count] = method;
    owner->methods = methods;
    owner->method_count++;
    owner->mono_types_rewritten = true;
    return true;
}

// Materialize the graph-wide specialization closure in declaration-owning
// roots. The collector grows while specialized clones are scanned, and the
// exact identity dedup plus depth/instance budgets provide the fixpoint bound.
static void inject_mono_decls(XaMonoRootState *roots, int root_count, XaGenericRegistry *registry,
                              XaMonoCollector *collector, XrVMRuntime *isolate) {
    if (!roots || root_count <= 0 || !registry || !collector || collector->count == 0)
        return;

    XrAstCloneCtx clone_ctx = {
        .session = xr_compiler_session_current_for_isolate(isolate),
        .analyzer = collector->analyzer,
    };

    for (int i = 0; i < collector->count; i++) {
        if (collector->rewrite_failed)
            break;
        XaMonoInstance *inst = &collector->instances[i];
        XaGenericDecl *decl = registry_find_decl(registry, inst->identity.generic_decl);
        XaMonoRootState *root_state =
            decl ? mono_root_state_find(roots, root_count, decl->defining_root) : NULL;
        if (!decl || !decl->node || !root_state || !root_state->root ||
            root_state->root->type != AST_PROGRAM ||
            inst->identity.owner_decl !=
                (decl->node->type == AST_METHOD_DECL ? decl->owner : NULL) ||
            !mono_specialization_supported(&inst->identity)) {
            mono_report_exact_type_identity(collector, inst->generic_name, NULL);
            continue;
        }
        AstNode *root = root_state->root;
        ProgramNode *prog = &root->as.program;
        const XaMonoImportAliases *import_aliases = &root_state->imports;
        collector->analyzer->current_file =
            mono_root_source_path(collector->analyzer, root_state->spec_index);

        // Build one substitution map from receiver and declaration dimensions.
        int receiver_map_count = 0;
        ClassDeclNode *generic_owner = NULL;
        if (decl->node->type == AST_METHOD_DECL && decl->owner &&
            (decl->owner->type == AST_CLASS_DECL || decl->owner->type == AST_STRUCT_DECL)) {
            generic_owner = decl->owner->type == AST_CLASS_DECL ? &decl->owner->as.class_decl
                                                                : &decl->owner->as.struct_decl;
            receiver_map_count = generic_owner->type_param_count;
        }
        int declaration_map_count = decl->type_param_count;
        if (receiver_map_count < 0 || declaration_map_count < 0 ||
            (uint32_t) receiver_map_count != inst->identity.receiver_type_arg_count ||
            (uint32_t) declaration_map_count != inst->identity.declaration_type_arg_count ||
            receiver_map_count > INT_MAX - declaration_map_count) {
            mono_report_exact_type_identity(collector, inst->generic_name, NULL);
            continue;
        }
        int map_count = receiver_map_count + declaration_map_count;

        XrMonoTypeMap *map = (XrMonoTypeMap *) xr_calloc(map_count, sizeof(XrMonoTypeMap));
        if (!map) {
            mono_report_exact_type_identity(collector, inst->generic_name, NULL);
            continue;
        }
        for (int j = 0; j < receiver_map_count; ++j) {
            map[j].param_name = generic_owner->type_params[j]->name;
            map[j].concrete_type = inst->identity.receiver_type_args[j];
            map[j].concrete_semantic_type =
                collector->analyzer ? xa_analyzer_get_type_ref_type(
                                          collector->analyzer, inst->identity.receiver_type_args[j])
                                    : NULL;
        }
        for (int j = 0; j < declaration_map_count; ++j) {
            int map_index = receiver_map_count + j;
            map[map_index].param_name = decl->type_params[j]->name;
            map[map_index].concrete_type = inst->identity.declaration_type_args[j];
            map[map_index].concrete_semantic_type =
                collector->analyzer
                    ? xa_analyzer_get_type_ref_type(collector->analyzer,
                                                    inst->identity.declaration_type_args[j])
                    : NULL;
        }
        bool shadowed_type_parameter = false;
        for (int receiver_index = 0; receiver_index < receiver_map_count; ++receiver_index) {
            for (int declaration_index = receiver_map_count; declaration_index < map_count;
                 ++declaration_index) {
                shadowed_type_parameter |=
                    map[receiver_index].param_name && map[declaration_index].param_name &&
                    strcmp(map[receiver_index].param_name, map[declaration_index].param_name) == 0;
            }
        }
        if (shadowed_type_parameter) {
            xr_free(map);
            mono_report_exact_type_identity(collector, inst->generic_name, NULL);
            continue;
        }

        // Clone the generic function with type substitution
        clone_ctx.type_substitution_failed = false;
        AstNode *cloned = xr_ast_clone_ctx(decl->node, map, map_count, &clone_ctx);
        xr_free(map);

        if (!cloned || clone_ctx.type_substitution_failed) {
            mono_report_exact_type_identity(collector, inst->generic_name, NULL);
            continue;
        }

        // Rename the concrete declaration to its private specialization name.
        if (cloned->type == AST_FUNCTION_DECL) {
            xr_free(cloned->as.function_decl.name);
            cloned->as.function_decl.name = xr_strdup(inst->mangled_name);
            cloned->as.function_decl.type_param_count = 0;
            cloned->as.function_decl.type_params = NULL;
            if (!qualify_mono_hof_callback_params(cloned, decl->node, inst->identity.effect)) {
                mono_report_exact_type_identity(collector, inst->generic_name, NULL);
                continue;
            }
        } else if (cloned->type == AST_CLASS_DECL) {
            xr_free(cloned->as.class_decl.name);
            cloned->as.class_decl.name = xr_strdup(inst->mangled_name);
            cloned->as.class_decl.type_param_count = 0;
            cloned->as.class_decl.type_params = NULL;
            cloned->as.class_decl.is_monomorphized = true;
            cloned->as.class_decl.generic_origin_name = xr_strdup(inst->generic_name);
            cloned->as.class_decl.display_name = xr_strdup(inst->generic_name);
            /* Store concrete type arg display names for cold typename/debug metadata. */
            if (inst->identity.declaration_type_arg_count > 0u &&
                inst->identity.declaration_type_args) {
                const char **names = (const char **) xr_calloc(
                    inst->identity.declaration_type_arg_count, sizeof(const char *));
                if (!names) {
                    mono_report_exact_type_identity(collector, inst->generic_name, NULL);
                    continue;
                }
                for (uint32_t ti = 0u; ti < inst->identity.declaration_type_arg_count; ++ti)
                    names[ti] = mono_type_display_name(inst->identity.declaration_type_args[ti]);
                cloned->as.class_decl.mono_type_arg_names = names;
                cloned->as.class_decl.mono_type_arg_count =
                    (int) inst->identity.declaration_type_arg_count;
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
            if (inst->identity.declaration_type_arg_count > 0u &&
                inst->identity.declaration_type_args) {
                const char **names = (const char **) xr_calloc(
                    inst->identity.declaration_type_arg_count, sizeof(const char *));
                if (!names) {
                    mono_report_exact_type_identity(collector, inst->generic_name, NULL);
                    continue;
                }
                for (uint32_t ti = 0u; ti < inst->identity.declaration_type_arg_count; ++ti)
                    names[ti] = mono_type_display_name(inst->identity.declaration_type_args[ti]);
                cloned->as.struct_decl.mono_type_arg_names = names;
                cloned->as.struct_decl.mono_type_arg_count =
                    (int) inst->identity.declaration_type_arg_count;
            }
            if (decl->node && decl->node->type == AST_STRUCT_DECL) {
                decl->node->as.struct_decl.is_generic_skeleton = true;
            }
        } else if (cloned->type == AST_METHOD_DECL) {
            xr_free(cloned->as.method_decl.name);
            cloned->as.method_decl.name = xr_strdup(inst->mangled_name);
            cloned->as.method_decl.type_param_count = 0;
            cloned->as.method_decl.type_params = NULL;
            if (!qualify_mono_hof_callback_params(cloned, decl->node, inst->identity.effect)) {
                mono_report_exact_type_identity(collector, inst->generic_name, NULL);
                continue;
            }
        }

        bool clone_identity_complete = false;
        if (cloned->type == AST_FUNCTION_DECL)
            clone_identity_complete = cloned->as.function_decl.name != NULL;
        else if (cloned->type == AST_METHOD_DECL)
            clone_identity_complete = cloned->as.method_decl.name != NULL;
        else if (cloned->type == AST_CLASS_DECL)
            clone_identity_complete = cloned->as.class_decl.name &&
                                      cloned->as.class_decl.generic_origin_name &&
                                      cloned->as.class_decl.display_name;
        else if (cloned->type == AST_STRUCT_DECL)
            clone_identity_complete = cloned->as.struct_decl.name &&
                                      cloned->as.struct_decl.generic_origin_name &&
                                      cloned->as.struct_decl.display_name;
        if (!clone_identity_complete) {
            mono_report_exact_type_identity(collector, inst->generic_name, NULL);
            continue;
        }

        if (!mono_publish_specialization(collector, cloned, &inst->identity))
            continue;

        if (cloned->type == AST_METHOD_DECL) {
            AstNode *target_owner = decl->owner;
            if (inst->identity.receiver_type_arg_count > 0u) {
                XaMonoInstance *owner_instance = xa_mono_collector_find_type_instance(
                    collector, decl->owner, inst->identity.receiver_type_args,
                    (int) inst->identity.receiver_type_arg_count);
                target_owner = owner_instance ? owner_instance->materialized_decl : NULL;
                if (!target_owner) {
                    mono_report_exact_type_identity(collector, inst->generic_name, NULL);
                    continue;
                }
            }
            if (!mono_append_method_clone(prog->arena, target_owner, cloned)) {
                mono_report_rewrite_failure(collector, cloned, decl->name);
                continue;
            }
            inst->materialized_decl = cloned;
            if (import_aliases) {
                int saved_expanding = collector->expanding;
                collector->expanding = i;
                collect_instantiation_sites(cloned, registry, collector, import_aliases, false);
                collector->expanding = saved_expanding;
            }
            continue;
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

        // Program storage remains owned by the declaration root's arena. This
        // avoids mixing parser-arena and heap ownership as the fixpoint grows.
        if (prog->count >= prog->capacity) {
            int new_cap = prog->capacity ? prog->capacity * 2 : (prog->count + 16);
            AstNode **new_buf =
                (AstNode **) xr_arena_alloc_array(prog->arena, sizeof(AstNode *), (size_t) new_cap);
            if (!new_buf) {
                mono_report_rewrite_failure(collector, cloned, decl->name);
                continue;
            }
            if (prog->statements && prog->count > 0)
                memcpy(new_buf, prog->statements, (size_t) prog->count * sizeof(AstNode *));
            prog->statements = new_buf;
            prog->capacity = new_cap;
        }
        // Shift statements after insert_pos to make room
        if (insert_pos < prog->count) {
            memmove(&prog->statements[insert_pos + 1], &prog->statements[insert_pos],
                    (size_t) (prog->count - insert_pos) * sizeof(AstNode *));
        }
        prog->statements[insert_pos] = cloned;
        prog->count++;
        inst->materialized_decl = cloned;

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

XaMonoBudget xa_mono_default_budget(void) {
    return (XaMonoBudget) {
        .max_depth = XR_MONO_MAX_DEPTH,
        .max_instances = XR_MONO_MAX_INSTANCES,
    };
}

bool xa_mono_budget_valid(const XaMonoBudget *budget) {
    return budget && budget->max_depth != 0u && budget->max_depth <= XR_MONO_MAX_DEPTH &&
           budget->max_instances != 0u && budget->max_instances <= XR_MONO_MAX_INSTANCES;
}

static bool xa_mono_graph_pass_internal(AstNode **roots, int root_count, XrVMRuntime *isolate,
                                        const XaMonoBudget *budget, XaMonoUsage *usage,
                                        XaAnalyzer *analyzer) {
    if (!xa_mono_budget_valid(budget) || !usage || usage->instance_count > budget->max_instances ||
        usage->max_depth > budget->max_depth || root_count < 0 || (root_count > 0 && !roots))
        return false;
    if (root_count == 0)
        return true;

    bool ok = true;
    XaGenericRegistry registry;
    registry_init(&registry);

    XaMonoCollector collector;
    xa_mono_collector_init(&collector);
    collector.analyzer = analyzer;
    collector.max_depth = budget->max_depth;
    collector.max_instances = budget->max_instances;
    collector.instance_offset = usage->instance_count;
    const char *saved_current_file = analyzer ? analyzer->current_file : NULL;

    XaMonoRootState *states = (XaMonoRootState *) xr_calloc((size_t) root_count, sizeof(*states));
    if (!states) {
        xa_mono_collector_free(&collector);
        return false;
    }

    // Phase 1: register every generic declaration once with its absolute
    // declaration-owning root and retain one import view per root.
    for (int i = 0; i < root_count; ++i) {
        AstNode *root = roots[i];
        if (!root || root->type != AST_PROGRAM) {
            ok = false;
            goto cleanup;
        }
        states[i].root = root;
        states[i].spec_index = mono_root_spec_index(analyzer, root);
        analyzer->current_file = mono_root_source_path(analyzer, states[i].spec_index);
        mono_import_aliases_init(&states[i].imports);
        collect_import_aliases(root, &states[i].imports);
        if (!collect_generic_decls(root, states[i].spec_index, &registry)) {
            mono_report_exact_type_identity(&collector, "<generic-registry>", NULL);
            goto cleanup;
        }
    }
    if (registry.count == 0)
        goto cleanup;

    // Phase 2: seed one graph-wide collector from all roots. Open template
    // edges remain deferred until their enclosing concrete clone is scanned.
    for (int i = 0; i < root_count; ++i) {
        analyzer->current_file = mono_root_source_path(analyzer, states[i].spec_index);
        collect_instantiation_sites(states[i].root, &registry, &collector, &states[i].imports,
                                    false);
        if (collector.rewrite_failed)
            goto cleanup;
    }

    if (collector.count == 0)
        goto cleanup;

    // Phase 3: materialize the dynamic collector closure in exact owner roots.
    inject_mono_decls(states, root_count, &registry, &collector, isolate);
    if (collector.rewrite_failed)
        goto cleanup;
    for (int i = 0; i < collector.count; ++i) {
        if (!collector.instances[i].materialized_decl) {
            mono_report_rewrite_failure(&collector, collector.instances[i].identity.generic_decl,
                                        collector.instances[i].generic_name);
            goto cleanup;
        }
    }

    // Phase 4: only a closed, fully materialized graph may rewrite callers.
    for (int i = 0; i < root_count; ++i) {
        analyzer->current_file = mono_root_source_path(analyzer, states[i].spec_index);
        collector.rewrite_root = states[i].root;
        rewrite_call_sites(states[i].root, &registry, &collector, &states[i].imports);
        if (collector.rewrite_failed)
            goto cleanup;
    }

    // Debug: print mono stats if XRAY_MONO_DEBUG is set
#if XR_DEBUG
    if (getenv("XRAY_MONO_DEBUG")) {
        xr_log_debug("mono", "%d generic decls, %d mono instances", registry.count,
                     collector.count);
    }
#endif

cleanup:
    if (analyzer)
        analyzer->current_file = saved_current_file;
    for (int i = 0; i < root_count; ++i)
        mono_import_aliases_free(&states[i].imports);
    xr_free(states);
    ok = ok && !collector.budget_reported && !collector.rewrite_failed;
    usage->instance_count += (uint32_t) collector.count;
    if (collector.max_observed_depth > usage->max_depth)
        usage->max_depth = collector.max_observed_depth;
    xa_mono_collector_free(&collector);
    xr_free(registry.decls);
    return ok;
}

bool xa_mono_graph_pass(AstNode **roots, int root_count, XrVMRuntime *isolate,
                        const XaMonoBudget *budget, XaMonoUsage *usage, XaAnalyzer *analyzer) {
    XR_DCHECK(analyzer != NULL,
              "xa_mono_graph_pass: NULL analyzer; budgets need a diagnostic sink");
    return xa_mono_graph_pass_internal(roots, root_count, isolate, budget, usage, analyzer);
}
