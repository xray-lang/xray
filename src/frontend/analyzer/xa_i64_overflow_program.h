/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */

#ifndef XA_I64_OVERFLOW_PROGRAM_H
#define XA_I64_OVERFLOW_PROGRAM_H

#include "xa_program_semantic_closure.h"

XR_FUNC XaProgramSemanticClosurePublishStatus xa_i64_overflow_program_publish(
    struct XaAnalyzer *analyzer, const struct AstNode *syntax,
    const struct XrModuleSpec *module_spec, XrProgramSemanticClosure **out, char *error,
    size_t error_size);

#endif  // XA_I64_OVERFLOW_PROGRAM_H
