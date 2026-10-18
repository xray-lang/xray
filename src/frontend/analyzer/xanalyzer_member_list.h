/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_member_list.h - Explicit ownership for completion query results
 */
#ifndef XANALYZER_MEMBER_LIST_H
#define XANALYZER_MEMBER_LIST_H

#include "xanalyzer_symbol.h"

struct XaBuiltinType;

typedef struct XaMemberList {
    XaSymbol **items;
    int count;
    bool owns_symbols;
} XaMemberList;

/* The initially empty output owns its array on the registry's original state.
 * Builtin results own detached symbols with real birth IDs, without publishing
 * scope bindings. Class results borrow the original symbols; their class and
 * scope owners must remain alive until the list is disposed. Neither branch
 * adopts a foreign symbol or extends borrowed semantic metadata lifetime.
 * Failure preserves the output and records the original sticky state. */
XR_FUNC XrCompileResourceStatus xa_member_list_builtins(
    XaSymbolRegistry *registry, const struct XaBuiltinType *type, XaMemberList *output);
XR_FUNC XrCompileResourceStatus xa_member_list_class(
    XaSymbolRegistry *registry, const XrClassInfo *info, XaMemberList *output);
/* Cleanup is valid after the producer Session and initial registry handle die,
 * does not admit new work, and releases only symbols actually owned by the list. */
XR_FUNC void xa_member_list_dispose(XaMemberList *list);

#endif // XANALYZER_MEMBER_LIST_H
