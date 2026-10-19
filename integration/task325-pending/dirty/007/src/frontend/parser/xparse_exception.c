/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xparse_exception.c - Exception statement parsing
 */
#include "xparse_internal.h"
#include "../../base/xchecks.h"
#include "xtype_ref.h"

// ========== Exception handling parse functions ==========

/* Simple enum head of a variant pattern, used only as the analyzer's type
 * hint. Module-qualified and explicitly instantiated heads yield NULL; the
 * XIR source owner resolves every pattern root from the pattern path itself. */
static const char *catch_pattern_enum_head_name(AstNode *pattern) {
    if (!pattern)
        return NULL;
    AstNode *path = NULL;
    if (pattern->type == AST_PATTERN_ADT)
        path = pattern->as.pattern_adt.variant;
    else if (pattern->type == AST_PATTERN_LITERAL)
        path = pattern->as.pattern_literal.value;
    if (!path)
        return NULL;
    if (path->type == AST_ENUM_ACCESS)
        return path->as.enum_access.enum_name;
    if (path->type == AST_MEMBER_ACCESS && path->as.member_access.object &&
        path->as.member_access.object->type == AST_VARIABLE)
        return path->as.member_access.object->as.variable.name;
    return NULL;
}

/* A catch pattern names enum variants: every alternative is a qualified
 * variant path, optionally followed by its named payload pattern. Bare names
 * inside the parentheses are bindings, so literals, ranges, wildcards and
 * structural patterns cannot stand in for a catch-all or a type filter. */
static bool catch_pattern_names_variants(Parser *parser, AstNode *pattern) {
    if (!pattern)
        return false;
    int count = 1;
    AstNode **alternatives = &pattern;
    if (pattern->type == AST_PATTERN_MULTI) {
        count = pattern->as.pattern_multi.count;
        alternatives = pattern->as.pattern_multi.patterns;
    }
    if (count <= 0 || !alternatives)
        return false;
    for (int i = 0; xr_parser_step(parser) && i < count; i++) {
        AstNode *alternative = alternatives[i];
        if (!alternative)
            return false;
        if (alternative->type == AST_PATTERN_ADT)
            continue;
        AstNode *path = alternative->type == AST_PATTERN_LITERAL
                            ? alternative->as.pattern_literal.value
                            : NULL;
        if (!path || (path->type != AST_MEMBER_ACCESS && path->type != AST_ENUM_ACCESS))
            return false;
    }
    return true;
}

/*
 * Parse try-catch statement.
 * Every ordinary catch header is parenthesized; only a panic clause may omit
 * its header:
 *   try { ... }
 *   catch (e: NetErr)                   { ... }   // typed binding
 *   catch (DbErr.QueryFailed { query }) { ... }   // enum variant pattern
 *   catch (NetErr.Timeout, NetErr.Lost) { ... }   // same-enum alternatives
 *   catch (e)                           { ... }   // catch-all binding
 *   catch panic (p)                     { ... }   // recoverable-fault boundary
 *   catch panic                         { ... }
 * Inside the parentheses a name followed by ':' or ')' is a binding; anything
 * else is a pattern. There is no `finally`; use `defer` for cleanup.
 */
AstNode *xr_parse_try_statement(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_try_statement: NULL parser");
    int line = parser->current.line;

    // Consume 'try'
    do {
        xr_parser_advance(parser);
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    // Parse try block
    do {
        xr_parser_consume(parser, TK_LBRACE, "expected '{' after try");
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);
    AstNode *try_body = xr_parse_block(parser);
    if (!xr_parser_healthy(parser)) return NULL;

    // Parse zero or more catch clauses
    XrCatchClause **clauses = NULL;
    int catch_count = 0;
    int catch_cap = 0;

    while (xr_parser_healthy(parser) && xr_parser_check(parser, TK_CATCH)) {
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);  // consume 'catch'

        /* `catch panic` optionally binds its panic value with `(p)`.
         * `panic` is a contextual keyword here, not a reserved word. */
        bool is_panic = false;
        if (parser->current.type == TK_NAME && parser->current.length == 5 &&
            xr_parser_compare_bytes(parser, parser->current.start, "panic", 5) == 0) {
            is_panic = true;
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);  // consume 'panic'
        }

        char *var_name = NULL;
        int var_line = parser->current.line;
        int var_column = parser->current.column;
        XrTypeRef *type_ann = NULL;
        AstNode *pattern = NULL;

        if (!is_panic && !xr_parser_check(parser, TK_LPAREN)) {
            do {
                xr_parser_error(parser, "expected '(' after catch");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }
        if (xr_parser_match(parser, TK_LPAREN)) {
            XrParserStreamState saved = xr_parser_stream_save(parser);
            if (!xr_parser_healthy(parser)) return NULL;
            bool is_binding_header = false;
            if (xr_parser_check(parser, TK_NAME)) {
                do {
                    xr_parser_advance(parser);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                do {
                    is_binding_header =
                    xr_parser_check(parser, TK_COLON) || xr_parser_check(parser, TK_RPAREN);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
            }
            do {
                xr_parser_stream_restore(parser, &saved);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);

            if (is_binding_header) {
                do {
                    var_name = (char *) ast_alloc(parser->compiler_session,
                                              (size_t) parser->current.length + 1);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                do { if (!ast_copy(parser->compiler_session, var_name, parser->current.start, parser->current.length)) return NULL; } while (0);
                do { if (!ast_work(parser->compiler_session, 1)) return NULL; var_name[parser->current.length] = '\0'; } while (0);
                var_line = parser->current.line;
                var_column = parser->current.column;
                do {
                    xr_parser_advance(parser);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                if (xr_parser_match(parser, TK_COLON))
                    do {
                        type_ann = xr_parse_type_annotation(parser);
                        if (!xr_parser_healthy(parser)) return NULL;
                    } while (0);
            } else {
                if (is_panic) {
                    do {
                        xr_parser_error(parser, "catch panic requires a variable binding");
                        if (!xr_parser_healthy(parser)) return NULL;
                    } while (0);
                    return NULL;
                }
                do {
                    pattern = xr_parse_match_pattern(parser);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                if (!pattern) {
                    do {
                        xr_parser_error(parser, "expected catch pattern");
                        if (!xr_parser_healthy(parser)) return NULL;
                    } while (0);
                    return NULL;
                }
                if (!catch_pattern_names_variants(parser, pattern)) {
                    do {
                        xr_parser_error(parser, "catch pattern must name enum variants");
                        if (!xr_parser_healthy(parser)) return NULL;
                    } while (0);
                    return NULL;
                }
                var_line = pattern->line;
                var_column = pattern->column;
                const char *head = catch_pattern_enum_head_name(pattern);
                if (!xr_parser_healthy(parser)) return NULL;
                if (head)
                    do {
                        type_ann = xr_tref_named(parser->compiler_session, head);
                        if (!xr_parser_healthy(parser)) return NULL;
                    } while (0);
            }
            do {
                xr_parser_consume(parser, TK_RPAREN, "expected ')' after catch header");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        }

        // Parse catch body
        do {
            xr_parser_consume(parser, TK_LBRACE, "expected '{' after catch");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        AstNode *body = xr_parse_block(parser);
        if (!xr_parser_healthy(parser)) return NULL;

        XrCatchClause *clause = xr_ast_catch_clause(parser->compiler_session, var_name, var_line,
                                                    var_column, type_ann, body);
        if (!xr_parser_healthy(parser)) return NULL;
        clause->pattern = pattern;
        clause->is_panic = is_panic;
        do {
            XR_PARSE_PUSH(parser, clauses, catch_count, catch_cap, clause);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
    }

    // Need at least one catch clause
    if (catch_count == 0) {
        do {
            xr_parser_error(parser, "try statement requires a catch block");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }

    AstNode *node =
        xr_ast_try_catch(parser->compiler_session, try_body, clauses, catch_count, line);
    if (!xr_parser_healthy(parser)) return NULL;
    // Slice ends at the last block present (last catch > try).
    AstNode *last_block = clauses[catch_count - 1]->body;
    if (!last_block)
        last_block = try_body;
    if (last_block) {
        node->end_line = last_block->end_line;
        node->end_column = last_block->end_column;
    }
    return node;
}

/*
 * Parse throw statement
 * throw expr
 */
AstNode *xr_parse_throw_statement(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_throw_statement: NULL parser");
    int line = parser->current.line;
    int column = parser->current.column;

    // Consume 'throw'
    do {
        xr_parser_advance(parser);
        if (!xr_parser_healthy(parser)) return NULL;
    } while (0);

    // Parse expression to throw
    AstNode *expression = xr_parse_expression(parser);
    if (!xr_parser_healthy(parser)) return NULL;

    if (!expression) {
        do {
            xr_parser_error(parser, "throw statement requires an expression");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }

    return xr_ast_throw_stmt(parser->compiler_session, expression, line, column);
}
