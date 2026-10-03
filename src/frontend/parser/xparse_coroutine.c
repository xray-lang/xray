/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xparse_coroutine.c - Coroutine syntax parsing
 *
 * KEY CONCEPT:
 *   Parses go/await/select/defer/scope/channel/cancelled expressions.
 *   Extracted from xparse.c for maintainability.
 */

#include "xparse_internal.h"
#include "../../base/xchecks.h"
#include "../../base/xutf8.h"
#include "../lexer/xquoted_literal.h"

/*
 * Parse go expression
 * go fn()                      - Start coroutine on any thread
 * go fn() { block }()                - Inline closure call
 * go(name: "xxx") fn()              - Named coroutine
 *
 * Note: Thread binding uses Coro.lockThread() runtime API
 */
/*
 * Internal: parse go expression body with explicit link_mode.
 * Caller must have already consumed the 'go' keyword.
 */
static AstNode *parse_go_body(Parser *parser, uint8_t link_mode) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_go_body: NULL parser");
    int line = parser->previous.line;  // go keyword already consumed
    char *name = NULL;                 // Coroutine name (optional, owned heap)

    // Check if has option parameters go(name: "xxx")
    if (xr_parser_check(parser, TK_LPAREN)) {
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);  // Consume '('

        // Parse options, support multiple options separated by comma
        do {
            if (!xr_parser_check(parser, TK_NAME)) {
                do {
                    xr_parser_error_expected_name(parser, "expected option name (name)");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                goto fail;
            }

            Token opt_name = parser->current;

            if (opt_name.length == 4 && xr_parser_compare_bytes(parser, opt_name.start, "name", 4) == 0) {
                do {
                    xr_parser_advance(parser);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);  // Consume 'name'

                if (!xr_parser_match(parser, TK_COLON)) {
                    do {
                        xr_parser_error(parser, "expected ':' after name");
                        if (!xr_parser_healthy(parser)) return NULL;
                    } while (0);
                    goto fail;
                }

                // Parse name string
                if (!xr_parser_check(parser, TK_LITERAL_STRING)) {
                    do {
                        xr_parser_error(parser, "expected string as coroutine name");
                        if (!xr_parser_healthy(parser)) return NULL;
                    } while (0);
                    goto fail;
                }

                Token str_token = parser->current;
                do {
                    xr_parser_advance(parser);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                XrParsedQuoted payload = {0};
                const char *error = NULL;
                bool decode_escapes = str_token.escape_mode == XR_LITERAL_ESCAPED;
                if (xr_parser_decode_quoted(parser, &str_token, decode_escapes, &payload, &error) != XR_QUOTED_OK) {
                    do {
                        xr_parser_error_at_previous(parser,
                                                error ? error : "invalid coroutine name literal");
                        if (!xr_parser_healthy(parser)) return NULL;
                    } while (0);
                    goto fail;
                }
                if (xr_parser_find_byte(parser, payload.bytes, '\0', payload.length) != NULL ||
                    !xr_parser_utf8_validate(parser, (const char *) payload.bytes, payload.length)) {

                    do {
                        xr_parser_error_at_previous(
                        parser, "coroutine name must be valid UTF-8 without NUL bytes");
                        if (!xr_parser_healthy(parser)) return NULL;
                    } while (0);
                    goto fail;
                }
                char *str_copy = (char *) ast_alloc(parser->compiler_session, payload.length + 1);
                if (!xr_parser_healthy(parser)) return NULL;
                do { if (!ast_copy(parser->compiler_session, str_copy, payload.bytes, payload.length)) return NULL; } while (0);
                do { if (!ast_work(parser->compiler_session, 1)) return NULL; str_copy[payload.length] = '\0'; } while (0);

                name = str_copy;
            } else {
                do {
                    xr_parser_error(parser, "go(...) only supports name: option");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                goto fail;
            }
        } while (xr_parser_healthy(parser) && xr_parser_match(parser, TK_COMMA));

        if (!xr_parser_match(parser, TK_RPAREN)) {
            do {
                xr_parser_error(parser, "expected ')' to close go options");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            goto fail;
        }
    }

    /* The block-only spawn surface was removed. A lambda call is the one
     * inline form: it exposes the function boundary and keeps `go`'s AST
     * contract honest that the operand is always a CallExpr. */
    if (xr_parser_check(parser, TK_LBRACE)) {
        do {
            xr_parser_error_at_current(parser,
                                   "go takes a call; wrap an inline block as `go fn() { ... }()`");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        goto fail;
    }

    // go expr - parse function call expression
    AstNode *expr = xr_parse_precedence(parser, PREC_CALL);
    if (!xr_parser_healthy(parser)) return NULL;
    if (!expr) {
        do {
            xr_parser_error(parser, "go takes a call, such as `go f()`");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        goto fail;
    }
    if (expr->type != AST_CALL_EXPR) {
        do {
            xr_parser_error_at_previous(
            parser, "go takes a call; add `()` to call a function or use `go fn() { ... }()`");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        goto fail;
    }

    return xr_ast_go_expr(parser->compiler_session, expr, name, link_mode, line);

fail:
    return NULL;
}

AstNode *xr_parse_go_expr(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    return parse_go_body(parser, XR_LINK_NONE);
}

AstNode *xr_parse_go_expr_with_link(Parser *parser, uint8_t link_mode) {
    if (!xr_parser_healthy(parser)) return NULL;
    return parse_go_body(parser, link_mode);
}

/*
 * Parse await expression
 * await task
 * await(timeout: N) task
 * await all [tasks]
 * await all [tasks] into results
 * await any [tasks]
 * await anySuccess [tasks]
 */
AstNode *xr_parse_await_expr(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_await_expr: NULL parser");
    int line = parser->previous.line;  // await keyword already consumed

    // Check if has timeout parameter await(timeout: N)
    AstNode *timeout = NULL;
    if (xr_parser_check(parser, TK_LPAREN)) {
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);  // Consume '('

        if (xr_parser_check(parser, TK_MOVE) || xr_parser_check_name(parser, "move")) {
            do {
                xr_parser_error_at_current(
                parser,
                "await move is not allowed; await the Task directly because await performs the "
                "required terminal take automatically");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }

        // Expect 'timeout:'
        if (xr_parser_check(parser, TK_NAME)) {
            Token name = parser->current;
            if (name.length == 7 && xr_parser_compare_bytes(parser, name.start, "timeout", 7) == 0) {
                do {
                    xr_parser_advance(parser);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);  // Consume 'timeout'

                if (!xr_parser_match(parser, TK_COLON)) {
                    do {
                        xr_parser_error(parser, "expected ':' after timeout");
                        if (!xr_parser_healthy(parser)) return NULL;
                    } while (0);
                    return NULL;
                }

                // Parse timeout value (milliseconds)
                do {
                    timeout = xr_parse_expression(parser);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                if (!timeout) {
                    do {
                        xr_parser_error(parser, "expected timeout value expression");
                        if (!xr_parser_healthy(parser)) return NULL;
                    } while (0);
                    return NULL;
                }
            } else {
                do {
                    xr_parser_error(parser, "expected 'timeout' after await(");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                return NULL;
            }
        } else {
            do {
                xr_parser_error(parser, "expected 'timeout' after await(");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }

        if (!xr_parser_match(parser, TK_RPAREN)) {
            do {
                xr_parser_error(parser, "expected ')' after timeout parameter");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }
    }

    // Check if await all / await any / await anySuccess (context keywords)
    bool is_any = false;
    bool is_all = false;
    bool is_any_success = false;
    if (xr_parser_check(parser, TK_NAME)) {
        Token name = parser->current;
        if (name.length == 3 && xr_parser_compare_bytes(parser, name.start, "all", 3) == 0) {
            is_all = true;
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);  // Consume 'all'
        } else if (name.length == 3 && xr_parser_compare_bytes(parser, name.start, "any", 3) == 0) {
            is_any = true;
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);  // Consume 'any'
        } else if (name.length == 10 && xr_parser_compare_bytes(parser, name.start, "anySuccess", 10) == 0) {
            is_any_success = true;
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);  // Consume 'anySuccess'
        }
    }
    // Parse awaited expression
    AstNode *expr = xr_parse_precedence(parser, PREC_UNARY);
    if (!xr_parser_healthy(parser)) return NULL;
    if (!expr) {
        do {
            xr_parser_error(parser, "expected expression after await");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }

    AstNode *into = NULL;
    if (is_all && xr_parser_check(parser, TK_NAME)) {
        Token name = parser->current;
        if (name.length == 4 && xr_parser_compare_bytes(parser, name.start, "into", 4) == 0) {
            if (timeout) {
                do {
                    xr_parser_error_at_current(parser, "await all into does not support timeout");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                return NULL;
            }
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);  // Consume 'into'
            do {
                into = xr_parse_precedence(parser, PREC_UNARY);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            if (!into) {
                do {
                    xr_parser_error(parser, "expected result array expression after 'into'");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                return NULL;
            }
        }
    }

    return xr_ast_await_expr(parser->compiler_session, expr, timeout, into, is_any, is_all,
                             is_any_success, line);
}

/*
 * Parse Channel creation: Channel() or Channel(10).
 *
 * Reached from xr_parse_variable's contextual intercept after the
 * `Channel` identifier has been consumed and a `(` is the next token.
 * Producing a dedicated AST_CHANNEL_NEW node (rather than letting the
 * regular call_expr path take over) is what lets compile_channel_new,
 * the shared-variable preregister pass, and the select compiler all
 * recognise channel-typed variables by AST shape alone.
 */
AstNode *xr_parse_channel_new(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_channel_new: NULL parser");
    int line = parser->previous.line;  // `Channel` IDENT already consumed

    // Expect '('
    if (!xr_parser_match(parser, TK_LPAREN)) {
        do {
            xr_parser_error(parser, "expected '(' after Channel");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }

    // Parse optional buffer size
    AstNode *buffer_size = NULL;
    if (!xr_parser_check(parser, TK_RPAREN)) {
        do {
            buffer_size = xr_parse_expression(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        if (!buffer_size) {
            do {
                xr_parser_error(parser, "expected buffer size expression");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }
    }

    // Expect ')'
    if (!xr_parser_match(parser, TK_RPAREN)) {
        do {
            xr_parser_error(parser, "expected ')' to close Channel call");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }

    return xr_ast_channel_new(parser->compiler_session, buffer_size, line);
}

/*
 * Parse cancelled() expression
 */
AstNode *xr_parse_cancelled_expr(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_cancelled_expr: NULL parser");
    int line = parser->previous.line;  // cancelled keyword already consumed

    // Expect '()'
    if (!xr_parser_match(parser, TK_LPAREN)) {
        do {
            xr_parser_error(parser, "expected '(' after cancelled");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }
    if (!xr_parser_match(parser, TK_RPAREN)) {
        do {
            xr_parser_error(parser, "expected ')' to close cancelled call");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }

    return xr_ast_cancelled_expr(parser->compiler_session, line);
}

/*
 * Parse defer statement
 * defer { block }
 */
AstNode *xr_parse_defer_statement(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_defer_statement: NULL parser");
    int line = parser->previous.line;  // defer keyword already consumed

    if (!xr_parser_match(parser, TK_LBRACE)) {
        do {
            xr_parser_error_at_current(
            parser, "expected '{' after defer; deferred cleanup must use `defer { ... }`");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }
    AstNode *body = xr_parse_block(parser);
    if (!xr_parser_healthy(parser)) return NULL;
    return body ? xr_ast_defer_stmt(parser->compiler_session, body, line) : NULL;
}

/*
 * Parse select statement
 * select { msg from ch -> ..., value to ch -> ..., _ -> ... }
 */
AstNode *xr_parse_select_statement(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_select_statement: NULL parser");
    int line = parser->previous.line;  // select keyword already consumed

    // Expect '{'
    if (!xr_parser_match(parser, TK_LBRACE)) {
        do {
            xr_parser_error(parser, "expected '{' after select");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }

    // Parse case list
    AstNode **cases = NULL;
    int case_count = 0;
    int case_capacity = 8;
    do {
        cases = (AstNode **) ast_alloc_array(parser->compiler_session, sizeof(AstNode *),
                                         (size_t) case_capacity);
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    while (xr_parser_healthy(parser) && !xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF)) {
        AstNode *case_node = NULL;
        int case_line = parser->current.line;

        // Check if wildcard fallback case (_ -> ...)
        if (xr_parser_match(parser, TK_UNDERSCORE)) {
            // Wildcard fallback case
            if (!xr_parser_match(parser, TK_ARROW)) {
                do {
                    xr_parser_error(parser, "expected '->' after _");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                return NULL;
            }

            // Parse case body: { } block or expression
            AstNode *body;
            if (xr_parser_check(parser, TK_LBRACE)) {
                do {
                    xr_parser_advance(parser);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                do {
                    body = xr_parse_block(parser);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
            } else {
                do {
                    body = xr_parse_expression(parser);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
            }
            do {
                case_node = xr_ast_select_case(parser->compiler_session, NULL, NULL, NULL, body, false,
                                           true, false, case_line);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        } else {
            // Parse expression, then check if from or to
            // var from ch -> ... (recv) or val to ch -> ... (send)
            AstNode *first_expr = xr_parse_precedence(parser, PREC_CALL);
            if (!xr_parser_healthy(parser)) return NULL;
            if (!first_expr) {
                do {
                    xr_parser_error(parser, "expected select case expression");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                return NULL;
            }

            if (xr_parser_check_name(parser, "from")) {
                // recv case: var from ch -> ...
                do {
                    xr_parser_advance(parser);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);  // Consume 'from'

                // first_expr should be variable name
                if (first_expr->type != AST_VARIABLE) {
                    do {
                        xr_parser_error(parser, "expected variable name before 'from'");
                        if (!xr_parser_healthy(parser)) return NULL;
                    } while (0);
                    return NULL;
                }
                char *var_name = first_expr->as.variable.name;

                // Parse channel expression
                AstNode *channel = xr_parse_precedence(parser, PREC_CALL);
                if (!xr_parser_healthy(parser)) return NULL;
                if (!channel) {
                    do {
                        xr_parser_error(parser, "expected channel expression");
                        if (!xr_parser_healthy(parser)) return NULL;
                    } while (0);
                    return NULL;
                }

                // Expect '->'
                if (!xr_parser_match(parser, TK_ARROW)) {
                    do {
                        xr_parser_error(parser, "expected '->' after channel");
                        if (!xr_parser_healthy(parser)) return NULL;
                    } while (0);
                    return NULL;
                }

                // Parse case body
                AstNode *body;
                if (xr_parser_check(parser, TK_LBRACE)) {
                    do {
                        xr_parser_advance(parser);
                        if (!xr_parser_healthy(parser)) return NULL;
                    } while (0);
                    do {
                        body = xr_parse_block(parser);
                        if (!xr_parser_healthy(parser)) return NULL;
                    } while (0);
                } else {
                    do {
                        body = xr_parse_expression(parser);
                        if (!xr_parser_healthy(parser)) return NULL;
                    } while (0);
                }
                do {
                    case_node = xr_ast_select_case(parser->compiler_session, var_name, channel, NULL,
                                               body, false, false, false, case_line);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);

            } else if (xr_parser_check_name(parser, "to")) {
                // send case: val to ch -> ...
                do {
                    xr_parser_advance(parser);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);  // Consume 'to'

                AstNode *value = first_expr;  // Value to send

                // Parse channel expression
                AstNode *channel = xr_parse_precedence(parser, PREC_CALL);
                if (!xr_parser_healthy(parser)) return NULL;
                if (!channel) {
                    do {
                        xr_parser_error(parser, "expected channel expression");
                        if (!xr_parser_healthy(parser)) return NULL;
                    } while (0);
                    return NULL;
                }

                // Expect '->'
                if (!xr_parser_match(parser, TK_ARROW)) {
                    do {
                        xr_parser_error(parser, "expected '->' after channel");
                        if (!xr_parser_healthy(parser)) return NULL;
                    } while (0);
                    return NULL;
                }

                // Parse case body
                AstNode *body;
                if (xr_parser_check(parser, TK_LBRACE)) {
                    do {
                        xr_parser_advance(parser);
                        if (!xr_parser_healthy(parser)) return NULL;
                    } while (0);
                    do {
                        body = xr_parse_block(parser);
                        if (!xr_parser_healthy(parser)) return NULL;
                    } while (0);
                } else {
                    do {
                        body = xr_parse_expression(parser);
                        if (!xr_parser_healthy(parser)) return NULL;
                    } while (0);
                }
                do {
                    case_node = xr_ast_select_case(parser->compiler_session, NULL, channel, value, body,
                                               true, false, false, case_line);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);

            } else if (xr_parser_check(parser, TK_ARROW)) {
                // Check if after case: after ms -> ...
                if (first_expr->type == AST_VARIABLE &&
                    xr_parser_compare_string(parser, first_expr->as.variable.name, "after") == 0) {
                    do {
                        xr_parser_error(parser,
                                    "after requires timeout expression, format: after 1000 -> ...");
                        if (!xr_parser_healthy(parser)) return NULL;
                    } while (0);
                    return NULL;
                }
                // Possibly after ms -> ... form, first_expr is number
                do {
                    xr_parser_error(parser, "expected 'from' or 'to' in select case");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                return NULL;

            } else if (first_expr->type == AST_VARIABLE &&
                       xr_parser_compare_string(parser, first_expr->as.variable.name, "after") == 0) {
                // after case: after ms -> ...
                // Parse timeout expression (milliseconds)
                AstNode *timeout_expr = xr_parse_precedence(parser, PREC_CALL);
                if (!xr_parser_healthy(parser)) return NULL;
                if (!timeout_expr) {
                    do {
                        xr_parser_error(parser, "expected timeout expression after 'after'");
                        if (!xr_parser_healthy(parser)) return NULL;
                    } while (0);
                    return NULL;
                }

                // Expect '->'
                if (!xr_parser_match(parser, TK_ARROW)) {
                    do {
                        xr_parser_error(parser, "expected '->' after timeout expression");
                        if (!xr_parser_healthy(parser)) return NULL;
                    } while (0);
                    return NULL;
                }

                // Parse case body
                AstNode *body;
                if (xr_parser_check(parser, TK_LBRACE)) {
                    do {
                        xr_parser_advance(parser);
                        if (!xr_parser_healthy(parser)) return NULL;
                    } while (0);
                    do {
                        body = xr_parse_block(parser);
                        if (!xr_parser_healthy(parser)) return NULL;
                    } while (0);
                } else {
                    do {
                        body = xr_parse_expression(parser);
                        if (!xr_parser_healthy(parser)) return NULL;
                    } while (0);
                }
                // is_timeout = true, value = timeout_expr
                do {
                    case_node = xr_ast_select_case(parser->compiler_session, NULL, NULL, timeout_expr,
                                               body, false, false, true, case_line);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);

            } else {
                do {
                    xr_parser_error(parser, "expected 'from', 'to' or 'after' in select case");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                return NULL;
            }
        }

        if (case_node) {
            do {
                XR_PARSE_PUSH(parser, cases, case_count, case_capacity, case_node);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        }

        // Optional comma separator
        do {
            xr_parser_match(parser, TK_COMMA);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    }

    // Expect '}'
    if (!xr_parser_match(parser, TK_RBRACE)) {
        do {
            xr_parser_error(parser, "expected '}' to close select block");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }

    AstNode *result = xr_ast_select_stmt(parser->compiler_session, cases, case_count, line);
    if (!xr_parser_healthy(parser)) return NULL;
    return result;
}

/*
 * Parse scope block
 * scope { ... }
 * linked scope { ... }
 */
static AstNode *parse_scope_body(Parser *parser, uint8_t scope_mode) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_scope_body: NULL parser");
    int line = parser->previous.line;  // scope keyword already consumed

    // Expect '{'
    if (!xr_parser_match(parser, TK_LBRACE)) {
        do {
            xr_parser_error(parser, "expected '{' after scope");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }

    // Parse block content
    AstNode *body = xr_parse_block(parser);
    if (!xr_parser_healthy(parser)) return NULL;

    AstNode *node = xr_ast_scope_block(parser->compiler_session, body, scope_mode, line);
    if (!xr_parser_healthy(parser)) return NULL;
    if (body) {
        node->end_line = body->end_line;
        node->end_column = body->end_column;
    }
    return node;
}

AstNode *xr_parse_scope_block(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    return parse_scope_body(parser, XR_SCOPE_WAIT);
}

AstNode *xr_parse_scope_block_with_mode(Parser *parser, uint8_t scope_mode) {
    if (!xr_parser_healthy(parser)) return NULL;
    return parse_scope_body(parser, scope_mode);
}

/*
 * Parse move expression: move var
 * Explicit ownership transfer for go/ch.send arguments.
 */
AstNode *xr_parse_move_expr(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_move_expr: NULL parser");
    int line = parser->previous.line;
    int column = parser->previous.column;

    // Parse the expression after 'move' (must be a variable)
    AstNode *expr = xr_parse_precedence(parser, PREC_UNARY);
    if (!xr_parser_healthy(parser)) return NULL;
    if (!expr) {
        do {
            xr_parser_error(parser, "expected expression after 'move'");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }

    if (expr->type != AST_VARIABLE) {
        do {
            xr_parser_error(parser, "move requires a variable name");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }

    return xr_ast_move_expr(parser->compiler_session, expr, line, column);
}

/*
 * Parse unsafe expression: unsafe { stmt* }
 *
 * The braces delimit a statement block whose trailing expression (if any)
 * becomes the value of the unsafe expression. The wrapper is semantically
 * transparent — it only marks a region where the analyzer permits extern calls
 * and raw-pointer dereference. Used both as a value (var r = unsafe { sqrt(2.0) }
 * — a single trailing expression) and as a statement block executed for effect
 * (unsafe { p[0] = 1; p[1] = 2; free(p) }).
 */
AstNode *xr_parse_unsafe_expr(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_unsafe_expr: NULL parser");
    int line = parser->previous.line;
    int column = parser->previous.column;

    do {
        xr_parser_consume(parser, TK_LBRACE, "expected '{' after 'unsafe'");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    /* xr_parse_block assumes the '{' was already consumed and consumes the
     * matching '}'. The body is an AST_BLOCK; the analyzer/lowerer treat its
     * trailing expression statement as the unsafe expression's value. */
    bool saved_observed = parser->expr_value_observed;
    parser->expr_value_observed = true;
    AstNode *body = xr_parse_block(parser);
    if (!xr_parser_healthy(parser)) return NULL;
    parser->expr_value_observed = saved_observed;
    if (!body) {
        do {
            xr_parser_error(parser, "expected statements inside 'unsafe { }'");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }

    return xr_ast_unsafe_expr(parser->compiler_session, body, line, column);
}
