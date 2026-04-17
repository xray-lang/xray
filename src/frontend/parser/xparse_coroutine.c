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

/*
 * Parse go expression
 * go fn()                      - Start coroutine on any thread
 * go { block }                       - Anonymous closure coroutine
 * go(name: "xxx") fn()               - Named coroutine
 * go(priority: Coro.HIGH) fn()       - Specify priority (constant)
 * go(priority: 2) fn()               - Specify priority (number: 0=LOW, 1=NORMAL, 2=HIGH)
 *
 * Note: Thread binding uses Coro.lockThread() runtime API
 */
/*
 * Internal: parse go expression body with explicit link_mode.
 * Caller must have already consumed the 'go' keyword.
 */
static AstNode *parse_go_body(Parser *parser, uint8_t link_mode) {
    XR_DCHECK(parser != NULL, "parse_go_body: NULL parser");
    int line = parser->previous.line;  // go keyword already consumed
    char *name = NULL;                 // Coroutine name (optional, owned heap)
    AstNode *priority = NULL;          // Priority expression (optional, Coro.LOW/NORMAL/HIGH or number)

    // Check if has option parameters go(name: "xxx") or go(priority: Coro.HIGH)
    if (xr_parser_check(parser, TK_LPAREN)) {
        xr_parser_advance(parser);  // Consume '('

        // Parse options, support multiple options separated by comma
        do {
            if (!xr_parser_check(parser, TK_NAME)) {
                xr_parser_error_expected_name(parser, "expected option name (name or priority)");
                goto fail;
            }

            Token opt_name = parser->current;

            // Check if 'name' option
            if (opt_name.length == 4 && memcmp(opt_name.start, "name", 4) == 0) {
                xr_parser_advance(parser);  // Consume 'name'

                if (!xr_parser_match(parser, TK_COLON)) {
                    xr_parser_error(parser, "expected ':' after name");
                    goto fail;
                }

                // Parse name string
                if (!xr_parser_check(parser, TK_LITERAL_STRING)) {
                    xr_parser_error(parser, "expected string as coroutine name");
                    goto fail;
                }

                // Extract string value (remove quotes)
                Token str_token = parser->current;
                xr_parser_advance(parser);

                // Copy string (use malloc, name persists). If a previous
                // 'name:' option already allocated, free it before overwriting.
                int str_len = str_token.length - 2;  // Remove quotes
                char *str_copy = (char *)xr_malloc(str_len + 1);
                if (str_copy) {
                    memcpy(str_copy, str_token.start + 1, str_len);
                    str_copy[str_len] = '\0';
                    xr_free(name);
                    name = str_copy;
                }
            }
            // Check if 'priority' option
            else if (opt_name.length == 8 && memcmp(opt_name.start, "priority", 8) == 0) {
                xr_parser_advance(parser);  // Consume 'priority'

                if (!xr_parser_match(parser, TK_COLON)) {
                    xr_parser_error(parser, "expected ':' after priority");
                    goto fail;
                }

                // Parse priority expression (support Coro.LOW/NORMAL/HIGH or number)
                priority = xr_parse_precedence(parser, PREC_UNARY);
                if (!priority) {
                    xr_parser_error(parser, "expected priority expression (Coro.LOW/NORMAL/HIGH or number)");
                    goto fail;
                }
            }
            else {
                xr_parser_error(parser, "go(...) only supports name: and priority: options");
                goto fail;
            }
        } while (xr_parser_match(parser, TK_COMMA));

        if (!xr_parser_match(parser, TK_RPAREN)) {
            xr_parser_error(parser, "expected ')' to close go options");
            goto fail;
        }
    }

    // Check if anonymous closure
    if (xr_parser_check(parser, TK_LBRACE)) {
        // go { block } - anonymous closure coroutine
        xr_parser_advance(parser);  // Consume '{'
        AstNode *body = xr_parse_block(parser);

        // Create anonymous function expression to wrap block
        AstNode *fn_expr = xr_ast_function_decl(parser->X, "", NULL, 0, body, line);
        fn_expr->type = AST_FUNCTION_EXPR;  // Mark as expression not declaration
        return xr_ast_go_expr(parser->X, fn_expr, name, priority, link_mode, line);
    }

    // go expr - parse function call expression
    AstNode *expr = xr_parse_precedence(parser, PREC_CALL);
    if (!expr) {
        xr_parser_error(parser, "go requires function call or code block");
        goto fail;
    }

    return xr_ast_go_expr(parser->X, expr, name, priority, link_mode, line);

fail:
    xr_free(name);
    return NULL;
}

AstNode *xr_parse_go_expr(Parser *parser) {
    return parse_go_body(parser, XR_LINK_NONE);
}

AstNode *xr_parse_go_expr_with_link(Parser *parser, uint8_t link_mode) {
    return parse_go_body(parser, link_mode);
}

/*
 * Parse await expression
 * await task
 * await(timeout: N) task
 * await.all [tasks]
 * await.any [tasks]
 * await.anySuccess [tasks]
 */
AstNode *xr_parse_await_expr(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_await_expr: NULL parser");
    int line = parser->previous.line;  // await keyword already consumed

    // Check if has timeout parameter await(timeout: N)
    AstNode *timeout = NULL;
    if (xr_parser_check(parser, TK_LPAREN)) {
        xr_parser_advance(parser);  // Consume '('

        // Expect 'timeout:'
        if (xr_parser_check(parser, TK_NAME)) {
            Token name = parser->current;
            if (name.length == 7 && memcmp(name.start, "timeout", 7) == 0) {
                xr_parser_advance(parser);  // Consume 'timeout'

                if (!xr_parser_match(parser, TK_COLON)) {
                    xr_parser_error(parser, "expected ':' after timeout");
                    return NULL;
                }

                // Parse timeout value (milliseconds)
                timeout = xr_parse_expression(parser);
                if (!timeout) {
                    xr_parser_error(parser, "expected timeout value expression");
                    return NULL;
                }
            } else {
                xr_parser_error(parser, "expected 'timeout' after await(");
                return NULL;
            }
        } else {
            xr_parser_error(parser, "expected 'timeout' after await(");
            return NULL;
        }

        if (!xr_parser_match(parser, TK_RPAREN)) {
            xr_parser_error(parser, "expected ')' after timeout parameter");
            return NULL;
        }
    }

    // Check if await.all / await.any / await.anySuccess
    bool is_any = false;
    bool is_all = false;
    bool is_any_success = false;
    if (xr_parser_check(parser, TK_DOT)) {
        xr_parser_advance(parser);  // Consume '.'

        if (xr_parser_check(parser, TK_NAME)) {
            Token name = parser->current;
            if (name.length == 3 && memcmp(name.start, "any", 3) == 0) {
                is_any = true;
                xr_parser_advance(parser);  // Consume 'any' keyword (await.any)
            } else if (name.length == 3 && memcmp(name.start, "all", 3) == 0) {
                is_all = true;
                xr_parser_advance(parser);  // Consume 'all'
            } else if (name.length == 10 && memcmp(name.start, "anySuccess", 10) == 0) {
                is_any_success = true;
                xr_parser_advance(parser);  // Consume 'anySuccess'
            } else {
                xr_parser_error(parser, "expected 'all', 'any' or 'anySuccess' after await.");
                return NULL;
            }
        } else {
            xr_parser_error(parser, "expected 'all', 'any' or 'anySuccess' after await.");
            return NULL;
        }
    }
    // Parse awaited expression
    AstNode *expr = xr_parse_precedence(parser, PREC_UNARY);
    if (!expr) {
        xr_parser_error(parser, "expected expression after await");
        return NULL;
    }

    return xr_ast_await_expr(parser->X, expr, timeout, is_any, is_all, is_any_success, line);
}

/*
 * Parse Channel creation
 * Channel() or Channel(10)
 */
AstNode *xr_parse_channel_new(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_channel_new: NULL parser");
    int line = parser->previous.line;  // Channel keyword already consumed

    // Expect '('
    if (!xr_parser_match(parser, TK_LPAREN)) {
        xr_parser_error(parser, "expected '(' after Channel");
        return NULL;
    }

    // Parse optional buffer size
    AstNode *buffer_size = NULL;
    if (!xr_parser_check(parser, TK_RPAREN)) {
        buffer_size = xr_parse_expression(parser);
        if (!buffer_size) {
            xr_parser_error(parser, "expected buffer size expression");
            return NULL;
        }
    }

    // Expect ')'
    if (!xr_parser_match(parser, TK_RPAREN)) {
        xr_parser_error(parser, "expected ')' to close Channel call");
        return NULL;
    }

    return xr_ast_channel_new(parser->X, buffer_size, line);
}

/*
 * Parse cancelled() expression
 */
AstNode *xr_parse_cancelled_expr(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_cancelled_expr: NULL parser");
    int line = parser->previous.line;  // cancelled keyword already consumed

    // Expect '()'
    if (!xr_parser_match(parser, TK_LPAREN)) {
        xr_parser_error(parser, "expected '(' after cancelled");
        return NULL;
    }
    if (!xr_parser_match(parser, TK_RPAREN)) {
        xr_parser_error(parser, "expected ')' to close cancelled call");
        return NULL;
    }

    return xr_ast_cancelled_expr(parser->X, line);
}

/*
 * Parse defer statement
 * defer fn() or defer { block }
 */
AstNode *xr_parse_defer_statement(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_defer_statement: NULL parser");
    int line = parser->previous.line;  // defer keyword already consumed

    // Check if anonymous closure
    if (xr_parser_check(parser, TK_LBRACE)) {
        // defer { block } - anonymous closure
        xr_parser_advance(parser);  // Consume '{'
        AstNode *body = xr_parse_block(parser);

        // Create anonymous function expression to wrap block
        AstNode *fn_expr = xr_ast_function_expr(parser->X, NULL, 0, body, line);
        return xr_ast_defer_stmt(parser->X, fn_expr, line);
    }

    // defer expr - parse function call expression
    AstNode *expr = xr_parse_expression(parser);
    if (!expr) {
        xr_parser_error(parser, "defer requires function call or code block");
        return NULL;
    }

    return xr_ast_defer_stmt(parser->X, expr, line);
}

/*
 * Parse select statement
 * select { msg from ch => ..., to ch => ..., _ => ... }
 */
AstNode *xr_parse_select_statement(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_select_statement: NULL parser");
    int line = parser->previous.line;  // select keyword already consumed

    // Expect '{'
    if (!xr_parser_match(parser, TK_LBRACE)) {
        xr_parser_error(parser, "expected '{' after select");
        return NULL;
    }

    // Parse case list
    AstNode **cases = NULL;
    int case_count = 0;
    int case_capacity = 8;
    cases = (AstNode**)xr_malloc(sizeof(AstNode*) * case_capacity);

    while (!xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF)) {
        AstNode *case_node = NULL;
        int case_line = parser->current.line;

        // Check if default case (_ => ...)
        if (xr_parser_match(parser, TK_UNDERSCORE)) {
            // default case
            if (!xr_parser_match(parser, TK_ARROW)) {
                xr_parser_error(parser, "expected '=>' after _");
                xr_free(cases);
                return NULL;
            }

            // Parse case body: { } block or expression
            AstNode *body;
            if (xr_parser_check(parser, TK_LBRACE)) {
                xr_parser_advance(parser);
                body = xr_parse_block(parser);
            } else {
                body = xr_parse_expression(parser);
            }
            case_node = xr_ast_select_case(parser->X, NULL, NULL, NULL, body, false, true, false, case_line);
        } else {
            // Parse expression, then check if from or to
            // var from ch => ... (recv) or val to ch => ... (send)
            AstNode *first_expr = xr_parse_precedence(parser, PREC_CALL);
            if (!first_expr) {
                xr_parser_error(parser, "expected select case expression");
                xr_free(cases);
                return NULL;
            }

            if (xr_parser_check_name(parser, "from")) {
                // recv case: var from ch => ...
                xr_parser_advance(parser);  // Consume 'from'

                // first_expr should be variable name
                if (first_expr->type != AST_VARIABLE) {
                    xr_parser_error(parser, "expected variable name before 'from'");
                    xr_free(cases);
                    return NULL;
                }
                char *var_name = first_expr->as.variable.name;

                // Parse channel expression
                AstNode *channel = xr_parse_precedence(parser, PREC_CALL);
                if (!channel) {
                    xr_parser_error(parser, "expected channel expression");
                    xr_free(cases);
                    return NULL;
                }

                // Expect '=>'
                if (!xr_parser_match(parser, TK_ARROW)) {
                    xr_parser_error(parser, "expected '=>' after channel");
                    xr_free(cases);
                    return NULL;
                }

                // Parse case body
                AstNode *body;
                if (xr_parser_check(parser, TK_LBRACE)) {
                    xr_parser_advance(parser);
                    body = xr_parse_block(parser);
                } else {
                    body = xr_parse_expression(parser);
                }
                case_node = xr_ast_select_case(parser->X, var_name, channel, NULL, body, false, false, false, case_line);

            } else if (xr_parser_check_name(parser, "to")) {
                // send case: val to ch => ...
                xr_parser_advance(parser);  // Consume 'to'

                AstNode *value = first_expr;  // Value to send

                // Parse channel expression
                AstNode *channel = xr_parse_precedence(parser, PREC_CALL);
                if (!channel) {
                    xr_parser_error(parser, "expected channel expression");
                    xr_free(cases);
                    return NULL;
                }

                // Expect '=>'
                if (!xr_parser_match(parser, TK_ARROW)) {
                    xr_parser_error(parser, "expected '=>' after channel");
                    xr_free(cases);
                    return NULL;
                }

                // Parse case body
                AstNode *body;
                if (xr_parser_check(parser, TK_LBRACE)) {
                    xr_parser_advance(parser);
                    body = xr_parse_block(parser);
                } else {
                    body = xr_parse_expression(parser);
                }
                case_node = xr_ast_select_case(parser->X, NULL, channel, value, body, true, false, false, case_line);

            } else if (xr_parser_check(parser, TK_ARROW)) {
                // Check if after case: after ms => ...
                if (first_expr->type == AST_VARIABLE &&
                    strcmp(first_expr->as.variable.name, "after") == 0) {
                    xr_parser_error(parser, "after requires timeout expression, format: after 1000 => ...");
                    xr_free(cases);
                    return NULL;
                }
                // Possibly after ms => ... form, first_expr is number
                xr_parser_error(parser, "expected 'from' or 'to' in select case");
                xr_free(cases);
                return NULL;

            } else if (first_expr->type == AST_VARIABLE &&
                       strcmp(first_expr->as.variable.name, "after") == 0) {
                // after case: after ms => ...
                // Parse timeout expression (milliseconds)
                AstNode *timeout_expr = xr_parse_precedence(parser, PREC_CALL);
                if (!timeout_expr) {
                    xr_parser_error(parser, "expected timeout expression after 'after'");
                    xr_free(cases);
                    return NULL;
                }

                // Expect '=>'
                if (!xr_parser_match(parser, TK_ARROW)) {
                    xr_parser_error(parser, "expected '=>' after timeout expression");
                    xr_free(cases);
                    return NULL;
                }

                // Parse case body
                AstNode *body;
                if (xr_parser_check(parser, TK_LBRACE)) {
                    xr_parser_advance(parser);
                    body = xr_parse_block(parser);
                } else {
                    body = xr_parse_expression(parser);
                }
                // is_timeout = true, value = timeout_expr
                case_node = xr_ast_select_case(parser->X, NULL, NULL, timeout_expr, body, false, false, true, case_line);

            } else {
                xr_parser_error(parser, "expected 'from', 'to' or 'after' in select case");
                xr_free(cases);
                return NULL;
            }
        }

        if (case_node) {
            XR_PARSE_PUSH(cases, case_count, case_capacity, case_node);
        }

        // Optional comma separator
        xr_parser_match(parser, TK_COMMA);
    }

    // Expect '}'
    if (!xr_parser_match(parser, TK_RBRACE)) {
        xr_parser_error(parser, "expected '}' to close select block");
        xr_free(cases);
        return NULL;
    }

    AstNode *result = xr_ast_select_stmt(parser->X, cases, case_count, line);
    xr_free(cases);
    return result;
}

/*
 * Parse scope block
 * scope { ... }
 * linked scope { ... }
 * supervisor scope { ... }
 */
static AstNode *parse_scope_body(Parser *parser, uint8_t scope_mode) {
    XR_DCHECK(parser != NULL, "parse_scope_body: NULL parser");
    int line = parser->previous.line;  // scope keyword already consumed

    // Expect '{'
    if (!xr_parser_match(parser, TK_LBRACE)) {
        xr_parser_error(parser, "expected '{' after scope");
        return NULL;
    }

    // Parse block content
    AstNode *body = xr_parse_block(parser);

    return xr_ast_scope_block(parser->X, body, scope_mode, line);
}

AstNode *xr_parse_scope_block(Parser *parser) {
    return parse_scope_body(parser, XR_SCOPE_WAIT);
}

AstNode *xr_parse_scope_block_with_mode(Parser *parser, uint8_t scope_mode) {
    return parse_scope_body(parser, scope_mode);
}

/*
 * Parse move expression: move var
 * Explicit ownership transfer for go/ch.send arguments.
 */
AstNode *xr_parse_move_expr(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_move_expr: NULL parser");
    int line = parser->previous.line;
    int column = parser->previous.column;

    // Parse the expression after 'move' (must be a variable)
    AstNode *expr = xr_parse_precedence(parser, PREC_UNARY);
    if (!expr) {
        xr_parser_error(parser, "expected expression after 'move'");
        return NULL;
    }

    if (expr->type != AST_VARIABLE) {
        xr_parser_error(parser, "move requires a variable name");
        return NULL;
    }

    return xr_ast_move_expr(parser->X, expr, line, column);
}
