/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_runtime_internal.h - Reads from the retained execution Final
 *
 * KEY CONCEPT:
 *   Frame admission reads the sole published owner. Queries allocate nothing,
 *   invoke no callback and never recover a retired compiler work allowance.
 */
#ifndef XXIR_EFFECT_RUNTIME_INTERNAL_H
#define XXIR_EFFECT_RUNTIME_INTERNAL_H
#include "xxir_effect_contract_internal.h"
#include "xxir_cell_provenance_internal.h"

/* Empty outputs only. The caller retains Effects and charges actual frame
 * admission on its runtime ledger; these views grant no execution permission. */
XR_FUNC XrXirStatus xir_effects_runtime_cell_role(const struct XrXirEffects *effects,
    uint32_t function,uint32_t parameter,XrXirCellProofRole *output);
/* Parameter storage borrows the same Final until the retained owner dies. */
XR_FUNC XrXirStatus xir_effects_runtime_entry_root(const struct XrXirEffects *effects,
    uint32_t function,XirEffectEntryRootView *output);
#endif // XXIR_EFFECT_RUNTIME_INTERNAL_H
