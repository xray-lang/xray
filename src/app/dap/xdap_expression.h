/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_expression.h - Source-checked expressions over owned lexical observations
 */
#ifndef XDAP_EXPRESSION_H
#define XDAP_EXPRESSION_H
#include "xdap_lexical.h"
typedef struct XdapExpressionRequest {
    XrSourceCompiler *compiler;
    const XdapLexicalView *lexical;
    const char *text;
    size_t length,source_limit;
} XdapExpressionRequest;
/* Identifiers read owned data. Ordinary Source inference and closed proof
 * feed finite scalar CTFE or an isolated, bounded data Program. Structural
 * inputs are rebuilt from actual owned observations; the target never runs.
 * Readonly Core calls retain their actual Source intrinsic identity; an
 * original same-name binding never becomes a debugger-provided builtin.
 * Unsupported expressions fail with the empty output unchanged. */
XR_FUNC XrXirStatus xdap_expression_evaluate(const XdapExpressionRequest *,XrXirValue *);
/* Syntax and supported expression shape only. Name/type admission still uses
 * the actual stopped frame; this check never supplies placeholder values. */
XR_FUNC XrXirStatus xdap_expression_validate(XrSourceCompiler *,const char *,size_t);
#endif // XDAP_EXPRESSION_H
