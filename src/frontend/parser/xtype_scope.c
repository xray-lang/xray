/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtype_scope.c - Parser-owned type alias / generic parameter scope.
 */

#include <string.h>
#include "xtype_scope.h"
#include "../../base/xchecks.h"
#include "../../base/xmalloc.h"

struct XrTypeScope {
    XrTypeScope *parent;
    XrTypeAlias *aliases;  // Singly linked list (small N per scope).
};

static XrTypeAlias *find_local(XrTypeScope *scope, const char *name) {
    if (!scope || !name) return NULL;
    for (XrTypeAlias *a = scope->aliases; a; a = a->next) {
        if (a->name && strcmp(a->name, name) == 0) return a;
    }
    return NULL;
}

XrTypeScope *xr_type_scope_new(XrTypeScope *parent) {
    XrTypeScope *scope = (XrTypeScope*)xr_malloc(sizeof(XrTypeScope));
    if (!scope) return NULL;
    scope->parent = parent;
    scope->aliases = NULL;
    return scope;
}

void xr_type_scope_free(XrTypeScope *scope) {
    if (!scope) return;
    XrTypeAlias *a = scope->aliases;
    while (a) {
        XrTypeAlias *next = a->next;
        if (a->name) xr_free((void*)a->name);
        xr_free(a);
        a = next;
    }
    xr_free(scope);
}

XrTypeAlias *xr_type_scope_define(XrTypeScope *scope,
                                  const char *name, XrType *type) {
    XR_DCHECK(scope != NULL, "xr_type_scope_define: NULL scope");
    XR_DCHECK(name != NULL, "xr_type_scope_define: NULL name");

    if (find_local(scope, name) != NULL) {
        return NULL;  // Duplicate definition in this scope.
    }

    XrTypeAlias *a = (XrTypeAlias*)xr_malloc(sizeof(XrTypeAlias));
    if (!a) return NULL;

    char *name_copy = xr_strdup(name);
    if (!name_copy) {
        xr_free(a);
        return NULL;
    }

    a->name = name_copy;
    a->type = type;
    a->next = scope->aliases;
    scope->aliases = a;
    return a;
}

XrTypeAlias *xr_type_scope_lookup(XrTypeScope *scope, const char *name) {
    if (!name) return NULL;
    for (XrTypeScope *s = scope; s; s = s->parent) {
        XrTypeAlias *a = find_local(s, name);
        if (a) return a;
    }
    return NULL;
}

XrTypeAlias *xr_type_scope_lookup_local(XrTypeScope *scope, const char *name) {
    return find_local(scope, name);
}

XrType *xr_type_scope_resolve(XrTypeScope *scope, const char *name) {
    XrTypeAlias *a = xr_type_scope_lookup(scope, name);
    return a ? a->type : NULL;
}
