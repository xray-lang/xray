/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_signature.h - Non-admitting signature storage cleanup
 */
#ifndef XANALYZER_SIGNATURE_H
#define XANALYZER_SIGNATURE_H
#include "xanalyzer_symbol.h"
/* Array/string storage belongs to the factory-bound original state. Referenced
 * XrType and AstNode objects remain borrowed from their actual caller owners. */
XR_FUNC void xa_symbol_links_clear_function_sig(XaSymbolLinks *links);
XR_FUNC void xa_symbol_links_clear_type_params(XaSymbolLinks *links);
struct XrGenericParam;
/* Predeclaration stores names before the constraint-aware resolver runs. */
XR_FUNC XrCompileResourceStatus xa_symbol_links_set_generic_declarations(
    XaSymbolLinks *links, struct XrGenericParam **parameters, int count);
#endif // XANALYZER_SIGNATURE_H
