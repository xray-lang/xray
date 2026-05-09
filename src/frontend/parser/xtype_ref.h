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

struct XrayIsolate;

/* ========== Kind Enum ========== */

typedef enum {
    /* Primitive keywords */
    XR_TREF_INT,     /* int                              */
    XR_TREF_FLOAT,   /* float                            */
    XR_TREF_STRING,  /* string                           */
    XR_TREF_BOOL,    /* bool                             */
    XR_TREF_VOID,    /* void                             */
    XR_TREF_NULL,    /* null                             */
    XR_TREF_UNKNOWN, /* error recovery / unresolved      */

    /* Native-width scalars: native_width carries the width tag */
    XR_TREF_INT_WIDTH,   /* int8 / int16 / int32 / int64 /
                            uint8 / uint16 / uint32 / uint64 */
    XR_TREF_FLOAT_WIDTH, /* float32 / float64                */

    /* Composite */
    XR_TREF_NAMED,       /* user class / enum / interface /
                            prelude name (Array, Json, ...)   */
    XR_TREF_GENERIC,     /* Name<T1, T2, ...>                */
    XR_TREF_OPTIONAL,    /* T?  — children[0] = inner        */
    XR_TREF_UNION,       /* T | U — children[0..n-1]         */
    XR_TREF_FUNCTION,    /* fn(P1,..): R — children[0..n-2] = params,
                            children[n-1] = return type       */
    XR_TREF_TUPLE,       /* (T1, T2, ...) — children[0..n-1] */
    XR_TREF_OBJECT,      /* { f1: T1, ... } — field_names +
                            children as field types            */
    XR_TREF_FIXED_ARRAY, /* [N]T — fixed_length + children[0] */
    XR_TREF_TYPE_PARAM,  /* generic type parameter (T, U, ...)  */
} XrTypeRefKind;

/* Native-width tags — mirrors xstruct_layout.h values so the
 * resolver can map directly without a lookup table. */
#define XR_TREF_NW_I64 0
#define XR_TREF_NW_F64 1
#define XR_TREF_NW_BOOL 2
#define XR_TREF_NW_I8 3
#define XR_TREF_NW_I16 4
#define XR_TREF_NW_I32 5
#define XR_TREF_NW_U8 6
#define XR_TREF_NW_U16 7
#define XR_TREF_NW_U32 8
#define XR_TREF_NW_U64 9
#define XR_TREF_NW_F32 10

/* Union member limit (mirrors XR_UNION_MAX_MEMBERS in xtype.h) */
#define XR_TREF_UNION_MAX 6

/* ========== The Type Reference ========== */

typedef struct XrTypeRef {
    uint8_t kind;                /* XrTypeRefKind                    */
    uint8_t nchildren;           /* number of child type refs        */
    uint8_t native_width;        /* XR_TREF_NW_* for INT_WIDTH /
                                    FLOAT_WIDTH; 0 otherwise         */
    bool extensible;             /* OBJECT: has ... marker           */
    int16_t fixed_length;        /* FIXED_ARRAY: compile-time length */
    const char *name;            /* NAMED / GENERIC: type name
                                    (arena-allocated, NUL-terminated) */
    const char **field_names;    /* OBJECT: per-field names         */
    struct XrTypeRef **children; /* child type refs (arena array) */
} XrTypeRef;

/* ========== Arena Constructors ========================================
 *
 * All allocate from the parse arena via the isolate.  The returned
 * pointers are valid for the lifetime of the current parse.
 * ===================================================================== */

/* Primitives (singletons — safe to share across an arena lifetime) */
XR_FUNC XrTypeRef *xr_tref_int(struct XrayIsolate *X);
XR_FUNC XrTypeRef *xr_tref_float(struct XrayIsolate *X);
XR_FUNC XrTypeRef *xr_tref_string(struct XrayIsolate *X);
XR_FUNC XrTypeRef *xr_tref_bool(struct XrayIsolate *X);
XR_FUNC XrTypeRef *xr_tref_void(struct XrayIsolate *X);
XR_FUNC XrTypeRef *xr_tref_null(struct XrayIsolate *X);
XR_FUNC XrTypeRef *xr_tref_unknown(struct XrayIsolate *X);

/* Native-width scalars */
XR_FUNC XrTypeRef *xr_tref_int_width(struct XrayIsolate *X, uint8_t nw);
XR_FUNC XrTypeRef *xr_tref_float_width(struct XrayIsolate *X, uint8_t nw);

/* Named type (class / enum / prelude name, no generic args) */
XR_FUNC XrTypeRef *xr_tref_named(struct XrayIsolate *X, const char *name);

/* Generic instance: Name<T1, T2, ...> */
XR_FUNC XrTypeRef *xr_tref_generic(struct XrayIsolate *X, const char *name, XrTypeRef **args,
                                   int nargs);

/* Optional: T? */
XR_FUNC XrTypeRef *xr_tref_optional(struct XrayIsolate *X, XrTypeRef *inner);

/* Union: T | U | ... */
XR_FUNC XrTypeRef *xr_tref_union(struct XrayIsolate *X, XrTypeRef **members, int count);

/* Function type: fn(P1, ...): R
 * |params| has |nparam| entries; |ret| is the return type. */
XR_FUNC XrTypeRef *xr_tref_function(struct XrayIsolate *X, XrTypeRef **params, int nparam,
                                    XrTypeRef *ret);

/* Tuple: (T1, T2, ...) */
XR_FUNC XrTypeRef *xr_tref_tuple(struct XrayIsolate *X, XrTypeRef **elems, int count);

/* Object / struct type literal: { f1: T1, f2: T2 } or { f1: T1, ... }
 * |extensible| indicates trailing ... marker. */
XR_FUNC XrTypeRef *xr_tref_object(struct XrayIsolate *X, const char **field_names,
                                  XrTypeRef **field_types, int count, bool extensible);

/* Fixed-length array: [N]T */
XR_FUNC XrTypeRef *xr_tref_fixed_array(struct XrayIsolate *X, XrTypeRef *elem, int length);

/* Generic type parameter: T, U, V, ... */
XR_FUNC XrTypeRef *xr_tref_type_param(struct XrayIsolate *X, const char *name);

/* ========== Queries ========== */

static inline bool xr_tref_is_nullable(const XrTypeRef *t) {
    return t && t->kind == XR_TREF_OPTIONAL;
}

/* ========== Debug / Formatting ========== */

/* Return a human-readable string for a type ref (e.g. "Array<int>").
 * The string is arena-allocated and valid for the current parse. */
XR_FUNC const char *xr_tref_to_string(struct XrayIsolate *X, const XrTypeRef *t);

/* Write type ref into caller-supplied buffer (no arena needed).
 * Returns number of characters written (excluding NUL). */
XR_FUNC int xr_tref_to_string_buf(const XrTypeRef *t, char *buf, int cap);

#endif  // XTYPE_REF_H
