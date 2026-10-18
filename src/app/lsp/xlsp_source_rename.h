/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_rename.h - Source-backed rename candidate for an explicit source corpus
 *
 * KEY CONCEPT:
 *   Canonical Source owns binding semantics. Consumers retain exact owned facts
 *   and reject unavailable evidence without publishing partial edits.
 */
#ifndef XLSP_SOURCE_RENAME_H
#define XLSP_SOURCE_RENAME_H
#include "xlsp_source_workspace.h"
#include "xlsp_source_buffer.h"
typedef struct XlspSourceRenameInput {
    size_t entry;
    XrLspPosition position;
    const char *name;
    size_t name_length;
    bool prepare;
    const bool *known_versions; /* NULL means every input has a known editor version. */
} XlspSourceRenameInput;
/* Exact Source bindings only. A real candidate edit is checked again on the
 * same cumulative ledger, and its complete binding set must match the mapped
 * original set. No analyzer, guessed name positions, or partial edits.
 * The caller supplies the complete intended source scope. documentChanges
 * carries captured editor versions, or null for explicitly unversioned files.
 * JSON survives every producer. This is not workspace discovery or file I/O.
 * Missing import-selector/alias facts remain typed failures. */
XR_FUNC XrXirStatus xlsp_source_rename_json(const XrXirCompileContext *,const XlspSourceDocument *,size_t,
    const XlspSourceRenameInput *,XrJsonValue **);
#endif // XLSP_SOURCE_RENAME_H
