/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_view_origin.h - Borrowed-result origin contracts
 *
 * KEY CONCEPT:
 *   Source origin names are resolved once into declaration-independent
 *   kind-and-ordinal rows. The normalized rows are function type identity.
 */

#ifndef XR_VIEW_ORIGIN_H
#define XR_VIEW_ORIGIN_H

#include <stdint.h>

typedef enum XrBorrowOriginSyntaxState {
    XR_BORROW_ORIGIN_OMITTED = 0,
    XR_BORROW_ORIGIN_EXPLICIT_SET = 1,
} XrBorrowOriginSyntaxState;

typedef enum AstBorrowOriginKind {
    AST_BORROW_ORIGIN_PARAM_NAME = 0,
    AST_BORROW_ORIGIN_RECEIVER = 1,
    AST_BORROW_ORIGIN_STATIC = 2,
} AstBorrowOriginKind;

typedef struct AstBorrowOriginRef {
    AstBorrowOriginKind kind;
    const char *name;
    int line;
    int column;
} AstBorrowOriginRef;

typedef enum XrViewOriginKind {
    XR_VIEW_ORIGIN_PARAM = 0,
    XR_VIEW_ORIGIN_RECEIVER = 1,
    XR_VIEW_ORIGIN_STATIC = 2,
} XrViewOriginKind;

typedef struct XrViewOrigin {
    XrViewOriginKind kind;
    int16_t param_ordinal;
} XrViewOrigin;

#endif  // XR_VIEW_ORIGIN_H
