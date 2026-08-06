/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtype_ref.h - Syntax-level type references for AST nodes
 *
 * XrTypeRef is a lightweight, arena-allocated representation of a type
 * annotation as written in source code.  It captures the syntactic shape
 * (name, generics, nullable, union arms, etc.) without resolving to a
 * runtime XrType*.  Resolution happens in the analyzer via
 * xr_type_ref_resolve().
 *
 * This decouples the parser from the runtime type system: the parser is
 * a pure syntax phase, and the analyzer owns all type resolution.
 */

#ifndef XTYPE_REF_H
#define XTYPE_REF_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../../base/xdefs.h"
#include "../../shared/xr_param_mode.h"
#include "../../shared/xr_scalar_type.h"
#include "../../shared/xobject_row.h"

struct XrCompilerSession;
struct AstNode;

/* ========== Kind Enum ========== */

typedef enum {
    /* Primitive keywords */
    XR_TREF_INT,    /* int                              */
    XR_TREF_FLOAT,  /* float                            */
    XR_TREF_STRING, /* string                           */
    XR_TREF_BOOL,   /* bool                             */
    XR_TREF_RUNE,   /* char (Unicode scalar value)      */
    XR_TREF_UNIT,   /* unit `()` - the 0-arity tuple    */
    XR_TREF_NULL,   /* null                             */
    XR_TREF_ERROR,  /* compiler-only error recovery      */

    /* Numeric scalars: scalar_rep is semantic; builtin_spelling is syntax-only. */
    XR_TREF_INT_WIDTH,   /* int8 / int16 / int32 / int64 /
                            uint8 / uint16 / uint32 / uint64 */
    XR_TREF_FLOAT_WIDTH, /* float32 / float64                */

    /* Composite */
    XR_TREF_NAMED,       /* user class / enum / interface /
                            prelude name (Array, Json, ...)   */
    XR_TREF_GENERIC,     /* Name<T1, T2, ...>                */
    XR_TREF_CONST,       /* const T -- children[0] = inner   */
    XR_TREF_OPTIONAL,    /* T?  — children[0] = inner        */
    XR_TREF_UNION,       /* T | U — children[0..n-1]         */
    XR_TREF_FUNCTION,    /* (P1,..) -> R — children[0..n-2] = params,
                            children[n-1] = return type       */
    XR_TREF_TUPLE,       /* (T1, T2, ...) — children[0..n-1] */
    XR_TREF_OBJECT,      /* { f1: T1, ... } — field_names +
                            children as field types            */
    XR_TREF_FIXED_ARRAY, /* [T; N] — fixed_length_expr + children[0] */
    XR_TREF_TYPE_PARAM,  /* generic type parameter (T, U, ...)  */
} XrTypeRefKind;

/* Union member limit (mirrors XR_UNION_MAX_MEMBERS in xtype.h) */
#define XR_TREF_UNION_MAX 6

/* ========== The Type Reference ========== */

typedef struct XrTypeRef {
    uint8_t kind;      /* XrTypeRefKind                    */
    uint8_t nchildren; /* number of child type refs        */
    /* Source position of the annotation, 0 when synthesized. Type-level
     * diagnostics are raised long after the AST node that carried the
     * annotation is out of reach, so the position travels with the ref. */
    int line;
    int column;
    uint8_t scalar_rep;                /* XrNativeType scalar representation */
    uint8_t builtin_spelling;          /* XrSourceTypeSpelling or NONE       */
    XrObjectRowMode object_row_mode;   /* OBJECT: exact or trailing ...    */
    bool requires_nothrow;             /* FUNCTION: compiler-inferred HOF specialization */
    int fixed_length;                  /* FIXED_ARRAY: literal length if known, 0 otherwise */
    struct AstNode *fixed_length_expr; /* FIXED_ARRAY: source expression for N */
    const char *name;                  /* NAMED / GENERIC: type name
                                          (arena-allocated, NUL-terminated) */
    const char **field_names;          /* OBJECT: per-field names         */
    bool *field_readonly;
    XrParamMode *function_param_modes; /* FUNCTION: per-param modes       */
    struct XrTypeRef **children;       /* child type refs (arena array)   */
} XrTypeRef;

/* ========== Arena Constructors ========================================
 *
 * All allocate from the parse arena via the compiler session.  The returned
 * pointers are valid for the lifetime of the current parse.
 * ===================================================================== */

/* Primitives (singletons — safe to share across an arena lifetime) */
XR_FUNC XrTypeRef *xr_tref_int(struct XrCompilerSession *session);
XR_FUNC XrTypeRef *xr_tref_float(struct XrCompilerSession *session);
XR_FUNC XrTypeRef *xr_tref_string(struct XrCompilerSession *session);
XR_FUNC XrTypeRef *xr_tref_bool(struct XrCompilerSession *session);
XR_FUNC XrTypeRef *xr_tref_char(struct XrCompilerSession *session);
XR_FUNC XrTypeRef *xr_tref_unit(struct XrCompilerSession *session);
XR_FUNC XrTypeRef *xr_tref_null(struct XrCompilerSession *session);
XR_FUNC XrTypeRef *xr_tref_error(struct XrCompilerSession *session);

/* Native-width scalars */
XR_FUNC XrTypeRef *xr_tref_int_width(struct XrCompilerSession *session, uint8_t nw);
XR_FUNC XrTypeRef *xr_tref_float_width(struct XrCompilerSession *session, uint8_t nw);
XR_FUNC XrTypeRef *xr_tref_scalar(struct XrCompilerSession *session, XrSourceTypeSpelling spelling,
                                  uint8_t scalar_rep, bool float_family);

/* Named type (class / enum / prelude name, no generic args) */
XR_FUNC XrTypeRef *xr_tref_named(struct XrCompilerSession *session, const char *name);

/* Generic instance: Name<T1, T2, ...> */
XR_FUNC XrTypeRef *xr_tref_generic(struct XrCompilerSession *session, const char *name,
                                   XrTypeRef **args, int nargs);

/* Deep-readonly capability qualifier: const T. Reapplying it is idempotent. */
XR_FUNC XrTypeRef *xr_tref_const(struct XrCompilerSession *session, XrTypeRef *inner);

/* Optional: T? */
XR_FUNC XrTypeRef *xr_tref_optional(struct XrCompilerSession *session, XrTypeRef *inner);

/* Union: T | U | ... */
XR_FUNC XrTypeRef *xr_tref_union(struct XrCompilerSession *session, XrTypeRef **members, int count);

/* Attach the source position of the annotation this ref was parsed from. */
XR_FUNC void xr_tref_set_source_position(XrTypeRef *tref, int line, int column);

/* Function type: (P1, ...) -> R.
 * |params| has |nparam| entries; |ret| is the return type. */
XR_FUNC XrTypeRef *xr_tref_function(struct XrCompilerSession *session, XrTypeRef **params,
                                    int nparam, XrTypeRef *ret);
XR_FUNC XrTypeRef *xr_tref_function_with_modes(struct XrCompilerSession *session,
                                               XrTypeRef **params, const XrParamMode *param_modes,
                                               int nparam, XrTypeRef *ret);

/* Tuple: (T1, T2, ...) */
XR_FUNC XrTypeRef *xr_tref_tuple(struct XrCompilerSession *session, XrTypeRef **elems, int count);

/* Object / struct type literal: { f1: T1, f2: T2 } or { f1: T1, ... }. */
XR_FUNC XrTypeRef *xr_tref_object(struct XrCompilerSession *session, const char **field_names,
                                  XrTypeRef **field_types, const bool *field_readonly, int count,
                                  XrObjectRowMode row_mode);

/* Fixed-length array: [T; N] */
XR_FUNC XrTypeRef *xr_tref_fixed_array(struct XrCompilerSession *session, XrTypeRef *elem,
                                       int length);
XR_FUNC XrTypeRef *xr_tref_fixed_array_expr(struct XrCompilerSession *session, XrTypeRef *elem,
                                            struct AstNode *length_expr, int literal_length);

/* Generic type parameter: T, U, V, ... */
XR_FUNC XrTypeRef *xr_tref_type_param(struct XrCompilerSession *session, const char *name);

/* Copy a temporary type-reference pointer vector into the AST arena. */
XR_FUNC XrTypeRef **xr_tref_array_copy(struct XrCompilerSession *session, XrTypeRef **refs,
                                       int count);

/* ========== Queries ========== */

static inline bool xr_tref_is_nullable(const XrTypeRef *t) {
    return t && t->kind == XR_TREF_OPTIONAL;
}

/* Head name of a NAMED or GENERIC type reference.
 * Returns NULL for any other kind. Useful for the OOP `extends` /
 * `implements` clauses where the analyzer needs the bare interface or
 * class name regardless of whether type arguments were supplied. */
static inline const char *xr_tref_head_name(const XrTypeRef *t) {
    if (!t)
        return NULL;
    if (t->kind == XR_TREF_NAMED || t->kind == XR_TREF_GENERIC)
        return t->name;
    return NULL;
}

/* ========== Debug / Formatting ========== */

/* Return a human-readable string for a type ref (e.g. "Array<int>").
 * The string is arena-allocated and valid for the current parse. */
XR_FUNC const char *xr_tref_to_string(struct XrCompilerSession *session, const XrTypeRef *t);

/* Write type ref into caller-supplied buffer (no arena needed).
 * Returns number of characters written (excluding NUL). */
XR_FUNC int xr_tref_to_string_buf(const XrTypeRef *t, char *buf, int cap);

/* Like xr_tref_to_string_buf, but expands a named structural-object type into its field
 * structure instead of printing its name. Only the outermost ref is expanded.
 * For the right-hand side of `type Name = { ... }`, where printing the name
 * would emit the self-referential `type Name = Name`. */
XR_FUNC int xr_tref_to_string_buf_structural(const XrTypeRef *t, char *buf, int cap);

#endif  // XTYPE_REF_H
