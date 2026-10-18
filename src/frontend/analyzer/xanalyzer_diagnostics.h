/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_diagnostics.h - Owned diagnostic mutation and cleanup
 */
#ifndef XANALYZER_DIAGNOSTICS_H
#define XANALYZER_DIAGNOSTICS_H
#include "xanalyzer.h"

/* The record, message and source path own their original state independently
 * of the parser, Session and analyzer registry. Cleanup never needs admission. */
XR_FUNC void xa_diagnostic_dispose(XaDiagnostic *diagnostic);
/* Only a record in this analyzer's list may be decorated. A failed append
 * preserves its message, location, code, links and reported flag. */
XR_FUNC XrCompileResourceStatus xa_analyzer_append_diagnostic_suffix(
    XaAnalyzer *analyzer, XaDiagnostic *diagnostic, const char *suffix);
#endif // XANALYZER_DIAGNOSTICS_H
