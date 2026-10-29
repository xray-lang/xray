/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef XHIR_ITEMS_H
#define XHIR_ITEMS_H
#include "xhir_diagnostics.h"
typedef struct XrHirItems XrHirItems;
typedef enum {XR_HIR_ITEM_DECLARATION,XR_HIR_ITEM_PARAMETER,XR_HIR_ITEM_PARAMETER_SCOPE,
    XR_HIR_ITEM_METHOD} XrHirItemKind;
typedef struct XrHirItemSpec {
    const char *name;
    uint32_t module,parent,node; /* Zero parent is the module; otherwise item+1. */
    int line,column;
    XrHirItemKind kind;
} XrHirItemSpec;
XR_FUNC XrCompileResourceStatus xr_hir_items_new(XrCompileResources *,XrHirItems **);
XR_FUNC void xr_hir_items_free(XrHirItems *);
/* Source bytes are borrowed only while the current compiler graph is live. */
XR_FUNC XrCompileResourceStatus xr_hir_items_source(XrHirItems *,uint32_t,const char *,
    const char *,size_t,const uint8_t *);
/* Register every declaration, including all members of an ambiguous group. */
XR_FUNC XrCompileResourceStatus xr_hir_items_reserve(XrHirItems *,const XrHirItemSpec *,uint32_t *);
/* Parameters bind only reserved declarations; generated module names may join. */
XR_FUNC XrCompileResourceStatus xr_hir_items_claim(XrHirItems *,const XrHirItemSpec *,uint32_t *);
XR_FUNC bool xr_hir_items_find(const XrHirItems *,uint32_t,uint32_t,const char *,uint32_t *);
/* Methods and private parameter scopes each have one AST-keyed parameter owner. */
XR_FUNC bool xr_hir_items_find_scope(const XrHirItems *,uint32_t,uint32_t,uint32_t *);
XR_FUNC bool xr_hir_items_module(const XrHirItems *,uint32_t,uint32_t *);
/* Receiving bindings are synchronous borrows, never part of a report. */
XR_FUNC bool xr_hir_items_bind(XrHirItems *,uint32_t,void *);
XR_FUNC bool xr_hir_items_binding_module(const XrHirItems *,const void *,uint32_t *);
XR_FUNC void *xr_hir_items_binding(const XrHirItems *,uint32_t);
XR_FUNC bool xr_hir_items_errors(const XrHirItems *);
XR_FUNC bool xr_hir_items_has_report(const XrHirItems *);
XR_FUNC XrFrontendReport *xr_hir_items_take_report(XrHirItems *);
#endif
