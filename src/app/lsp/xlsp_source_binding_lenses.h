/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_binding_lenses.h - Complete CodeLens counts from Source bindings
 *
 * KEY CONCEPT:
 *   Declaration display and reference counts consume canonical Source facts
 *   directly; semantic-token classification supplies no prerequisite authority.
 */
#ifndef XLSP_SOURCE_BINDING_LENSES_H
#define XLSP_SOURCE_BINDING_LENSES_H
#include "xlsp_source_bindings.h"
#include "../../base/xjson.h"
/* The caller supplies the complete intended corpus. Exact name spans, nominal
 * kinds and bound reference occurrences are owned Source observations. No
 * partial counts, lexical inference or guessed source positions are published.
 * Source producers die before the final independently owned JSON is built. */
XR_FUNC XrXirStatus xlsp_source_binding_lenses_json(const XrXirCompileContext *,
    const XlspSourceDocument *,size_t,size_t,XrJsonValue **);
#endif // XLSP_SOURCE_BINDING_LENSES_H
