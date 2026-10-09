/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xparse_stmt.c - Control flow statement parsing
 *
 * KEY CONCEPT:
 *   Parses if/while/for/for-in/break/continue statements.
 *   Split from xparse.c to keep file sizes manageable.
 *   Also owns the effectless-expression-statement rule (E0208).
 */

#include "xparse_internal.h"
#include "../../base/xchecks.h"
#include "../../runtime/xerror_codes.h"

/* ========== Effectless Expression Statements (E0208) ==========
 *
 * An expression used as a statement must be able to do something; its value is
 * discarded, so a pure operator application is always a mistake. This is the
 * loud half of the L-04 statement-boundary rule in xparse.c: the shapes rejected
 * here are exactly the shapes a mis-continued line collapses into.
 *
 *     var x = a
 *     !b            // parsed as its own statement -> E0208, not a silent no-op
 *
 * Deliberately a deny-list, not an allow-list. Only node types that provably
 * cannot do anything are rejected, so a construct whose effect lives behind an
 * abstraction (a call, a member access that may run a getter, `await`, `match`)
 * is never falsely accused. `&&` / `||` / `??` are excluded because
 * `ready && start()` is an idiomatic guarded-effect statement.
 */
static bool expr_stmt_is_effectless(const AstNode *expr) {
    if (!expr)
        return false;

    switch (expr->type) {
        // Values: naming one discards it.
        case AST_LITERAL_INT:
        case AST_LITERAL_FLOAT:
        case AST_LITERAL_BIGINT:
        case AST_LITERAL_STRING:
        case AST_LITERAL_RUNE:
        case AST_LITERAL_REGEX:
        case AST_LITERAL_NULL:
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
        case AST_FIXED_BYTES_LITERAL:
        case AST_TEMPLATE_STRING:
        case AST_VARIABLE:
        case AST_FUNCTION_REF:
        case AST_THIS_EXPR:
        case AST_ARRAY_LITERAL:
        case AST_OBJECT_LITERAL:
        case AST_MAP_LITERAL:
        case AST_SET_LITERAL:
        case AST_TUPLE_LITERAL:
        // Pure operators: the result is the only thing they produce.
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_MOD:
        case AST_BINARY_BAND:
        case AST_BINARY_BOR:
        case AST_BINARY_BXOR:
        case AST_BINARY_LSHIFT:
        case AST_BINARY_RSHIFT:
        case AST_BINARY_EQ:
        case AST_BINARY_NE:
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_IS_EXPR:
        case AST_RANGE:
        // `expr!` only ever panics; as a whole statement it is the misparse
        // shape this rule exists to catch, never something worth writing.
        case AST_FORCE_UNWRAP:
            return true;
        // Parentheses are transparent: judge what they wrap.
        case AST_GROUPING:
            return expr_stmt_is_effectless(expr->as.grouping);
        default:
            return false;
    }
}

// Reject `expr` as a statement when it cannot do anything. `anchor` is the
// statement's first token, so the caret lands on the `!` / `-` / `(` that
// started the line.
void xr_parser_reject_effectless_expr_stmt(Parser *parser, const AstNode *expr, Token *anchor) {
    if (!xr_parser_healthy(parser)) return;
    XR_DCHECK(parser != NULL, "reject_effectless_expr_stmt: NULL parser");
    XR_DCHECK(anchor != NULL, "reject_effectless_expr_stmt: NULL anchor");

    // In the REPL the value of a bare expression is printed, so it is observed
    // rather than discarded.
    if (parser->expr_value_observed)
        return;
    if (parser->block_tail_value_observed) {
        bool tail = xr_parser_check(parser, TK_RBRACE);
        if (!xr_parser_healthy(parser)) return;
        if (xr_parser_check(parser, TK_SEMICOLON)) {
            XrParserStreamState saved = xr_parser_stream_save(parser);
            if (!xr_parser_healthy(parser)) return;
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return;
            } while (0);
            do {
                tail = xr_parser_check(parser, TK_RBRACE);
                if (!xr_parser_healthy(parser)) return;
            } while (0);
            do {
                xr_parser_stream_restore(parser, &saved);
                if (!xr_parser_healthy(parser)) return;
            } while (0);
        }
        if (tail) return;
    }
    if (!expr_stmt_is_effectless(expr))
        return;

    // The continuation hint only makes sense when the statement opens with a
    // token that could have continued the previous line — i.e. a dual-role
    // prefix/infix token. `42` on its own line is dead code, not a mis-wrap.
    const ParseRule *rule = xr_get_rule(anchor->type);
    if (!xr_parser_healthy(parser)) return;
    const char *note = (rule->prefix && rule->infix)
                           ? "if this line was meant to continue the previous one, move the "
                             "operator to the end of the previous line or wrap the whole "
                             "expression in parentheses"
                           : NULL;

    do {
        xr_parser_error_coded_note(parser, anchor, XR_ERR_SYN_EFFECTLESS_STMT,
                               "expression statement has no effect; its result is discarded", note);
        if (!xr_parser_healthy(parser)) return;
    } while (0);
}

/* ========== Control Flow Parsing ========== */

// Helper: propagate `body->end_*` as the enclosing statement's end span.
static inline void inherit_block_end(AstNode *stmt, AstNode *body) {
    if (!stmt || !body)
        return;
    stmt->end_line = body->end_line;
    stmt->end_column = body->end_column;
}

static char *token_to_ast_string(Parser *parser, Token tok) {
    if (!xr_parser_healthy(parser)) return NULL;
    char *s = (char *) ast_alloc(parser->compiler_session, (size_t) tok.length + 1);
    if (!xr_parser_healthy(parser)) return NULL;
    do { if (!ast_copy(parser->compiler_session, s, tok.start, (size_t) tok.length)) return NULL; } while (0);
    do { if (!ast_work(parser->compiler_session, 1)) return NULL; s[tok.length] = '\0'; } while (0);
    return s;
}

// Parse if statement
AstNode *xr_parse_if_statement(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    int line = parser->current.line;
    do {
        xr_parser_advance(parser);
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    do {
        xr_parser_consume(parser, TK_LPAREN, "expected '(' after if");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    AstNode *condition = xr_parse_expression(parser);
    if (!xr_parser_healthy(parser)) return NULL;
    do {
        xr_parser_consume(parser, TK_RPAREN, "expected ')' after if condition");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    if (!xr_parser_check(parser, TK_LBRACE)) {
        do {
            xr_parser_error_at_current(parser, "if statement requires braces { }");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }
    do {
        xr_parser_advance(parser);
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    AstNode *then_branch = xr_parse_block(parser);
    if (!xr_parser_healthy(parser)) return NULL;

    AstNode *else_branch = NULL;
    // Detect 'elif' (Python habit) — must check before else
    if (xr_parser_check(parser, TK_NAME) && parser->current.length == 4 &&
        xr_parser_compare_bytes(parser, parser->current.start, "elif", 4) == 0) {
        do {
            xr_parser_error_at_current(parser, "unknown keyword 'elif'. Use 'else if' in Xray");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        AstNode *stmt =
            xr_ast_if_stmt(parser->compiler_session, condition, then_branch, NULL, line);
        if (!xr_parser_healthy(parser)) return NULL;
        inherit_block_end(stmt, then_branch);
        return stmt;
    }
    if (xr_parser_match(parser, TK_ELSE)) {
        if (xr_parser_check(parser, TK_IF)) {
            do {
                else_branch = xr_parse_if_statement(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        } else {
            if (!xr_parser_check(parser, TK_LBRACE)) {
                do {
                    xr_parser_error_at_current(parser, "else requires braces { } or if statement");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                return NULL;
            }
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            do {
                else_branch = xr_parse_block(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        }
    }

    AstNode *stmt =
        xr_ast_if_stmt(parser->compiler_session, condition, then_branch, else_branch, line);
    if (!xr_parser_healthy(parser)) return NULL;
    // End span = end of else branch if present, otherwise end of then branch.
    inherit_block_end(stmt, else_branch ? else_branch : then_branch);
    return stmt;
}

// Parse while loop
AstNode *xr_parse_while_statement(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    int line = parser->previous.line;
    do {
        xr_parser_advance(parser);
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    do {
        xr_parser_consume(parser, TK_LPAREN, "expected '(' after while");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    AstNode *condition = xr_parse_expression(parser);
    if (!xr_parser_healthy(parser)) return NULL;
    do {
        xr_parser_consume(parser, TK_RPAREN, "expected ')' after while condition");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    if (!xr_parser_check(parser, TK_LBRACE)) {
        do {
            xr_parser_error_at_current(parser, "while statement requires braces { }");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }
    do {
        xr_parser_advance(parser);
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    AstNode *body = xr_parse_block(parser);
    if (!xr_parser_healthy(parser)) return NULL;

    AstNode *stmt = xr_ast_while_stmt(parser->compiler_session, NULL, condition, body, line);
    if (!xr_parser_healthy(parser)) return NULL;
    inherit_block_end(stmt, body);
    return stmt;
}

// Parse for loop: for (init; condition; increment) { ... }
AstNode *xr_parse_for_statement(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    int line = parser->previous.line;
    do {
        xr_parser_advance(parser);
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    do {
        xr_parser_consume(parser, TK_LPAREN, "expected '(' after for");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    AstNode *initializer = NULL;
    if (xr_parser_match(parser, TK_SEMICOLON)) {
        initializer = NULL;
    } else if (xr_parser_match(parser, TK_VAR)) {
        do {
            initializer = xr_parse_single_var_declaration(parser, 0);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        do {
            xr_parser_consume(parser, TK_SEMICOLON, "expected ';' after for initializer");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    } else {
        do {
            initializer = xr_parse_expr_statement(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        do {
            xr_parser_consume(parser, TK_SEMICOLON, "expected ';' after for initializer");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    }

    AstNode *condition = NULL;
    if (!xr_parser_check(parser, TK_SEMICOLON)) {
        do {
            condition = xr_parse_expression(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    }
    do {
        xr_parser_consume(parser, TK_SEMICOLON, "expected ';' after for condition");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    AstNode *increment = NULL;
    if (!xr_parser_check(parser, TK_RPAREN)) {
        do {
            increment = xr_parse_standalone_inc_dec(parser, true);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        if (!increment)
            do {
                increment = xr_parse_expression(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
    }
    do {
        xr_parser_consume(parser, TK_RPAREN, "expected ')' after for header");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    if (!xr_parser_check(parser, TK_LBRACE)) {
        do {
            xr_parser_error_at_current(parser, "for statement requires braces { }");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }
    do {
        xr_parser_advance(parser);
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    AstNode *body = xr_parse_block(parser);
    if (!xr_parser_healthy(parser)) return NULL;

    AstNode *stmt = xr_ast_for_stmt(parser->compiler_session, NULL, initializer, condition,
                                    increment, body, line);
    if (!xr_parser_healthy(parser)) return NULL;
    inherit_block_end(stmt, body);
    return stmt;
}

// Parse for-in loop: for (item in collection) { ... }
AstNode *xr_parse_for_in_statement(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    int line = parser->previous.line;
    do {
        xr_parser_advance(parser);
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    do {
        xr_parser_consume(parser, TK_LPAREN, "expected '(' after for");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    /* Tuple destructuring head: `for ((x, y) in coll) { body }`.
     *
     * We consume the tuple pattern eagerly, synthesise a hidden iteration
     * variable, and after the body is parsed prepend `var (...) = __tmp`
     * to it. The result is identical to writing the destructuring `var`
     * by hand, so all downstream stages see only the canonical form. */
    XrDestructurePattern *tuple_pattern = NULL;
    char *tuple_tmp_name = NULL;
    if (xr_parser_match(parser, TK_LPAREN)) {
        do {
            tuple_pattern = xr_parse_tuple_pattern(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        if (!tuple_pattern)
            return NULL;
        char buf[32];
        if (parser->tuple_head_sequence == UINT32_MAX) {
            do {
                xr_parser_error_at_current(parser, "too many tuple iteration heads");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }
        /* Parse order survives formatting; line numbers do not. */
        xr_parser_format(parser, buf, sizeof(buf), "__for_in_tuple_%u", parser->tuple_head_sequence++);
        do {
            tuple_tmp_name = (char *) ast_alloc(parser->compiler_session, xr_parser_string_length(parser, buf) + 1);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        do { if (!ast_copy(parser->compiler_session, tuple_tmp_name, buf, xr_parser_string_length(parser, buf) + 1)) return NULL; } while (0);
    }

    char *first_name = NULL;
    if (tuple_tmp_name) {
        first_name = tuple_tmp_name;
    } else {
        // Support _ as blank identifier (discards value)
        if (parser->current.type != TK_NAME && parser->current.type != TK_UNDERSCORE) {
            do {
                xr_parser_error_expected_name(parser, "expected loop variable name");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        do {
            first_name =
            (char *) ast_alloc(parser->compiler_session, (size_t) parser->previous.length + 1);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        do { if (!ast_copy(parser->compiler_session, first_name, parser->previous.start, parser->previous.length)) return NULL; } while (0);
        do { if (!ast_work(parser->compiler_session, 1)) return NULL; first_name[parser->previous.length] = '\0'; } while (0);
    }

    // Check for comma (key-value pattern)
    char *second_name = NULL;
    bool is_keyvalue = false;

    if (xr_parser_match(parser, TK_COMMA)) {
        is_keyvalue = true;

        // Support _ as blank identifier in key-value pattern
        if (parser->current.type != TK_NAME && parser->current.type != TK_UNDERSCORE) {
            do {
                xr_parser_error_expected_name(parser, "expected value variable name");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);

        do {
            second_name =
            (char *) ast_alloc(parser->compiler_session, (size_t) parser->previous.length + 1);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        do { if (!ast_copy(parser->compiler_session, second_name, parser->previous.start, parser->previous.length)) return NULL; } while (0);
        do { if (!ast_work(parser->compiler_session, 1)) return NULL; second_name[parser->previous.length] = '\0'; } while (0);
    }

    XrTypeRef *item_type = NULL;
    if (xr_parser_match(parser, TK_COLON)) {
        do {
            xr_parser_error_at_current(parser, "for-in bindings do not accept type annotations");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }

    do {
        xr_parser_consume(parser, TK_IN, "expected 'in' after loop variable");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    AstNode *collection = xr_parse_expression(parser);
    if (!xr_parser_healthy(parser)) return NULL;

    do {
        xr_parser_consume(parser, TK_RPAREN, "expected ')' after for-in header");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    if (!xr_parser_check(parser, TK_LBRACE)) {
        do {
            xr_parser_error_at_current(parser, "for-in statement requires braces { }");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }
    do {
        xr_parser_advance(parser);
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    AstNode *body = xr_parse_block(parser);
    if (!xr_parser_healthy(parser)) return NULL;

    /* Inject the synthesised destructuring `var` at the top of the
     * loop body. We rebuild the body block in place: the new array
     * holds destructure_decl followed by the original statements,
     * preserving program order. */
    if (tuple_pattern && body && body->type == AST_BLOCK) {
        AstNode *iter_var = xr_ast_variable(parser->compiler_session, tuple_tmp_name, line);
        if (!xr_parser_healthy(parser)) return NULL;
        AstNode *destructure_decl =
            xr_ast_destructure_decl(parser->compiler_session, tuple_pattern, iter_var, false, line);
        if (!xr_parser_healthy(parser)) return NULL;

        int old_count = body->as.block.count;
        int new_capacity =
            (old_count + 1 > body->as.block.capacity) ? old_count + 1 : body->as.block.capacity;
        AstNode **new_stmts = (AstNode **) ast_alloc_array(
            parser->compiler_session, sizeof(AstNode *), (size_t) new_capacity);
        if (!xr_parser_healthy(parser)) return NULL;
        new_stmts[0] = destructure_decl;
        for (int i = 0; xr_parser_step(parser) && (i < old_count); i++)
            new_stmts[i + 1] = body->as.block.statements[i];
        body->as.block.statements = new_stmts;
        body->as.block.count = old_count + 1;
        body->as.block.capacity = new_capacity;
    }

    AstNode *stmt =
        is_keyvalue ? xr_ast_for_in_keyvalue_stmt(parser->compiler_session, first_name, second_name,
                                                  NULL, item_type, collection, body, line)
                    : xr_ast_for_in_stmt(parser->compiler_session, NULL, first_name, item_type,
                                         collection, body, line);
    if (!xr_parser_healthy(parser)) return NULL;
    if (stmt) stmt->as.for_in_stmt.is_tuple_head = tuple_pattern != NULL;
    inherit_block_end(stmt, body);
    return stmt;
}

// Parse break statement
AstNode *xr_parse_break_statement(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    int line = parser->current.line;
    do {
        xr_parser_advance(parser);
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    char *label = NULL;
    if (xr_parser_check(parser, TK_NAME) && parser->current.line == line) {
        do {
            label = token_to_ast_string(parser, parser->current);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    }
    return xr_ast_break_stmt(parser->compiler_session, label, line);
}

// Parse continue statement
AstNode *xr_parse_continue_statement(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    int line = parser->current.line;
    do {
        xr_parser_advance(parser);
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    char *label = NULL;
    if (xr_parser_check(parser, TK_NAME) && parser->current.line == line) {
        do {
            label = token_to_ast_string(parser, parser->current);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    }
    return xr_ast_continue_stmt(parser->compiler_session, label, line);
}
