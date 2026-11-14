/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xparse_arrow.c - Delimiter roles retained across rejected declarations
 *
 * KEY CONCEPT:
 *   Syntax tooling observes signatures and arm heads without accepting them.
 *   The ordinary parser remains the authority for successful syntax.
 */
#include "xparse_internal.h"

typedef struct XrArrowScope {
    bool declaration;
    int paren_depth;
} XrArrowScope;

static bool arrow_name(const Token *token, const char *name) {
    size_t length = strlen(name);
    return token->length == (int)length && memcmp(token->start, name, length) == 0;
}

static int arrow_signature_head(const Token *tokens, int index) {
    int head = index - 1;
    if (head < 0) return -1;
    if (tokens[head].type == TK_GT || tokens[head].type == TK_RSHIFT) {
        int depth = 0;
        for (; head >= 0; --head) {
            XrTokenType type = tokens[head].type;
            if (type == TK_GT) ++depth;
            else if (type == TK_RSHIFT) depth += 2;
            else if (type == TK_LT && --depth == 0) return head - 1;
        }
    }
    return head;
}

static bool arrow_signature(const Token *tokens, int index, bool member) {
    int head = arrow_signature_head(tokens, index);
    if (head < 0) return false;
    const Token *prefix = &tokens[head];
    if (prefix->type == TK_FN || arrow_name(prefix, "throws")) return true;
    if (head > 0 && tokens[head - 1].type == TK_FN) return true;
    if (!member) return false;
    unsigned char first = (unsigned char)prefix->start[0];
    if ((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || first == '_')
        return true;
    if (head > 0 && arrow_name(&tokens[head - 1], "operator")) return true;
    return head > 1 && prefix->type == TK_RBRACKET &&
        tokens[head - 1].type == TK_LBRACKET && arrow_name(&tokens[head - 2], "operator");
}

/* Collecting first lets generic heads be recognized without mutating the
 * parser stream or depending on recovery's abandoned AST nodes. */
XrParseStatus xr_parser_scan_arrow_syntax(Parser *parser, XrParseArrowSyntaxFn callback, void *data) {
    if (!parser || !callback || !xr_parser_healthy(parser)) return XR_PARSE_BAD_ARGUMENT;
    Scanner scanner = {0};
    if (xr_compile_scanner_open_source(&scanner, parser->state, parser->arena,
        parser->scanner.source, false, xr_compile_session_request(parser->compiler_session)) !=
        XR_COMPILE_RESOURCE_OK) return xr_compile_parser_status(parser);
    Token *tokens = NULL;
    int count = 0, capacity = 0;
    for (;;) {
        Token token = xr_scanner_scan(&scanner);
        if (!xr_parser_healthy(parser)) return xr_compile_parser_status(parser);
        if (token.type == TK_EOF) break;
        XR_PARSE_PUSH(parser, tokens, count, capacity, token);
        if (!xr_parser_healthy(parser)) return xr_compile_parser_status(parser);
    }
    bool *parens = ast_alloc_array(parser->compiler_session, sizeof(*parens), (size_t)count + 1);
    XrArrowScope *scopes = ast_alloc_array(parser->compiler_session, sizeof(*scopes), (size_t)count + 1);
    if (!xr_parser_healthy(parser)) return xr_compile_parser_status(parser);
    int paren_count = 0, scope_count = 0;
    bool pending_declaration = false, closed_signature = false, closed_paren = false;
    for (int i = 0; i < count; ++i) {
        XrTokenType type = tokens[i].type;
        if (type == TK_CLASS || type == TK_STRUCT || type == TK_INTERFACE ||
            type == TK_ENUM || type == TK_UNION) pending_declaration = true;
        else if (type == TK_MATCH || type == TK_SELECT) pending_declaration = false;
        if (type == TK_LBRACE || type == TK_EMPTY_MAP_START) {
            scopes[scope_count++] = (XrArrowScope){type == TK_LBRACE && pending_declaration, paren_count};
            pending_declaration = false;
        } else if (type == TK_RBRACE) {
            if (scope_count) --scope_count;
            pending_declaration = false;
        } else if (type == TK_LPAREN) {
            bool member = scope_count && scopes[scope_count - 1].declaration &&
                paren_count == scopes[scope_count - 1].paren_depth;
            parens[paren_count++] = arrow_signature(tokens, i, member);
        } else if (type == TK_RPAREN) {
            closed_paren = paren_count > 0;
            closed_signature = closed_paren && parens[--paren_count];
        } else if (type == TK_ARROW || type == TK_FAT_ARROW) {
            bool signature = i > 0 && arrow_name(&tokens[i - 1], "throws");
            if (i > 0 && tokens[i - 1].type == TK_RPAREN) {
                if (!closed_paren) continue;
                signature = closed_signature;
            }
            callback(data, &tokens[i], !signature);
        }
    }
    return XR_PARSE_OK;
}
