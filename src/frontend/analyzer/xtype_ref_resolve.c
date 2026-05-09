/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtype_ref_resolve.c - Resolve XrTypeRef (syntax) to XrType* (runtime)
 *
 * Walks the XrTypeRef tree and maps each node to its runtime XrType*
 * counterpart.  Named types are resolved via class/prelude lookup;
 * primitives map 1-to-1 to their runtime singletons.
 */

#include "xtype_ref_resolve.h"
#include "../parser/xtype_ref.h"
#include "../../runtime/value/xtype.h"
#include "../../runtime/xisolate_api.h"
#include "../../base/xchecks.h"
#include "../../../stdlib/prelude/prelude.h"
#include <string.h>

/* Resolve child type refs recursively. */
static XrType *resolve_impl(XrayIsolate *X, const XrTypeRef *t);

/* Map a named type to its runtime XrType*.
 * Order: prelude → well-known singletons → generic class fallback. */
static XrType *resolve_named(XrayIsolate *X, const char *name) {
    XR_DCHECK(name != NULL, "resolve_named: NULL name");

    /* Prelude lookup (Array, Map, Set, Channel, Json, Bytes, ...) */
    const XrPreludeSymbols *symbols = xr_prelude_get_symbols(X);
    if (symbols) {
        const XrPreludeTypeEntry *entry = xr_prelude_lookup_type(symbols, name, strlen(name));
        if (entry) {
            switch ((XrPreludeKind) entry->kind) {
                case XR_PRELUDE_KIND_SIMPLE:
                    return xr_type_new_named_instance(X, entry->name);
                case XR_PRELUDE_KIND_SINGLETON:
                    if (strcmp(entry->name, "Json") == 0)
                        return xr_type_new_json(X);
                    return xr_type_new_named_instance(X, entry->name);
                case XR_PRELUDE_KIND_GENERIC_1:
                case XR_PRELUDE_KIND_GENERIC_2:
                    /* Bare name without type args — use unknown placeholders */
                    if (strcmp(entry->name, "Bytes") == 0)
                        return xr_type_new_bytes(X);
                    if (strcmp(entry->name, "Array") == 0)
                        return xr_type_new_array(X, xr_type_new_unknown(NULL));
                    if (strcmp(entry->name, "Set") == 0)
                        return xr_type_new_set(X, xr_type_new_unknown(NULL));
                    if (strcmp(entry->name, "Channel") == 0)
                        return xr_type_new_channel(X, xr_type_new_unknown(NULL));
                    if (strcmp(entry->name, "Map") == 0)
                        return xr_type_new_map(X, xr_type_new_unknown(NULL),
                                               xr_type_new_unknown(NULL));
                    return xr_type_new_named_instance(X, entry->name);
            }
        }
    }

    /* Well-known names not in prelude */
    if (strcmp(name, "Task") == 0)
        return xr_type_new_task(X, xr_type_new_unknown(NULL));
    if (strcmp(name, "Exception") == 0)
        return xr_type_new_named_instance(X, "Exception");

    /* Default: treat as class name */
    return xr_type_new_class(X, name);
}

/* Map a generic type (Name<T1, ...>) to its runtime XrType*. */
static XrType *resolve_generic(XrayIsolate *X, const XrTypeRef *t) {
    XR_DCHECK(t != NULL && t->name != NULL, "resolve_generic: NULL");
    const char *name = t->name;
    int nargs = t->nchildren;

    /* Resolve all type arguments first */
    XrType *args[16];
    for (int i = 0; i < nargs && i < 16; i++)
        args[i] = resolve_impl(X, t->children[i]);

    /* Dispatch to known container constructors */
    if (strcmp(name, "Array") == 0 && nargs >= 1)
        return xr_type_new_array(X, args[0]);
    if (strcmp(name, "Set") == 0 && nargs >= 1)
        return xr_type_new_set(X, args[0]);
    if (strcmp(name, "Channel") == 0 && nargs >= 1)
        return xr_type_new_channel(X, args[0]);
    if (strcmp(name, "Map") == 0 && nargs >= 2)
        return xr_type_new_map(X, args[0], args[1]);
    if (strcmp(name, "Task") == 0 && nargs >= 1)
        return xr_type_new_task(X, args[0]);

    /* Generic class instance */
    XrType **args_copy = NULL;
    if (nargs > 0) {
        args_copy = (XrType **) xr_malloc(sizeof(XrType *) * (size_t) nargs);
        if (args_copy) {
            for (int i = 0; i < nargs; i++)
                args_copy[i] = args[i];
        }
    }
    return xr_type_new_generic_instance(X, name, NULL, args_copy, nargs);
}

static XrType *resolve_impl(XrayIsolate *X, const XrTypeRef *t) {
    if (!t)
        return xr_type_new_unknown(NULL);

    switch ((XrTypeRefKind) t->kind) {
        case XR_TREF_INT:
            return xr_type_new_int(NULL);
        case XR_TREF_FLOAT:
            return xr_type_new_float(NULL);
        case XR_TREF_STRING:
            return xr_type_new_string(NULL);
        case XR_TREF_BOOL:
            return xr_type_new_bool(NULL);
        case XR_TREF_VOID:
            return xr_type_new_void(NULL);
        case XR_TREF_NULL:
            return xr_type_new_null(NULL);
        case XR_TREF_UNKNOWN:
            return xr_type_new_unknown(NULL);

        case XR_TREF_INT_WIDTH:
            return xr_type_new_int_width(X, t->native_width);
        case XR_TREF_FLOAT_WIDTH:
            return xr_type_new_float_width(X, t->native_width);

        case XR_TREF_NAMED:
            return resolve_named(X, t->name);
        case XR_TREF_GENERIC:
            return resolve_generic(X, t);

        case XR_TREF_OPTIONAL: {
            XrType *inner = resolve_impl(X, t->children[0]);
            return xr_type_new_optional(X, inner);
        }

        case XR_TREF_UNION: {
            XrType *members[XR_UNION_MAX_MEMBERS];
            int count = t->nchildren < XR_UNION_MAX_MEMBERS ? t->nchildren : XR_UNION_MAX_MEMBERS;
            for (int i = 0; i < count; i++)
                members[i] = resolve_impl(X, t->children[i]);
            return xr_type_new_union(X, members, count);
        }

        case XR_TREF_FUNCTION: {
            int nparam = t->nchildren > 0 ? t->nchildren - 1 : 0;
            XrType *params[16];
            for (int i = 0; i < nparam && i < 16; i++)
                params[i] = resolve_impl(X, t->children[i]);
            XrType *ret = t->nchildren > 0 ? resolve_impl(X, t->children[t->nchildren - 1])
                                           : xr_type_new_void(NULL);
            return xr_type_new_function(X, params, nparam, ret, false);
        }

        case XR_TREF_TUPLE: {
            XrType *elems[16];
            int count = t->nchildren < 16 ? t->nchildren : 16;
            for (int i = 0; i < count; i++)
                elems[i] = resolve_impl(X, t->children[i]);
            return xr_type_new_tuple(X, elems, count);
        }

        case XR_TREF_OBJECT: {
            const char **names = (const char **) t->field_names;
            int count = t->nchildren;
            XrType *types[64];
            if (count > 64)
                count = 64;
            for (int i = 0; i < count; i++)
                types[i] = resolve_impl(X, t->children[i]);
            XrType **type_ptrs = types;
            bool is_sealed = !t->extensible;
            return xr_type_new_json_with_fields(X, names, type_ptrs, count, is_sealed);
        }

        case XR_TREF_FIXED_ARRAY: {
            XrType *elem =
                t->nchildren > 0 ? resolve_impl(X, t->children[0]) : xr_type_new_unknown(NULL);
            return xr_type_new_fixed_array(X, elem, (int) t->fixed_length);
        }

        case XR_TREF_TYPE_PARAM:
            return xr_type_new_type_param(X, t->name, 0);
    }

    return xr_type_new_unknown(NULL);
}

XR_FUNC XrType *xr_tref_resolve(XrayIsolate *X, const XrTypeRef *tref) {
    return resolve_impl(X, tref);
}
