/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlex.c - Lexical analyzer implementation
 *
 * KEY CONCEPT:
 *   Tokenizes source code for the parser using SIMD optimization for whitespace.
 */

#include <assert.h>
#include <string.h>
#include "xlex.h"
#include "../../base/xchecks.h"
#include "../../base/xsimd.h"
#include "../../base/xmalloc.h"

// ============================================================================
// Trivia Functions
// ============================================================================

XrTrivia *xr_trivia_new(XrTriviaType type, const char *start, int length, int line) {
    XrTrivia *trivia = (XrTrivia*)xr_malloc(sizeof(XrTrivia));
    if (!trivia) return NULL;
    trivia->type = type;
    trivia->start = start;
    trivia->length = length;
    trivia->line = line;
    trivia->next = NULL;
    return trivia;
}

void xr_trivia_free(XrTrivia *trivia) {
    xr_free(trivia);
}

void xr_trivia_free_chain(XrTrivia *head) {
    while (head) {
        XrTrivia *next = head->next;
        xr_free(head);
        head = next;
    }
}

// Append trivia using tail pointer for O(1)
static void trivia_append(Scanner *scanner, XrTrivia *item) {
    XR_DCHECK(item != NULL, "trivia_append: NULL item");
    if (!scanner->pending_trivia) {
        scanner->pending_trivia = item;
    } else {
        scanner->trivia_tail->next = item;
    }
    scanner->trivia_tail = item;
}

// ============================================================================
// Scanner Initialization
// ============================================================================

void xr_scanner_init(Scanner *scanner, const char *source) {
    XR_DCHECK(scanner != NULL, "scanner_init: NULL scanner");
    XR_DCHECK(source != NULL, "scanner_init: NULL source");
    xr_scanner_init_with_trivia(scanner, source, false);
}

void xr_scanner_init_with_trivia(Scanner *scanner, const char *source, bool collect_trivia) {
    XR_DCHECK(scanner != NULL, "scanner_init_with_trivia: NULL scanner");
    XR_DCHECK(source != NULL, "scanner_init_with_trivia: NULL source");
    scanner->source = source;
    scanner->start = source;
    scanner->current = source;
    scanner->end = source + strlen(source);
    scanner->line = 1;
    scanner->line_start = source;
    scanner->start_line = 1;
    scanner->start_line_start = source;
    scanner->had_leading_space = false;
    scanner->collect_trivia = collect_trivia;
    scanner->pending_trivia = NULL;
    scanner->trivia_tail = NULL;
    scanner->pending_error = NULL;
}

// Capture the start position of a new token. Must be called once per token,
// AFTER skip_whitespace() has run and BEFORE any character is consumed,
// so that `start_line` / `start_line_start` reflect where the token begins
// even if the token later spans multiple lines (e.g. multi-line strings,
// raw strings, template strings, or block comments).
static inline void scanner_begin_token(Scanner *scanner) {
    scanner->start = scanner->current;
    scanner->start_line = scanner->line;
    scanner->start_line_start = scanner->line_start;
}

static int is_at_end(Scanner *scanner) {
    return scanner->current >= scanner->end;
}

static char advance(Scanner *scanner) {
    scanner->current++;
    return scanner->current[-1];
}

static char peek(Scanner *scanner) {
    return *scanner->current;
}

static char peek_next(Scanner *scanner) {
    if (scanner->current + 1 >= scanner->end) return '\0';
    return scanner->current[1];
}

static int match(Scanner *scanner, char expected) {
    if (is_at_end(scanner)) return 0;
    if (*scanner->current != expected) return 0;
    scanner->current++;
    return 1;
}

// L-06: After producing a token, scan ahead for an inline same-line
// comment and return it as that token's trailing trivia. The caller
// MUST attach the returned chain (or NULL) to token.trailing_trivia.
//
// Rules:
//   - Only horizontal whitespace (' ', '\t', '\r') is consumed.
//   - A `//` line comment that begins on the SAME line as the token
//     is captured.
//   - A `/* ... */` block comment is captured ONLY if it terminates
//     on the same source line as the token. Multi-line block
//     comments are deliberately left for skip_whitespace() to absorb
//     into the next token's leading trivia.
//   - If no inline comment is found we rewind any horizontal-
//     whitespace skip so that the next call to skip_whitespace() can
//     correctly compute `had_leading_space` for the following token
//     (the parser's smart-semicolon / generic-vs-comparison
//     disambiguation depends on it).
static XrTrivia *scan_inline_trailing_trivia(Scanner *scanner) {
    if (!scanner->collect_trivia) return NULL;

    const char *save_current = scanner->current;

    while (scanner->current < scanner->end) {
        char c = *scanner->current;
        if (c == ' ' || c == '\t' || c == '\r') {
            scanner->current++;
        } else {
            break;
        }
    }

    if (scanner->current >= scanner->end) {
        scanner->current = save_current;
        return NULL;
    }

    char c = *scanner->current;
    char n = (scanner->current + 1 < scanner->end) ? scanner->current[1] : '\0';

    if (c == '/' && n == '/') {
        int comment_line = scanner->line;
        scanner->current += 2;  // skip //
        const char *cs = scanner->current;
        while (scanner->current < scanner->end && *scanner->current != '\n') {
            scanner->current++;
        }
        int len = (int)(scanner->current - cs);
        return xr_trivia_new(TRIVIA_LINE_COMMENT, cs, len, comment_line);
    }

    if (c == '/' && n == '*') {
        // Look ahead: only attach if `*/` appears before the next '\n'.
        const char *p = scanner->current + 2;
        bool same_line_terminator = false;
        while (p + 1 < scanner->end) {
            if (*p == '\n') break;
            if (p[0] == '*' && p[1] == '/') {
                same_line_terminator = true;
                break;
            }
            p++;
        }
        if (!same_line_terminator) {
            scanner->current = save_current;
            return NULL;
        }
        int comment_line = scanner->line;
        scanner->current += 2;  // /*
        const char *cs = scanner->current;
        while (scanner->current + 1 < scanner->end &&
               !(scanner->current[0] == '*' && scanner->current[1] == '/')) {
            scanner->current++;
        }
        int len = (int)(scanner->current - cs);
        scanner->current += 2;  // */
        return xr_trivia_new(TRIVIA_BLOCK_COMMENT, cs, len, comment_line);
    }

    scanner->current = save_current;
    return NULL;
}

static Token make_token(Scanner *scanner, TokenType type) {
    Token token;
    token.type = type;
    token.start = scanner->start;
    token.length = (int)(scanner->current - scanner->start);
    XR_DCHECK(token.length >= 0, "make_token: negative token length");
    // L-02: report the START line/column of the token. For multi-line tokens
    // scanner->line/line_start have already advanced to the token's last line,
    // so we use the snapshots captured by scanner_begin_token().
    token.line = scanner->start_line;
    token.column = (int)(scanner->start - scanner->start_line_start) + 1;  // 1-indexed
    token.has_leading_space = scanner->had_leading_space;
    token.error_message = NULL;
    // Attach pending trivia
    token.leading_trivia = scanner->pending_trivia;
    scanner->pending_trivia = NULL;
    scanner->trivia_tail = NULL;
    // L-06: same-line inline trailing comment, if any. EOF tokens never
    // have trailing trivia (nothing follows).
    token.trailing_trivia = (type == TK_EOF) ? NULL
                                             : scan_inline_trailing_trivia(scanner);
    return token;
}

static Token error_token(Scanner *scanner, const char *message) {
    Token token;
    token.type = TK_ERROR;
    // L-03: error_message carries the diagnostic; start still points into the
    // source buffer at the offending character so editors can place a caret.
    token.start = scanner->start;
    token.length = (int)(scanner->current - scanner->start);
    if (token.length < 0) token.length = 0;
    token.line = scanner->start_line;
    token.column = (int)(scanner->start - scanner->start_line_start) + 1;
    token.has_leading_space = scanner->had_leading_space;
    token.error_message = message;
    token.leading_trivia = scanner->pending_trivia;
    token.trailing_trivia = NULL;
    scanner->pending_trivia = NULL;
    scanner->trivia_tail = NULL;
    return token;
}

// Skip whitespace and comments, returns true if any whitespace/comments were skipped
static bool skip_whitespace(Scanner *scanner) {
    bool skipped = false;
    for (;;) {
        // SIMD fast path: batch skip whitespace
        size_t remaining = (size_t)(scanner->end - scanner->current);
        if (remaining >= 16) {
            const char *non_ws = xr_simd_skip_whitespace(scanner->current, remaining);
            if (non_ws > scanner->current) {
                skipped = true;
            }
            while (scanner->current < non_ws) {
                if (*scanner->current == '\n') {
                    scanner->line++;
                    scanner->line_start = scanner->current + 1;
                }
                scanner->current++;
            }
        }

        char c = peek(scanner);
        switch (c) {
            case ' ':
            case '\r':
            case '\t':
                skipped = true;
                advance(scanner);
                break;
            case '\n':
                skipped = true;
                scanner->line++;
                advance(scanner);
                scanner->line_start = scanner->current;
                break;
            case '/':
                if (peek_next(scanner) == '/') {
                    // Single-line comment - use SIMD to find line end
                    skipped = true;
                    int comment_line = scanner->line;
                    advance(scanner); // /
                    advance(scanner); // /
                    const char *comment_start = scanner->current;
                    remaining = (size_t)(scanner->end - scanner->current);
                    if (remaining >= 16) {
                        const char *eol = xr_simd_find_newline(scanner->current, remaining);
                        scanner->current = eol;
                    }
                    while (peek(scanner) != '\n' && !is_at_end(scanner)) {
                        advance(scanner);
                    }
                    // Collect trivia if enabled
                    if (scanner->collect_trivia) {
                        int comment_len = (int)(scanner->current - comment_start);
                        XrTrivia *trivia = xr_trivia_new(TRIVIA_LINE_COMMENT, comment_start, comment_len, comment_line);
                        trivia_append(scanner, trivia);
                    }
                } else if (peek_next(scanner) == '*') {
                    // Multi-line comment
                    skipped = true;
                    int comment_line = scanner->line;
                    advance(scanner); // /
                    advance(scanner); // *
                    const char *comment_start = scanner->current;
                    while (!is_at_end(scanner)) {
                        if (peek(scanner) == '*' && peek_next(scanner) == '/') {
                            // Collect trivia before consuming */
                            if (scanner->collect_trivia) {
                                int comment_len = (int)(scanner->current - comment_start);
                                XrTrivia *trivia = xr_trivia_new(TRIVIA_BLOCK_COMMENT, comment_start, comment_len, comment_line);
                                trivia_append(scanner, trivia);
                            }
                            advance(scanner); // *
                            advance(scanner); // /
                            break;
                        }
                        if (peek(scanner) == '\n') {
                            scanner->line++;
                            scanner->line_start = scanner->current + 1;
                        }
                        advance(scanner);
                    }
                    // Unterminated block comment
                    if (is_at_end(scanner) && !(scanner->current >= comment_start + 2 &&
                        scanner->current[-2] == '*' && scanner->current[-1] == '/')) {
                        scanner->pending_error = "unterminated block comment";
                        return skipped;
                    }
                } else {
                    return skipped;
                }
                break;
            default:
                return skipped;
        }
    }
}

// ============================================================================
// Keyword Table (single source of truth: xkeywords.def)
// ============================================================================

typedef struct {
    const char *name;
    int length;
    TokenType type;
} XrKeyword;

// Sorted lookup table. Order MUST match xkeywords.def, which is itself
// sorted by ASCII memcmp() then by length (matches our binary search).
static const XrKeyword keywords[] = {
#define XR_KW(name, len, type) { name, len, type },
#include "xkeywords.def"
#undef XR_KW
};

#define NUM_KEYWORDS (sizeof(keywords) / sizeof(keywords[0]))

// Compile-time check: each entry's `length` field equals sizeof(spelling) - 1.
// Catches manual-edit mistakes in xkeywords.def at build time.
#define XR_KW(name, len, type) \
    _Static_assert(sizeof(name) - 1 == (len), \
                   "keyword length mismatch in xkeywords.def: " name);
#include "xkeywords.def"
#undef XR_KW

static TokenType identifier_type(Scanner *scanner) {
    const char *s = scanner->start;
    int len = (int)(scanner->current - scanner->start);

    // Single underscore is the match wildcard pattern, not an identifier.
    if (len == 1 && s[0] == '_') {
        return TK_UNDERSCORE;
    }

    // Binary search keywords[] using lexicographic-then-length ordering.
    // The same total order is used to sort xkeywords.def, so this is correct
    // by construction; the lengths-differ branch ensures shorter prefixes
    // (e.g. "in") are found instead of being conflated with longer keywords
    // (e.g. "int" / "interface"), and prefixes of keywords (e.g. user-named
    // `iffy`) cannot collide with the keyword (`if`) because length differs.
    int lo = 0, hi = (int)NUM_KEYWORDS - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        const XrKeyword *kw = &keywords[mid];
        int min_len = len < kw->length ? len : kw->length;
        int cmp = memcmp(s, kw->name, (size_t)min_len);
        if (cmp == 0) cmp = len - kw->length;
        if (cmp == 0) return kw->type;
        if (cmp < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return TK_NAME;
}

static Token raw_string_with_quote(Scanner *scanner, char quote);

static Token identifier(Scanner *scanner) {
    while (XR_IS_IDENT(peek(scanner))) {
        advance(scanner);
    }
    // Detect raw string prefix: single 'r' followed by quote
    int len = (int)(scanner->current - scanner->start);
    if (len == 1 && scanner->start[0] == 'r') {
        char next = peek(scanner);
        if (next == '"' || next == '\'') {
            advance(scanner);
            return raw_string_with_quote(scanner, next);
        }
    }
    return make_token(scanner, identifier_type(scanner));
}

static int is_binary_digit(char c) {
    return c == '0' || c == '1';
}

static int is_octal_digit(char c) {
    return c >= '0' && c <= '7';
}

static Token number(Scanner *scanner) {
    TokenType type = TK_LITERAL_INT;
    char first = scanner->start[0];

    if (first == '0' && !is_at_end(scanner)) {
        char prefix = peek(scanner);

        // Hex: 0x or 0X
        if (prefix == 'x' || prefix == 'X') {
            advance(scanner);
            if (!XR_IS_HEX(peek(scanner))) {
                return error_token(scanner, "Hex literal requires at least one digit");
            }
            while (XR_IS_HEX(peek(scanner)) || peek(scanner) == '_') {
                advance(scanner);
            }
            if (peek(scanner) == 'n') {
                advance(scanner);
                return make_token(scanner, TK_LITERAL_BIGINT);
            }
            return make_token(scanner, TK_LITERAL_INT);
        }

        // Binary: 0b or 0B
        if (prefix == 'b' || prefix == 'B') {
            advance(scanner);
            if (!is_binary_digit(peek(scanner))) {
                return error_token(scanner, "Binary literal requires at least one digit");
            }
            while (is_binary_digit(peek(scanner)) || peek(scanner) == '_') {
                advance(scanner);
            }
            if (peek(scanner) == 'n') {
                advance(scanner);
                return make_token(scanner, TK_LITERAL_BIGINT);
            }
            return make_token(scanner, TK_LITERAL_INT);
        }

        // Octal: 0o or 0O
        if (prefix == 'o' || prefix == 'O') {
            advance(scanner);
            if (!is_octal_digit(peek(scanner))) {
                return error_token(scanner, "Octal literal requires at least one digit");
            }
            while (is_octal_digit(peek(scanner)) || peek(scanner) == '_') {
                advance(scanner);
            }
            if (peek(scanner) == 'n') {
                advance(scanner);
                return make_token(scanner, TK_LITERAL_BIGINT);
            }
            return make_token(scanner, TK_LITERAL_INT);
        }
    }

    // Decimal integer
    while (XR_IS_DIGIT(peek(scanner)) || peek(scanner) == '_') {
        advance(scanner);
    }

    // Check decimal point
    if (peek(scanner) == '.') {
        if (peek_next(scanner) == '.') {
            // Range operator ..
        } else if (XR_IS_DIGIT(peek_next(scanner))) {
            type = TK_LITERAL_FLOAT;
            advance(scanner);
            while (XR_IS_DIGIT(peek(scanner)) || peek(scanner) == '_') {
                advance(scanner);
            }
        }
    }

    // Scientific notation
    if (peek(scanner) == 'e' || peek(scanner) == 'E') {
        type = TK_LITERAL_FLOAT;
        advance(scanner);
        if (peek(scanner) == '+' || peek(scanner) == '-') {
            advance(scanner);
        }
        while (XR_IS_DIGIT(peek(scanner)) || peek(scanner) == '_') {
            advance(scanner);
        }
    }

    // BigInt suffix 'n'
    if (peek(scanner) == 'n') {
        if (type == TK_LITERAL_FLOAT) {
            return error_token(scanner, "BigInt cannot have decimal part");
        }
        advance(scanner);
        return make_token(scanner, TK_LITERAL_BIGINT);
    }

    return make_token(scanner, type);
}

// String scanning with SIMD optimization
// Detects ${} interpolation: returns TK_TEMPLATE_STRING when found
static Token string_with_quote(Scanner *scanner, char quote) {
    bool has_interpolation = false;
    while (!is_at_end(scanner)) {
        size_t remaining = (size_t)(scanner->end - scanner->current);
        const char *found = xr_simd_find_string_end_quote(scanner->current, remaining, quote);

        while (scanner->current < found) {
            // Detect ${} in SIMD-skipped region ($ is not a SIMD stop char)
            if (*scanner->current == '$' && (scanner->current + 1) < scanner->end
                && *(scanner->current + 1) == '{') {
                has_interpolation = true;
            }
            if (*scanner->current == '\n') {
                scanner->line++;
                scanner->line_start = scanner->current + 1;
            }
            scanner->current++;
        }

        if (is_at_end(scanner)) break;

        char c = peek(scanner);
        if (c == quote) {
            advance(scanner);
            return make_token(scanner, has_interpolation ? TK_TEMPLATE_STRING : TK_LITERAL_STRING);
        } else if (c == '\\') {
            advance(scanner);
            if (!is_at_end(scanner)) {
                if (peek(scanner) == '\n') {
                    scanner->line++;
                    scanner->line_start = scanner->current + 1;
                }
                advance(scanner);
            }
        } else {
            // Detect ${} interpolation
            if (c == '$' && peek_next(scanner) == '{') {
                has_interpolation = true;
            }
            if (c == '\n') {
                scanner->line++;
                scanner->line_start = scanner->current + 1;
            }
            advance(scanner);
        }
    }

    return error_token(scanner, "Unterminated string");
}

static Token string(Scanner *scanner) {
    return string_with_quote(scanner, '"');
}

static Token single_quote_string(Scanner *scanner) {
    return string_with_quote(scanner, '\'');
}

// Raw string: r"..." or r'...' (no escape processing, but ${} interpolation)
static Token raw_string_with_quote(Scanner *scanner, char quote) {
    bool has_interpolation = false;
    while (!is_at_end(scanner) && peek(scanner) != quote) {
        if (peek(scanner) == '$' && peek_next(scanner) == '{') {
            has_interpolation = true;
        }
        if (peek(scanner) == '\n') {
            scanner->line++;
            scanner->line_start = scanner->current + 1;
        }
        advance(scanner);
    }
    if (is_at_end(scanner)) {
        return error_token(scanner, "Unterminated raw string");
    }
    advance(scanner);
    return make_token(scanner, has_interpolation ? TK_RAW_TEMPLATE_STRING : TK_RAW_STRING);
}

// Backtick template strings are deprecated — use "" or '' with ${} instead

// Regex literal: /pattern/flags
static Token regex_literal(Scanner *scanner) {
    // scanner->start already points to '/'
    // scanner->current already skips the starting '/'

    // Scan regex pattern
    while (!is_at_end(scanner) && peek(scanner) != '/') {
        if (peek(scanner) == '\\') {
            // Escape character: skip the next character
            advance(scanner);
            if (!is_at_end(scanner)) {
                advance(scanner);
            }
        } else if (peek(scanner) == '\n') {
            // Regex cannot contain newline
            return error_token(scanner, "Regex cannot contain newline");
        } else if (peek(scanner) == '[') {
            // Character class
            advance(scanner);
            while (!is_at_end(scanner) && peek(scanner) != ']') {
                if (peek(scanner) == '\\') {
                    advance(scanner);
                    if (!is_at_end(scanner)) advance(scanner);
                } else if (peek(scanner) == '\n') {
                    return error_token(scanner, "Regex cannot contain newline");
                } else {
                    advance(scanner);
                }
            }
            if (!is_at_end(scanner)) advance(scanner);
        } else {
            advance(scanner);
        }
    }

    if (is_at_end(scanner)) {
        return error_token(scanner, "Unterminated regex");
    }

    // Consume the ending '/'
    advance(scanner);

    // Scan flags: i, m, s, g, u
    while (!is_at_end(scanner)) {
        char c = peek(scanner);
        if (c == 'i' || c == 'm' || c == 's' || c == 'g' || c == 'u' || c == 'U') {
            advance(scanner);
        } else {
            break;
        }
    }

    return make_token(scanner, TK_LITERAL_REGEX);
}

// Try to scan regex literal (called when expecting expression)
Token xr_scanner_try_regex(Scanner *scanner) {
    scanner->had_leading_space = skip_whitespace(scanner);
    scanner_begin_token(scanner);

    if (is_at_end(scanner) || peek(scanner) != '/') {
        return xr_scanner_scan(scanner);
    }

    advance(scanner);

    // Check for comment
    if (peek(scanner) == '/' || peek(scanner) == '*') {
        scanner->current = scanner->start;
        return xr_scanner_scan(scanner);
    }

    // Check for /=
    if (peek(scanner) == '=') {
        scanner->current = scanner->start;
        return xr_scanner_scan(scanner);
    }

    return regex_literal(scanner);
}

// Scan next token
Token xr_scanner_scan(Scanner *scanner) {
    scanner->had_leading_space = skip_whitespace(scanner);
    scanner_begin_token(scanner);

    // Check for errors detected during whitespace/comment skipping
    if (scanner->pending_error) {
        const char *msg = scanner->pending_error;
        scanner->pending_error = NULL;
        return error_token(scanner, msg);
    }

    if (is_at_end(scanner)) {
        return make_token(scanner, TK_EOF);
    }

    char c = advance(scanner);

    // Identifier or keyword (ASCII alpha, underscore, or UTF-8 lead byte)
    if (XR_IS_ALPHA(c) || c == '_' || (unsigned char)c >= 0x80) {
        return identifier(scanner);
    }

    // Number
    if (XR_IS_DIGIT(c)) {
        return number(scanner);
    }

    // Other symbols
    switch (c) {
        case '(': return make_token(scanner, TK_LPAREN);
        case ')': return make_token(scanner, TK_RPAREN);
        case '{': return make_token(scanner, TK_LBRACE);
        case '}': return make_token(scanner, TK_RBRACE);
        case '[':
            return make_token(scanner, TK_LBRACKET); // [
        case ']': return make_token(scanner, TK_RBRACKET);
        case ',': return make_token(scanner, TK_COMMA);
        case '.':
            if (match(scanner, '.')) {
                if (match(scanner, '.')) {
                    return make_token(scanner, TK_DOT_DOT_DOT);
                }
                return make_token(scanner, TK_RANGE);
            }
            return make_token(scanner, TK_DOT);
        case ':': return make_token(scanner, TK_COLON);
        case ';': return make_token(scanner, TK_SEMICOLON);
        case '+':
            if (match(scanner, '+')) {
                return make_token(scanner, TK_INC); // ++
            }
            return make_token(scanner, match(scanner, '=') ? TK_PLUS_ASSIGN : TK_PLUS);
        case '-':
            if (match(scanner, '-')) {
                return make_token(scanner, TK_DEC); // --
            }
            return make_token(scanner, match(scanner, '=') ? TK_MINUS_ASSIGN : TK_MINUS);
        case '*':
            return make_token(scanner, match(scanner, '=') ? TK_MUL_ASSIGN : TK_STAR);
        case '/':
            return make_token(scanner, match(scanner, '=') ? TK_DIV_ASSIGN : TK_SLASH);
        case '%':
            return make_token(scanner, match(scanner, '=') ? TK_MOD_ASSIGN : TK_PERCENT);
        case '@':
            if (XR_IS_ALPHA(peek(scanner)) || peek(scanner) == '_' || (unsigned char)peek(scanner) >= 0x80) {
                return make_token(scanner, TK_AT);
            }
            return error_token(scanner, "@ must be followed by identifier");
        case '#':
            if (peek(scanner) == '[') {
                advance(scanner);
                return make_token(scanner, TK_SET_START);
            }
            if (peek(scanner) == '{') {
                advance(scanner);
                return make_token(scanner, TK_EMPTY_MAP_START);
            }
            return make_token(scanner, TK_HASH);
        case '!':
            if (match(scanner, '=')) {
                return make_token(scanner, match(scanner, '=') ? TK_NE_STRICT : TK_NE);
            }
            return make_token(scanner, TK_NOT);
        case '=':
            if (match(scanner, '=')) {
                return make_token(scanner, match(scanner, '=') ? TK_EQ_STRICT : TK_EQ);
            } else if (match(scanner, '>')) {
                return make_token(scanner, TK_ARROW);
            }
            return make_token(scanner, TK_ASSIGN);
        case '<':
            if (match(scanner, '<')) {
                return make_token(scanner, match(scanner, '=') ? TK_LSHIFT_ASSIGN : TK_LSHIFT);
            }
            return make_token(scanner, match(scanner, '=') ? TK_LE : TK_LT);
        case '>':
            if (match(scanner, '>')) {
                return make_token(scanner, match(scanner, '=') ? TK_RSHIFT_ASSIGN : TK_RSHIFT);
            }
            return make_token(scanner, match(scanner, '=') ? TK_GE : TK_GT);
        case '&':
            if (match(scanner, '&')) return make_token(scanner, TK_AND);
            return make_token(scanner, match(scanner, '=') ? TK_AND_ASSIGN : TK_AMP);
        case '|':
            if (match(scanner, '|')) {
                return make_token(scanner, TK_OR); // ||
            }
            return make_token(scanner, match(scanner, '=') ? TK_OR_ASSIGN : TK_PIPE);
        case '^':
            return make_token(scanner, match(scanner, '=') ? TK_XOR_ASSIGN : TK_CARET);
        case '~':
            return make_token(scanner, TK_TILDE);
        case '\\':
            return error_token(scanner, "Invalid backslash character");
        case '?':
            if (match(scanner, '.')) {
                return make_token(scanner, TK_QUESTION_DOT);
            }
            if (match(scanner, '?')) {
                return make_token(scanner, TK_NULLISH_COALESCE);
            }
            return make_token(scanner, TK_QUESTION);
        case '"':
            return string(scanner);
        case '\'':
            return single_quote_string(scanner);
        case '`':
            return error_token(scanner, "Backtick strings are deprecated, use \"\" or '' with ${} interpolation");
    }

    return error_token(scanner, "Unknown character");
}

// Token name lookup table (indexed by TokenType value)
static const char *token_names[] = {
    // Single character tokens (ASCII values as index)
    [TK_LPAREN]    = "(", [TK_RPAREN]    = ")", [TK_LBRACE]    = "{",
    [TK_RBRACE]    = "}", [TK_LBRACKET]  = "[", [TK_RBRACKET]  = "]",
    [TK_COMMA]     = ",", [TK_DOT]       = ".", [TK_COLON]     = ":",
    [TK_SEMICOLON] = ";", [TK_PLUS]      = "+", [TK_MINUS]     = "-",
    [TK_STAR]      = "*", [TK_SLASH]     = "/", [TK_PERCENT]   = "%",
    [TK_HASH]      = "#", [TK_AMP]       = "&", [TK_CARET]     = "^",
    [TK_TILDE]     = "~",

    // Multi-character tokens (>= 256)
    [TK_EQ]        = "==",  [TK_NE]          = "!=",  [TK_EQ_STRICT]  = "===",
    [TK_NE_STRICT] = "!==", [TK_LT]          = "<",   [TK_LE]         = "<=",
    [TK_GT]        = ">",   [TK_GE]          = ">=",  [TK_LSHIFT]     = "<<",
    [TK_RSHIFT]    = ">>",  [TK_ASSIGN]      = "=",   [TK_PLUS_ASSIGN] = "+=",
    [TK_MINUS_ASSIGN] = "-=", [TK_MUL_ASSIGN] = "*=", [TK_DIV_ASSIGN] = "/=",
    [TK_MOD_ASSIGN] = "%=", [TK_AND_ASSIGN]  = "&=",  [TK_OR_ASSIGN]  = "|=",
    [TK_XOR_ASSIGN] = "^=", [TK_LSHIFT_ASSIGN] = "<<=", [TK_RSHIFT_ASSIGN] = ">>=",
    [TK_INC]       = "++",  [TK_DEC]         = "--",  [TK_AND]        = "&&",
    [TK_OR]        = "||",  [TK_NOT]         = "!",

    // Keywords
    [TK_LET]       = "let",       [TK_CONST]      = "const",
    [TK_SHARED]    = "shared",    [TK_IF]         = "if",
    [TK_ELSE]      = "else",      [TK_WHILE]      = "while",
    [TK_FOR]       = "for",       [TK_IN]         = "in",
    [TK_IS]        = "is",        [TK_BREAK]      = "break",
    [TK_CONTINUE]  = "continue",  [TK_RETURN]     = "return",
    [TK_YIELD]     = "yield",     [TK_NULL]       = "null",
    [TK_TRUE]      = "true",      [TK_FALSE]      = "false",
    [TK_CLASS]     = "class",     [TK_STRUCT]     = "struct",
    [TK_EXTENDS]   = "extends",   [TK_INTERFACE]  = "interface",
    [TK_IMPLEMENTS]= "implements",[TK_FN]         = "fn",
    [TK_NEW]       = "new",       [TK_THIS]       = "this",
    [TK_SUPER]     = "super",     [TK_CONSTRUCTOR]= "constructor",
    [TK_STATIC]    = "static",    [TK_PRIVATE]    = "private",
    [TK_PUBLIC]    = "public",    [TK_OPERATOR]   = "operator",
    [TK_ABSTRACT]  = "abstract",  [TK_OVERRIDE]   = "override",
    [TK_FINAL]     = "final",     [TK_ENUM]       = "enum",
    [TK_MATCH]     = "match",     [TK_TYPE_ALIAS] = "type",

    // Exception handling
    [TK_TRY]       = "try",       [TK_CATCH]      = "catch",
    [TK_FINALLY]   = "finally",   [TK_THROW]      = "throw",

    // Module system
    [TK_IMPORT]    = "import",    [TK_EXPORT]     = "export",
    [TK_AS]        = "as",

    // Coroutine
    [TK_GO]        = "go",        [TK_AWAIT]      = "await",
    [TK_SELECT]    = "select",    [TK_DEFER]      = "defer",
    [TK_SCOPE]     = "scope",

    // Contextual keywords (not in keyword range, identified by parser)
    [TK_FROM]      = "from",      [TK_TO]         = "to",
    [TK_DEFAULT]   = "default",   [TK_CANCELLED]  = "cancelled",
    [TK_REF]       = "ref",       [TK_MOVE]       = "move",

    // Type keywords
    [TK_VOID]      = "void",      [TK_INT]        = "int",
    [TK_FLOAT]     = "float",     [TK_STRING]     = "string",
    [TK_BOOL]      = "bool",      [TK_INT8]       = "int8",
    [TK_INT16]     = "int16",     [TK_INT32]      = "int32",
    [TK_INT64]     = "int64",     [TK_UINT8]      = "uint8",
    [TK_UINT16]    = "uint16",    [TK_UINT32]     = "uint32",
    [TK_UINT64]    = "uint64",    [TK_FLOAT32]    = "float32",
    [TK_FLOAT64]   = "float64",   [TK_TYPE_ARRAY] = "Array",
    [TK_TYPE_MAP]  = "Map",       [TK_TYPE_SET]   = "Set",
    [TK_TYPE_JSON] = "Json",      [TK_TYPE_CHANNEL]= "Channel",
    [TK_TYPE_BIGINT]= "BigInt",   [TK_TYPE_RANGE] = "Range",
    [TK_TYPE_DATETIME]= "DateTime", [TK_TYPE_BYTES] = "Bytes",
    [TK_UNKNOWN]    = "unknown",

    // Type operators / special
    [TK_QUESTION]  = "?",         [TK_QUESTION_DOT] = "?.",
    [TK_PIPE]      = "|",         [TK_ARROW]      = "=>",
    [TK_DOT_DOT_DOT] = "...",     [TK_RANGE]      = "..",
    [TK_NULLISH_COALESCE] = "??", [TK_UNDERSCORE] = "_",

    // Syntax tokens
    [TK_AT]        = "@",         [TK_EMPTY_MAP_START] = "#{",
    [TK_SET_START] = "#[",

    // Literals
    [TK_LITERAL_INT]   = "LITERAL_INT",   [TK_LITERAL_FLOAT] = "LITERAL_FLOAT",
    [TK_LITERAL_BIGINT]= "LITERAL_BIGINT",[TK_LITERAL_STRING]= "LITERAL_STRING",
    [TK_LITERAL_REGEX] = "LITERAL_REGEX", [TK_NAME]          = "NAME",
    [TK_TEMPLATE_STRING] = "TEMPLATE_STRING",
    [TK_RAW_STRING]    = "RAW_STRING",
    [TK_RAW_TEMPLATE_STRING] = "RAW_TEMPLATE_STRING",

    // Special
    [TK_EOF]       = "EOF",       [TK_ERROR]      = "ERROR",
};

const char *xr_token_name(TokenType type) {
    if (type >= 0 && type < (TokenType)(sizeof(token_names) / sizeof(token_names[0]))
        && token_names[type]) {
        return token_names[type];
    }
    return "UNKNOWN";
}

