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
/* Identifiers read owned data. Scalar operations pass through ordinary Source
 * inference, closed proof and finite CTFE. No target execution or write occurs.
 * Unsupported expressions fail with the empty output unchanged. */
XR_FUNC XrXirStatus xdap_expression_evaluate(const XdapExpressionRequest *,XrXirValue *);
/* Syntax and supported expression shape only. Name/type admission still uses
 * the actual stopped frame; this check never supplies placeholder values. */
XR_FUNC XrXirStatus xdap_expression_validate(XrSourceCompiler *,const char *,size_t);
#endif // XDAP_EXPRESSION_H
