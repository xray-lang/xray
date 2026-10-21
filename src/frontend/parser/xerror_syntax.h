/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xerror_syntax.h - Source-owned error clauses and propagation modes
 */
#ifndef XERROR_SYNTAX_H
#define XERROR_SYNTAX_H

struct XrTypeRef;

typedef enum XrThrowsSyntax {
    XR_THROWS_OMITTED,
    XR_THROWS_ERASED,
    XR_THROWS_TYPED
} XrThrowsSyntax;

/* Omission and erased throws have different meanings even without a type ref.
 * The type belongs to the same parse arena as the declaration or function type. */
typedef struct XrThrowsClause {
    XrThrowsSyntax syntax;
    struct XrTypeRef *type;
    int line;
    int column;
} XrThrowsClause;

typedef enum XrTryMode {
    XR_TRY_PROPAGATE,
    XR_TRY_OPTIONAL,
    XR_TRY_ASSERT
} XrTryMode;

#endif // XERROR_SYNTAX_H
