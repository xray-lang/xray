/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_storage.h - Analyzer scratch and type storage on one compiler state
 */
#ifndef XANALYZER_STORAGE_H
#define XANALYZER_STORAGE_H

#include "../../base/xcompile_state.h"

struct XaAnalyzer;

/* All three storage fields must be empty. Failure preserves them and the
 * original sticky state. Published arenas retain that same state after the
 * producing parser session is destroyed; no independent ledger is created. */
XR_FUNC XrCompileResourceStatus xa_analyzer_storage_open(
    XrCompileState *state, struct XaAnalyzer *analyzer);
/* Caller-owned analyzer storage is cleared before destruction. Cleanup
 * remains valid after resource failure and does not refund cumulative fees. */
XR_FUNC void xa_analyzer_storage_close(struct XaAnalyzer *analyzer);

#endif // XANALYZER_STORAGE_H
