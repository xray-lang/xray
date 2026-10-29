/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhir_literal.h - Checked numeric literals independent of parser storage
 */
#ifndef XHIR_LITERAL_H
#define XHIR_LITERAL_H
#include "../xir/xxir.h"
#include "../base/xcompile_request.h"

typedef struct XrHirLiteralOrigin {
    uint32_t node_id,module;
    int line,column,end_line,end_column;
} XrHirLiteralOrigin;
typedef enum XrHirLiteralKind {XR_HIR_LITERAL_INTEGER=1,XR_HIR_LITERAL_FLOAT} XrHirLiteralKind;
typedef struct XrHirLiteral {
    XrHirLiteralOrigin origin;
    XrXirType type;
    uint64_t bits;
    XrHirLiteralKind kind;
} XrHirLiteral;
/* Syntax and the actual type pool are borrowed for one synchronous check.
 * There is no pending typed node: scheduling incomplete context precedes it.
 * A successful node owns all its facts by value and retains no AST or text. */
typedef struct XrHirLiteralInput {
    XrHirLiteralOrigin origin;
    bool decimal,negative,expected;
    uint64_t magnitude;
    const char *decimal_text;
    size_t decimal_length;
    XrXirType expected_type;
} XrHirLiteralInput;
/* Failure preserves output. Reason borrows a static diagnostic string. */
XR_FUNC XrXirStatus xr_hir_literal_check(const XrXirTypes *,const XrHirLiteralInput *,
    XrCompileRequestControl *,XrHirLiteral *,const char **reason);
/* Consumes only a trusted checked node; invalid typed facts are compiler bugs. */
XR_FUNC XrXirInstruction xr_hir_literal_instruction(const XrHirLiteral *);
#endif // XHIR_LITERAL_H
