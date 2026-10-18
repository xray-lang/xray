/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_effect_storage.h - Original-state owned effect summary arrays
 */
#ifndef XANALYZER_EFFECT_STORAGE_H
#define XANALYZER_EFFECT_STORAGE_H

#include "xanalyzer_symbol.h"

/* Copies the array before replacing its previous contents. Summary ids still
 * belong to their actual analyzer database; copying storage does not import ids. */
XR_FUNC XrCompileResourceStatus xa_symbol_links_set_param_effect_summaries(
    XaSymbolLinks *links, const XaParamEffectSummary *effects, int count);

#endif // XANALYZER_EFFECT_STORAGE_H
