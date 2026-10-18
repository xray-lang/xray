/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_link_metadata.h - Owned auxiliary symbol metadata
 */
#ifndef XANALYZER_LINK_METADATA_H
#define XANALYZER_LINK_METADATA_H

#include "xanalyzer_symbol.h"

struct XrAttribute;

/* Copies declaration-independent rows; origin normalization belongs to the
 * actual function-type binder, not the metadata storage layer. */
XR_FUNC XrCompileResourceStatus xa_symbol_links_set_view_contract(
    XaSymbolLinks *links, const XaViewReturnContract *contract);
XR_FUNC XrCompileResourceStatus xa_symbol_links_ensure_inferred_params(XaSymbolLinks *links);
XR_FUNC XrCompileResourceStatus xa_symbol_links_set_deprecated_attrs(
    XaSymbolLinks *links, struct XrAttribute **attributes, int count);
XR_FUNC XrCompileResourceStatus xa_type_set_view_contract(XrCompileState *state,
    XrVMRuntime *isolate, XrType *type, const XrViewOrigin *origins, int count, bool was_elided);

#endif // XANALYZER_LINK_METADATA_H
