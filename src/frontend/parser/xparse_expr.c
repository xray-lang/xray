/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xparse_expr.c - Prefix and infix expression parsing
 *
 * KEY CONCEPT:
 *   Pratt parser prefix/infix handlers: literals, unary/binary ops,
 *   template strings, type casts, generics, optional chains, etc.
 */

#include "xparse_internal.h"
#include "../../base/xchecks.h"
#include "../../base/xarena.h"
#include "../../runtime/xisolate_api.h"
#include "../analyzer/xanalyzer_symbol.h"
#include "../xdiag_fmt.h"

/* ========== Helpers ========== */

// Strip underscore separators from numeric literal into dst buffer.
// Returns number of characters written (not counting NUL).
static int strip_underscores(const char *src, int src_len, char *dst, int dst_size) {
    int n = 0;
    for (int i = 0; i < src_len && n < dst_size - 1; i++) {
        if (src[i] != '_') dst[n++] = src[i];
    }
    dst[n] = '\0';
    return n;
}

/* ========== Prefix Parsing ========== */

// Parse integer literal (supports multiple bases and underscore separators)
// Formats: decimal (123), hex (0xFF), binary (0b1010), octal (0o755)
static xr_Integer parse_integer_literal(const char *start, int length) {
    char buf[64];
    int buf_len = strip_underscores(start, length, buf, sizeof(buf));
    
    // Detect base
    if (buf_len >= 2 && buf[0] == '0') {
        char prefix = buf[1];
        if (prefix == 'x' || prefix == 'X') {
            return strtoll(buf + 2, NULL, 16);  // Hex
        } else if (prefix == 'b' || prefix == 'B') {
            return strtoll(buf + 2, NULL, 2);   // Binary
        } else if (prefix == 'o' || prefix == 'O') {
            return strtoll(buf + 2, NULL, 8);   // Octal
        }
    }
    
    return strtoll(buf, NULL, 10);  // Decimal
}

// Parse literal (number, string, bool, null)
AstNode *xr_parse_literal(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_literal: NULL parser");
    int column = parser->previous.column;
    switch (parser->previous.type) {
        case TK_LITERAL_INT: {
            xr_Integer value = parse_integer_literal(
                parser->previous.start, 
                parser->previous.length
            );
            // Full int64 range allowed at parse time.
            // The compiler decides per-context: int64 targets use LOADK_RAW,
            // untyped (any) targets with >64-bit values get a compile error.
            AstNode *node = xr_ast_literal_int(parser->X, value, parser->previous.line);
            node->column = column;
            return node;
        }
        
        case TK_LITERAL_FLOAT: {
            char buf[64];
            strip_underscores(parser->previous.start, parser->previous.length, buf, sizeof(buf));
            xr_Number value = strtod(buf, NULL);
            AstNode *node = xr_ast_literal_float(parser->X, value, parser->previous.line);
            node->column = column;
            return node;
        }
        
        case TK_LITERAL_BIGINT: {
            // Strip 'n' suffix and underscores
            int length = parser->previous.length - 1;  // Strip 'n' suffix
            char *buf = (char *)xr_malloc(length + 1);
            strip_underscores(parser->previous.start, length, buf, length + 1);
            AstNode *node = xr_ast_literal_bigint(parser->X, buf, parser->previous.line);
            node->column = column;
            xr_free(buf);
            return node;
        }
        
        case TK_LITERAL_STRING: {
            const char *src = parser->previous.start + 1;
            size_t src_len = parser->previous.length - 2;
            char *str = (char *)xr_malloc(src_len + 1);
            size_t dst_pos = xr_process_escapes(src, src_len, str);
            str[dst_pos] = '\0';
            AstNode *node = xr_ast_literal_string(parser->X, str, parser->previous.line);
            node->column = column;
            xr_free(str);
            return node;
        }
        
        case TK_RAW_STRING: {
            // r"content" or r'content' - no escape processing
            const char *src = parser->previous.start + 2;
            size_t src_len = parser->previous.length - 3;
            char *str = (char *)xr_malloc(src_len + 1);
            memcpy(str, src, src_len);
            str[src_len] = '\0';
            AstNode *node = xr_ast_literal_string(parser->X, str, parser->previous.line);
            node->column = column;
            xr_free(str);
            return node;
        }
        
        case TK_TRUE: {
            AstNode *node = xr_ast_literal_bool(parser->X, 1, parser->previous.line);
            node->column = column;
            return node;
        }
            
        case TK_FALSE: {
            AstNode *node = xr_ast_literal_bool(parser->X, 0, parser->previous.line);
            node->column = column;
            return node;
        }
            
        case TK_NULL: {
            AstNode *node = xr_ast_literal_null(parser->X, parser->previous.line);
            node->column = column;
            return node;
        }
            
        default:
            xr_parser_error(parser, "unknown literal type");
            return NULL;
    }
}

// Regex prefix parsing (when '/' appears at expression start)
// Backtrack scanner and rescan as regex
AstNode *xr_parse_regex_prefix(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_regex_prefix: NULL parser");
    const char *slash_pos = parser->previous.start;
    parser->scanner.current = slash_pos;
    
    Token regex_token = xr_scanner_try_regex(&parser->scanner);
    
    if (regex_token.type == TK_LITERAL_REGEX) {
        parser->previous = regex_token;
        parser->current = xr_scanner_scan(&parser->scanner);
        return xr_parse_regex_literal(parser);
    } else {
        xr_parser_error(parser, "invalid regex literal");
        return NULL;
    }
}

// Parse regex literal: /pattern/flags
AstNode *xr_parse_regex_literal(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_regex_literal: NULL parser");
    const char *start = parser->previous.start;
    int length = parser->previous.length;
    
    // Skip opening '/'
    start++;
    length--;
    
    // Find closing '/'
    const char *end_slash = NULL;
    for (int i = length - 1; i >= 0; i--) {
        if (start[i] == '/') {
            end_slash = start + i;
            break;
        }
    }
    
    if (!end_slash) {
        xr_parser_error(parser, "invalid regex literal format");
        return NULL;
    }
    
    // Extract pattern
    int pattern_len = (int)(end_slash - start);
    char *pattern = (char *)xr_malloc(pattern_len + 1);
    memcpy(pattern, start, pattern_len);
    pattern[pattern_len] = '\0';
    
    // Extract flags
    const char *flags_start = end_slash + 1;
    int flags_len = length - pattern_len - 1;
    char *flags = (char *)xr_malloc(flags_len + 1);
    if (flags_len > 0) {
        memcpy(flags, flags_start, flags_len);
    }
    flags[flags_len] = '\0';
    
    // Create AST node
    AstNode *node = xr_ast_literal_regex(parser->X, pattern, flags, parser->previous.line);
    
    xr_free(pattern);
    xr_free(flags);
    
    return node;
}

// Parse type cast: int(x), float(x), string(x), bool(x)
AstNode *xr_parse_type_cast(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_type_cast: NULL parser");
    const char *type_name = NULL;
    switch (parser->previous.type) {
        case TK_INT:    type_name = "int";    break;
        case TK_FLOAT:  type_name = "float";  break;
        case TK_STRING: type_name = "string"; break;
        case TK_BOOL:   type_name = "bool";   break;
        default:
            xr_parser_error(parser, "expected type keyword");
            return NULL;
    }
    
    int line = parser->previous.line;
    
    if (!xr_parser_match(parser, TK_LPAREN)) {
        xr_parser_error(parser, "expected '(' after type cast");
        return NULL;
    }
    AstNode *arg = xr_parse_expression(parser);
    if (!arg) {
        return NULL;
    }
    
    xr_parser_consume(parser, TK_RPAREN, "expected ')' after type cast argument");
    
    AstNode *callee = xr_ast_variable(parser->X, type_name, line);
    AstNode **arguments = (AstNode **)xr_malloc(sizeof(AstNode *));
    arguments[0] = arg;
    
    return xr_ast_call_expr(parser->X, callee, arguments, 1, line);
}

// Parse container constructor: Array(...), Map(), Set(...)
// Parse type keyword as variable reference (e.g. Json.keys(obj))
AstNode *xr_parse_type_keyword_as_variable(Parser *parser) {
    const char *name = NULL;
    switch (parser->previous.type) {
        case TK_TYPE_JSON:      name = "Json"; break;
        case TK_TYPE_DATETIME:  name = "DateTime"; break;
        case TK_TYPE_BYTES:     name = "Bytes"; break;
        case TK_TYPE_BIGINT:    name = "BigInt"; break;
        default:
            xr_parser_error(parser, "unexpected type keyword in expression");
            return NULL;
    }
    return xr_ast_variable(parser->X, name, parser->previous.line);
}

AstNode *xr_parse_container_constructor(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_container_constructor: NULL parser");
    const char *type_name = NULL;
    switch (parser->previous.type) {
        case TK_TYPE_ARRAY: type_name = "Array"; break;
        case TK_TYPE_MAP:   type_name = "Map";   break;
        case TK_TYPE_SET:   type_name = "Set";   break;
        default:
            xr_parser_error(parser, "expected container type keyword");
            return NULL;
    }
    
    int line = parser->previous.line;
    
    if (!xr_parser_match(parser, TK_LPAREN)) {
        xr_parser_error(parser, "expected '(' after constructor");
        return NULL;
    }
    AstNode **arguments = NULL;
    int arg_count = 0;
    int arg_capacity = 0;
    
    if (!xr_parser_check(parser, TK_RPAREN)) {
        do {
            XR_PARSE_PUSH(arguments, arg_count, arg_capacity, xr_parse_expression(parser));
        } while (xr_parser_match(parser, TK_COMMA));
    }
    
    xr_parser_consume(parser, TK_RPAREN, "expected ')' after constructor arguments");
    
    AstNode *callee = xr_ast_variable(parser->X, type_name, line);
    
    return xr_ast_call_expr(parser->X, callee, arguments, arg_count, line);
}

// Helper: create string literal node from a template string part.
// For normal template strings, applies escape processing.
// For raw template strings, copies verbatim.
static AstNode *make_template_part(Parser *parser, const char *src, int len, bool is_raw) {
    char *buf = (char*)xr_malloc(len + 1);
    size_t out_len;
    if (is_raw) {
        memcpy(buf, src, len);
        out_len = len;
    } else {
        out_len = xr_process_escapes(src, len, buf);
    }
    buf[out_len] = '\0';
    AstNode *node = xr_ast_literal_string(parser->X, buf, parser->previous.line);
    xr_free(buf);
    return node;
}

// Parse template string: "Hello, ${name}!" or r"raw ${name}"
AstNode *xr_parse_template_string(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_template_string: NULL parser");
    bool is_raw = (parser->previous.type == TK_RAW_TEMPLATE_STRING);
    int skip = is_raw ? 2 : 1;  // r" vs "
    const char *tmpl = parser->previous.start + skip;
    int tmpl_len = parser->previous.length - skip - 1;
    AstNode **parts = NULL;
    int part_count = 0;
    int part_capacity = 4;
    
    parts = (AstNode**)xr_malloc(sizeof(AstNode*) * part_capacity);
    if (!parts) {
        xr_parser_error(parser, "memory allocation failed");
        return NULL;
    }
    
    int i = 0;
    while (i < tmpl_len) {
        // Find next ${ (for normal mode, \$ escapes the dollar sign)
        int expr_start = -1;
        for (int j = i; j < tmpl_len - 1; j++) {
            if (!is_raw && tmpl[j] == '\\' && j + 1 < tmpl_len) {
                j++;
                continue;
            }
            if (tmpl[j] == '$' && tmpl[j + 1] == '{') {
                expr_start = j;
                break;
            }
        }
        
        if (expr_start == -1) {
            // No more interpolations, rest is string
            if (i < tmpl_len) {
                AstNode *str_node = make_template_part(parser, tmpl + i, tmpl_len - i, is_raw);
                XR_PARSE_PUSH(parts, part_count, part_capacity, str_node);
            }
            break;
        }
        
        // Add string part before ${
        if (expr_start > i) {
            AstNode *str_node = make_template_part(parser, tmpl + i, expr_start - i, is_raw);
            XR_PARSE_PUSH(parts, part_count, part_capacity, str_node);
        }
        
        // Find matching }
        int expr_end = -1;
        int brace_count = 1;
        int j = expr_start + 2;  // Skip ${
        
        while (j < tmpl_len && brace_count > 0) {
            if (tmpl[j] == '{') {
                brace_count++;
            } else if (tmpl[j] == '}') {
                brace_count--;
                if (brace_count == 0) {
                    expr_end = j;
                    break;
                }
            }
            j++;
        }
        
        if (expr_end == -1) {
            xr_free(parts);
            xr_parser_error(parser, "missing closing } in template string");
            return NULL;
        }
        
        // Parse interpolation expression
        int expr_len = expr_end - (expr_start + 2);
        if (expr_len > 0) {
            char *expr_code = (char*)xr_malloc(expr_len + 1);
            memcpy(expr_code, tmpl + expr_start + 2, expr_len);
            expr_code[expr_len] = '\0';
            
            Scanner expr_scanner;
            xr_scanner_init(&expr_scanner, expr_code);
            
            Parser expr_parser;
            expr_parser.scanner = expr_scanner;
            expr_parser.X = parser->X;
            expr_parser.had_error = 0;
            expr_parser.panic_mode = 0;
            
            xr_parser_advance(&expr_parser);
            AstNode *expr_node = xr_parse_expression(&expr_parser);
            
            xr_free(expr_code);
            
            if (expr_node) {
                XR_PARSE_PUSH(parts, part_count, part_capacity, expr_node);
            }
        }
        
        i = expr_end + 1;  // Skip }
    }
    
    if (part_count == 0) {
        xr_free(parts);
        return xr_ast_literal_string(parser->X, "", parser->previous.line);
    }
    
    AstNode *node = xr_ast_template_string(parser->X, parts, part_count, parser->previous.line);
    xr_free(parts);
    
    return node;
}

// Parse grouping expression: (expression)
AstNode *xr_parse_grouping(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_grouping: NULL parser");
    int line = parser->previous.line;
    
    // Case 1: () => expr or (): type => expr (no params arrow function)
    if (xr_parser_check(parser, TK_RPAREN)) {
        xr_parser_advance(parser);
        // Optional return type annotation: (): int =>
        XrType *return_type = NULL;
        if (xr_parser_check(parser, TK_COLON)) {
            xr_parser_advance(parser);
            return_type = xr_parse_type_annotation(parser);
        }
        if (xr_parser_match(parser, TK_ARROW)) {
            AstNode *fn = xr_parse_arrow_function_body(parser, NULL, 0, line);
            if (fn && return_type) fn->as.function_expr.return_type = return_type;
            return fn;
        }
        if (return_type) {
            xr_parser_error(parser, "expected '=>' after return type annotation");
            return NULL;
        }
        xr_parser_error(parser, "empty grouping expression");
        return NULL;
    }
    
    // Case 2: (id) => expr or (id: type, ...) => expr (param list with optional types)
    if (xr_parser_check(parser, TK_NAME)) {
        Token first_name = parser->current;
        xr_parser_advance(parser);
        
        if (xr_parser_check(parser, TK_COMMA) || xr_parser_check(parser, TK_RPAREN) ||
            xr_parser_check(parser, TK_COLON)) {
            // May be arrow function, collect params as XrParamNode
            XrParamNode **params = (XrParamNode**)xr_malloc(sizeof(XrParamNode*) * 10);
            int param_count = 0;
            bool has_type_annotation = false;
            
            // First param
            char name_buf[256];
            snprintf(name_buf, sizeof(name_buf), "%.*s", first_name.length, first_name.start);
            params[param_count] = xr_param_node_new(parser->X, name_buf, first_name.line, first_name.column);
            if (xr_parser_match(parser, TK_COLON)) {
                params[param_count]->type = xr_parse_type_annotation(parser);
                has_type_annotation = true;
            }
            param_count++;
            
            // Parse remaining params (with optional type annotations)
            while (xr_parser_match(parser, TK_COMMA)) {
                xr_parser_consume(parser, TK_NAME, "expected parameter name");
                Token param = parser->previous;
                snprintf(name_buf, sizeof(name_buf), "%.*s", param.length, param.start);
                params[param_count] = xr_param_node_new(parser->X, name_buf, param.line, param.column);
                if (xr_parser_match(parser, TK_COLON)) {
                    params[param_count]->type = xr_parse_type_annotation(parser);
                    has_type_annotation = true;
                }
                param_count++;
            }
            
            if (!xr_parser_match(parser, TK_RPAREN)) {
                for (int i = 0; i < param_count; i++) xr_param_node_free(parser->X, params[i]);
                xr_free(params);
                xr_parser_error(parser, "expected ')' or '=>'");
                return NULL;
            }
            
            // Optional return type annotation: (n: int): int =>
            XrType *return_type = NULL;
            if (xr_parser_check(parser, TK_COLON)) {
                xr_parser_advance(parser);
                return_type = xr_parse_type_annotation(parser);
            }
            
            if (xr_parser_match(parser, TK_ARROW)) {
                AstNode *fn = xr_parse_arrow_function_body(parser, params, param_count, line);
                if (fn && return_type) fn->as.function_expr.return_type = return_type;
                return fn;
            }
            
            // Had type annotations but no => : syntax error
            if (has_type_annotation || return_type) {
                for (int i = 0; i < param_count; i++) xr_param_node_free(parser->X, params[i]);
                xr_free(params);
                xr_parser_error(parser, "expected '=>' after typed parameter list");
                return NULL;
            }
            
            for (int i = 0; i < param_count; i++) xr_param_node_free(parser->X, params[i]);
            xr_free(params);
            
            // Single (name) without => is a regular grouping expression
            if (param_count == 1) {
                char *var_name = (char*)xr_malloc(first_name.length + 1);
                memcpy(var_name, first_name.start, first_name.length);
                var_name[first_name.length] = '\0';
                AstNode *expr = xr_ast_variable(parser->X, var_name, line);
                xr_free(var_name);
                return xr_ast_grouping(parser->X, expr, line);
            }
            
            xr_parser_error(parser, "tuple expressions not supported");
            return NULL;
        }
        
        // Not arrow function params, regular expression
        char *var_name = (char*)xr_malloc(first_name.length + 1);
        strncpy(var_name, first_name.start, first_name.length);
        var_name[first_name.length] = '\0';
        AstNode *expr = xr_ast_variable(parser->X, var_name, line);
        xr_free(var_name);
        
        // Continue parsing possible infix operators
        while (xr_get_rule(parser->current.type)->precedence >= PREC_ASSIGNMENT) {
            xr_parser_advance(parser);
            InfixParseFn infix_rule = xr_get_rule(parser->previous.type)->infix;
            if (infix_rule) {
                expr = infix_rule(parser, expr);
            } else {
                break;
            }
        }
        
        xr_parser_consume(parser, TK_RPAREN, "expected ')' to close grouping");
        return xr_ast_grouping(parser->X, expr, line);
    }
    
    // Regular grouping expression
    AstNode *expr = xr_parse_expression(parser);
    xr_parser_consume(parser, TK_RPAREN, "expected ')' to close grouping");
    return xr_ast_grouping(parser->X, expr, parser->previous.line);
}

// Parse arrow function body
// Supports: => expr (auto return) or => { ... } (block)
AstNode *xr_parse_arrow_function_body(Parser *parser, XrParamNode **params, int param_count, int line) {
    AstNode *body;
    
    if (xr_parser_match(parser, TK_LBRACE)) {
        // Block body: => { ... }
        body = xr_parse_block(parser);
    } else {
        // Expression body: => expr (auto-wrap in return)
        AstNode *expr = xr_parse_expression(parser);
        
        AstNode **values = (AstNode**)xr_malloc(sizeof(AstNode*));
        values[0] = expr;
        AstNode *return_stmt = xr_ast_return_stmt(parser->X, values, 1, expr->line);
        
        body = xr_ast_block(parser->X, line);
        xr_ast_block_add(parser->X, body, return_stmt);
    }
    
    // params ownership transferred to func_expr
    return xr_ast_function_expr(parser->X, params, param_count, body, line);
}

// Parse fn anonymous function expression
// Syntax: fn() { ... } or fn(a, b) { return a + b }
AstNode *xr_parse_fn_expression(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_fn_expression: NULL parser");
    int line = parser->previous.line;
    
    xr_parser_consume(parser, TK_LPAREN, "expected '(' after fn");
    XrParamNode **params = NULL;
    int param_count = 0;
    int param_capacity = 0;
    
    if (!xr_parser_check(parser, TK_RPAREN)) {
        do {
            xr_parser_consume(parser, TK_NAME, "expected parameter name");
            Token param_token = parser->previous;
            
            char param_name[256];
            snprintf(param_name, sizeof(param_name), "%.*s", param_token.length, param_token.start);
            
            XrParamNode *param = xr_param_node_new(parser->X, param_name, param_token.line, param_token.column);
            
            // Parse optional type annotation
            if (xr_parser_match(parser, TK_COLON)) {
                param->type = xr_parse_type_annotation(parser);
            }
            
            XR_PARSE_PUSH(params, param_count, param_capacity, param);
        } while (xr_parser_match(parser, TK_COMMA));
    }
    
    xr_parser_consume(parser, TK_RPAREN, "expected ')' after parameter list");
    
    // Parse optional return type annotation
    XrType *return_type = NULL;
    if (xr_parser_match(parser, TK_COLON)) {
        return_type = xr_parse_type_annotation(parser);
    } else if (xr_parser_check(parser, TK_MINUS)) {
        // Detect common mistake: fn() -> Type (should be fn(): Type)
        xr_parser_advance(parser);  // consume '-'
        if (xr_parser_match(parser, TK_GT)) {
            xr_parser_error(parser,
                "use ':' instead of '->' for return type annotation, "
                "e.g. fn(): int");
            parser->panic_mode = 0;
            return_type = xr_parse_type_annotation(parser);
        }
    }
    
    // Parse function body (must be block)
    xr_parser_consume(parser, TK_LBRACE, "fn function body must use braces { }");
    AstNode *body = xr_parse_block(parser);
    
    AstNode *func_expr = xr_ast_function_expr(parser->X, params, param_count, body, line);
    func_expr->as.function_expr.return_type = return_type;
    
    return func_expr;
}

// Parse unary operators: -expr, !expr, ~expr
AstNode *xr_parse_unary(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_unary: NULL parser");
    TokenType operator_type = parser->previous.type;
    int line = parser->previous.line;
    
    AstNode *operand = xr_parse_precedence(parser, PREC_UNARY);
    switch (operator_type) {
        case TK_MINUS:
            return xr_ast_unary(parser->X, AST_UNARY_NEG, operand, line);
        case TK_NOT:
            return xr_ast_unary(parser->X, AST_UNARY_NOT, operand, line);
        case TK_TILDE:
            return xr_ast_unary(parser->X, AST_UNARY_BNOT, operand, line);
        default:
            xr_parser_error(parser, "unknown unary operator");
            return NULL;
    }
}

/* ========== Infix Parsing ========== */

// Try to parse generic call: callee<Type, ...>(args)
// Returns NULL if not a generic call (should fallback to comparison)
static AstNode *try_parse_generic_call(Parser *parser, AstNode *callee) {
    // Only try if callee is an identifier or member access
    if (callee->type != AST_VARIABLE && callee->type != AST_MEMBER_ACCESS) {
        return NULL;
    }
    
    int line = parser->previous.line;
    Parser checkpoint = *parser;
    int saved_panic_mode = parser->panic_mode;
    
    // Suppress error output during speculative parsing
    parser->panic_mode = 1;
    
    // Try to parse type arguments
    XrType *type_args[16];
    int type_arg_count = 0;
    
    // Already consumed '<', now parse type list
    do {
        if (type_arg_count >= 16) break;
        
        XrType *type = xr_parse_type_annotation(parser);
        if (!type || parser->had_error) {
            // Not valid type args, restore and return NULL
            *parser = checkpoint;
            parser->panic_mode = saved_panic_mode;
            return NULL;
        }
        type_args[type_arg_count++] = type;
        
    } while (xr_parser_match(parser, TK_COMMA));
    
    // Must have '>' followed by '('
    if (!xr_parser_match(parser, TK_GT)) {
        // Handle '>>' case
        if (parser->current.type == TK_RSHIFT) {
            parser->current.type = TK_GT;
            parser->current.start++;
            parser->current.length = 1;
        } else {
            *parser = checkpoint;
            parser->panic_mode = saved_panic_mode;
            return NULL;
        }
    }
    
    // Must be followed by '(' for function call
    if (!xr_parser_check(parser, TK_LPAREN)) {
        *parser = checkpoint;
        parser->panic_mode = saved_panic_mode;
        return NULL;
    }
    
    // Restore panic_mode now that we confirmed it's a valid generic call
    parser->panic_mode = saved_panic_mode;
    
    // Parse the function call
    xr_parser_advance(parser);  // consume '('
    
    AstNode **arguments = NULL;
    int arg_count = 0;
    int arg_capacity = 0;
    
    if (!xr_parser_check(parser, TK_RPAREN)) {
        do {
            XR_PARSE_PUSH(arguments, arg_count, arg_capacity, xr_parse_expression(parser));
        } while (xr_parser_match(parser, TK_COMMA));
    }
    
    xr_parser_consume(parser, TK_RPAREN, "expected ')' after argument list");
    
    return xr_ast_call_expr_generic(parser->X, callee, arguments, arg_count,
                                     type_args, type_arg_count, line);
}

// Parse '<' which could be comparison or generic call
// Uses space sensitivity: foo<T>() is generic, foo < T is comparison
AstNode *xr_parse_lt_or_generic(Parser *parser, AstNode *left) {
    // If '<' has leading space, treat as comparison
    // e.g., "a < b" is comparison, "a<b>" could be generic
    if (parser->previous.has_leading_space) {
        // Fall back to comparison
        int line = parser->previous.line;
        const ParseRule *rule = xr_get_rule(TK_LT);
        AstNode *right = xr_parse_precedence(parser, rule->precedence + 1);
        return xr_ast_binary(parser->X, AST_BINARY_LT, left, right, line);
    }
    
    // Try generic call first (no space before '<')
    AstNode *generic_call = try_parse_generic_call(parser, left);
    if (generic_call) {
        return generic_call;
    }
    
    // Fall back to comparison
    int line = parser->previous.line;
    const ParseRule *rule = xr_get_rule(TK_LT);
    AstNode *right = xr_parse_precedence(parser, rule->precedence + 1);
    return xr_ast_binary(parser->X, AST_BINARY_LT, left, right, line);
}

// TokenType -> AstNodeType mapping for binary operators
static const AstNodeType binary_op_map[] = {
    [TK_PLUS]      = AST_BINARY_ADD,    [TK_MINUS]     = AST_BINARY_SUB,
    [TK_STAR]      = AST_BINARY_MUL,    [TK_SLASH]     = AST_BINARY_DIV,
    [TK_PERCENT]   = AST_BINARY_MOD,    [TK_AMP]       = AST_BINARY_BAND,
    [TK_PIPE]      = AST_BINARY_BOR,    [TK_CARET]     = AST_BINARY_BXOR,
    [TK_LSHIFT]    = AST_BINARY_LSHIFT, [TK_RSHIFT]    = AST_BINARY_RSHIFT,
    [TK_EQ]        = AST_BINARY_EQ,     [TK_NE]        = AST_BINARY_NE,
    [TK_EQ_STRICT] = AST_BINARY_EQ_STRICT, [TK_NE_STRICT] = AST_BINARY_NE_STRICT,
    [TK_LT]        = AST_BINARY_LT,     [TK_LE]        = AST_BINARY_LE,
    [TK_GT]        = AST_BINARY_GT,      [TK_GE]        = AST_BINARY_GE,
    [TK_AND]       = AST_BINARY_AND,     [TK_OR]        = AST_BINARY_OR,
};

// Parse binary operators: left op right
AstNode *xr_parse_binary(Parser *parser, AstNode *left) {
    XR_DCHECK(parser != NULL, "parse_binary: NULL parser");
    TokenType operator_type = parser->previous.type;
    int line = parser->previous.line;
    
    const ParseRule *rule = xr_get_rule(operator_type);
    
    // Parse right operand (left-associative: precedence + 1)
    AstNode *right = xr_parse_precedence(parser, rule->precedence + 1);

    AstNodeType ast_type = 0;
    if (operator_type >= 0
        && operator_type < (TokenType)(sizeof(binary_op_map) / sizeof(binary_op_map[0]))) {
        ast_type = binary_op_map[operator_type];
    }
    if (ast_type == 0) {
        xr_parser_error(parser, "unknown binary operator");
        return NULL;
    }
    
    return xr_ast_binary(parser->X, ast_type, left, right, line);
}

// Parse 'is' expression: expr is Type
// Bare container types allowed: 'x is Array' checks runtime type without element type.
AstNode *xr_parse_is(Parser *parser, AstNode *left) {
    int line = parser->previous.line;
    
    // Allow bare container types for runtime type checks
    bool saved = parser->allow_bare_container;
    parser->allow_bare_container = true;
    XrType *type = xr_parse_type_annotation(parser);
    parser->allow_bare_container = saved;
    if (!type) {
        xr_parser_error(parser, "expected type after 'is'");
        return NULL;
    }
    
    return xr_ast_is_expr(parser->X, left, type, line);
}

// Parse ternary expression: condition ? trueValue : falseValue
AstNode *xr_parse_ternary(Parser *parser, AstNode *condition) {
    XR_DCHECK(parser != NULL, "parse_ternary: NULL parser");
    int line = parser->previous.line;
    
    AstNode *true_expr = xr_parse_precedence(parser, PREC_TERNARY + 1);
    
    xr_parser_consume(parser, TK_COLON, "expected ':' in ternary expression");
    
    AstNode *false_expr = xr_parse_precedence(parser, PREC_TERNARY);
    
    return xr_ast_ternary(parser->X, condition, true_expr, false_expr, line);
}

// Parse nullish coalescing: value ?? defaultValue
AstNode *xr_parse_nullish_coalesce(Parser *parser, AstNode *left) {
    XR_DCHECK(parser != NULL, "parse_nullish_coalesce: NULL parser");
    int line = parser->previous.line;
    
    AstNode *right = xr_parse_precedence(parser, PREC_NULLISH_COALESCE + 1);
    
    return xr_ast_binary(parser->X, AST_NULLISH_COALESCE, left, right, line);
}

// Parse force unwrap: expr! (panics at runtime if value is null)
AstNode *xr_parse_force_unwrap(Parser *parser, AstNode *operand) {
    XR_DCHECK(parser != NULL, "parse_force_unwrap: NULL parser");
    int line = parser->previous.line;
    return xr_ast_unary(parser->X, AST_FORCE_UNWRAP, operand, line);
}

// Parse as cast: expr as Type / expr as Type?
// Bare container types allowed: 'x as Array' for runtime type casts.
AstNode *xr_parse_as_cast(Parser *parser, AstNode *left) {
    XR_DCHECK(parser != NULL, "parse_as_cast: NULL parser");
    int line = parser->previous.line;
    // Allow bare container types for runtime type casts
    bool saved = parser->allow_bare_container;
    parser->allow_bare_container = true;
    XrType *target_type = xr_parse_type_annotation(parser);
    parser->allow_bare_container = saved;
    if (!target_type) {
        xr_parser_error(parser, "expected type after 'as'");
        return left;
    }
    // Check for safe cast: as Type?
    bool is_safe = target_type->is_nullable;
    return xr_ast_as_expr(parser->X, left, target_type, is_safe, line);
}

// Parse optional chain: obj?.prop, obj?.[index], obj?.method()
AstNode *xr_parse_optional_chain(Parser *parser, AstNode *object) {
    XR_DCHECK(parser != NULL, "parse_optional_chain: NULL parser");
    int line = parser->previous.line;
    
    if (parser->current.type == TK_NAME) {
        // Property access: obj?.prop
        xr_parser_advance(parser);
        const char *name = parser->previous.start;
        int name_len = parser->previous.length;
        char *name_str = (char *)xr_malloc(name_len + 1);
        if (name_str == NULL) {
            xr_parser_error(parser, "memory allocation failed");
            return NULL;
        }
        memcpy(name_str, name, name_len);
        name_str[name_len] = '\0';
        
        // Check for method call
        if (parser->current.type == TK_LPAREN) {
            return xr_ast_optional_chain(parser->X, object, name_str, NULL, 2, line);
        }
        
        return xr_ast_optional_chain(parser->X, object, name_str, NULL, 0, line);
    } else if (parser->current.type == TK_LBRACKET) {
        // Index access: obj?.[index]
        xr_parser_advance(parser);
        AstNode *index = xr_parse_expression(parser);
        xr_parser_consume(parser, TK_RBRACKET, "expected ']' in optional chain index");
        return xr_ast_optional_chain(parser->X, object, NULL, index, 1, line);
    } else {
        xr_parser_error(parser, "expected property name or index after '?.'");
        return NULL;
    }
}

// Parse range expression: start..end
AstNode *xr_parse_range(Parser *parser, AstNode *start) {
    XR_DCHECK(parser != NULL, "parse_range: NULL parser");
    int line = parser->previous.line;
    
    AstNode *end = xr_parse_precedence(parser, PREC_FACTOR + 1);
    
    return xr_ast_range(parser->X, start, end, line);
}

