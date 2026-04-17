/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xregex_parse.c - Regular expression parser
 *
 * KEY CONCEPT:
 *   Parses regex pattern string into AST (Abstract Syntax Tree).
 *
 * SUPPORTED SYNTAX:
 *   - Literal characters
 *   - Character classes [abc], [^abc], [a-z]
 *   - Predefined classes \d, \w, \s etc.
 *   - Quantifiers *, +, ?, {n}, {n,}, {n,m}
 *   - Non-greedy quantifiers *?, +?, ??
 *   - Groups (...), (?:...), (?P<name>...)
 *   - Alternation a|b
 *   - Anchors ^, $, \A, \z, \b, \B
 *   - Escapes \n, \t, \r, \x00, \u0000 etc.
 *   - Unicode properties \p{L}, \p{Han}, \P{N} etc.
 */

#include "xregex_internal.h"
#include "../../src/base/xunicode.h"

/* ========================================================================
 * AST Node Creation (using Arena allocation)
 * ======================================================================== */

static XrAstNode* ast_new(XrArena *arena, XrAstType type) {
    XrAstNode *node = (XrAstNode*)xr_arena_alloc(arena, sizeof(XrAstNode));
    memset(node, 0, sizeof(XrAstNode));
    node->type = type;
    return node;
}

static char* ast_strdup(XrArena *arena, const char *str, int len) {
    char *copy = (char*)xr_arena_alloc(arena, len + 1);
    memcpy(copy, str, len);
    copy[len] = '\0';
    return copy;
}

static XrAstNode* ast_literal(XrArena *arena, const char *str, int len) {
    XrAstNode *node = ast_new(arena, XR_AST_LITERAL);
    node->literal.str = ast_strdup(arena, str, len);
    node->literal.len = len;
    return node;
}

static XrAstNode* ast_literal_char(XrArena *arena, char c) {
    return ast_literal(arena, &c, 1);
}

static XrAstNode* ast_char_class(XrArena *arena) {
    XrAstNode *node = ast_new(arena, XR_AST_CHAR_CLASS);
    node->cc.ranges = NULL;
    node->cc.count = 0;
    node->cc.cap = 0;
    node->cc.negated = false;
    node->cc.unicode_prop = 0;
    node->cc.has_unicode_prop = false;
    return node;
}

static void ast_cc_add_range(XrArena *arena, XrAstNode *node, uint8_t lo, uint8_t hi) {
    assert(node->type == XR_AST_CHAR_CLASS);
    if (node->cc.count >= node->cc.cap) {
        int new_cap = node->cc.cap ? node->cc.cap * 2 : 8;
        XrCharRange *new_ranges = (XrCharRange*)xr_arena_alloc(
            arena, new_cap * sizeof(XrCharRange));
        if (node->cc.ranges) {
            memcpy(new_ranges, node->cc.ranges,
                   node->cc.count * sizeof(XrCharRange));
        }
        node->cc.ranges = new_ranges;
        node->cc.cap = new_cap;
    }
    node->cc.ranges[node->cc.count].lo = lo;
    node->cc.ranges[node->cc.count].hi = hi;
    node->cc.count++;
}

// Set Unicode property (for \p{...})
static void ast_cc_set_unicode_prop(XrAstNode *node, uint32_t prop_id) {
    assert(node->type == XR_AST_CHAR_CLASS);
    node->cc.unicode_prop = prop_id;
    node->cc.has_unicode_prop = true;
}

static XrAstNode* ast_any(XrArena *arena, bool dotall) {
    XrAstNode *node = ast_new(arena, XR_AST_ANY);
    node->any_dotall = dotall;
    return node;
}

static XrAstNode* ast_empty_match(XrArena *arena, XrEmptyWidth flags) {
    XrAstNode *node = ast_new(arena, XR_AST_EMPTY_MATCH);
    node->empty_flags = flags;
    return node;
}

static XrAstNode* ast_concat(XrArena *arena) {
    XrAstNode *node = ast_new(arena, XR_AST_CONCAT);
    node->list.children = NULL;
    node->list.count = 0;
    node->list.cap = 0;
    return node;
}

static XrAstNode* ast_alt(XrArena *arena) {
    XrAstNode *node = ast_new(arena, XR_AST_ALT);
    node->list.children = NULL;
    node->list.count = 0;
    node->list.cap = 0;
    return node;
}

static void ast_list_add(XrArena *arena, XrAstNode *node, XrAstNode *child) {
    assert(node->type == XR_AST_CONCAT || node->type == XR_AST_ALT);
    if (node->list.count >= node->list.cap) {
        int new_cap = node->list.cap ? node->list.cap * 2 : 4;
        XrAstNode **new_children = (XrAstNode**)xr_arena_alloc(
            arena, new_cap * sizeof(XrAstNode*));
        if (node->list.children) {
            memcpy(new_children, node->list.children,
                   node->list.count * sizeof(XrAstNode*));
        }
        node->list.children = new_children;
        node->list.cap = new_cap;
    }
    node->list.children[node->list.count++] = child;
}

static XrAstNode* ast_repeat(XrArena *arena, XrAstNode *child, int min, int max, bool greedy) {
    XrAstNode *node = ast_new(arena, XR_AST_REPEAT);
    node->repeat.child = child;
    node->repeat.min = min;
    node->repeat.max = max;
    node->repeat.greedy = greedy;
    return node;
}

static XrAstNode* ast_capture(XrArena *arena, XrAstNode *child, int index, const char *name) {
    XrAstNode *node = ast_new(arena, XR_AST_CAPTURE);
    node->capture.child = child;
    node->capture.index = index;
    if (name) {
        int len = strlen(name);
        node->capture.name = ast_strdup(arena, name, len);
    } else {
        node->capture.name = NULL;
    }
    return node;
}

/* ========================================================================
 * AST Free
 * ======================================================================== */

void xr_regex_ast_free(XrAstNode *node) {
    // AST nodes are managed by Arena, this function is a no-op.
    // Interface kept for compatibility with old code calls.
    // Arena is freed at once via xr_arena_destroy() after compilation.
    (void)node;
}

/* ========================================================================
 * Parser Helper Functions
 * ======================================================================== */

static inline bool at_end(XrParser *p) {
    return p->p >= p->end;
}

static inline char peek(XrParser *p) {
    return at_end(p) ? '\0' : *p->p;
}

static inline char peek_next(XrParser *p) {
    return (p->p + 1 < p->end) ? p->p[1] : '\0';
}

static inline void advance(XrParser *p) {
    if (!at_end(p)) p->p++;
}

static inline bool match(XrParser *p, char c) {
    if (peek(p) == c) {
        advance(p);
        return true;
    }
    return false;
}

static void parser_error(XrParser *p, XrRegexError code, const char *msg) {
    if (p->error == XR_RE_OK) {
        p->error = code;
        p->error_pos = (int)(p->p - p->pattern);
        snprintf(p->error_msg, sizeof(p->error_msg), "%s", msg);
    }
}

// Add named capture group
static void parser_add_capture_name(XrParser *p, int index, const char *name) {
    if (p->names_count >= p->names_cap) {
        int new_cap = p->names_cap ? p->names_cap * 2 : 8;
        XR_REALLOC_OR_ABORT(p->capture_names,
                            (size_t)new_cap * sizeof(char*),
                            "regex capture_names grow");
        // Initialize newly allocated space to NULL
        for (int i = p->names_cap; i < new_cap; i++) {
            p->capture_names[i] = NULL;
        }
        p->names_cap = new_cap;
    }
    // Ensure index is valid
    while (p->names_count <= index) {
        p->capture_names[p->names_count++] = NULL;
    }
    p->capture_names[index] = xr_re_strdup(name);
}

/* ========================================================================
 * Escape Character Parsing
 * ======================================================================== */

// Parse hexadecimal digit
static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Parse escape sequence, return parsed character
// Before call, p->p points to character after '\\'
static XrAstNode* parse_escape(XrParser *p) {
    if (at_end(p)) {
        parser_error(p, XR_RE_ERR_INVALID_ESCAPE, "trailing backslash");
        return NULL;
    }

    XrArena *arena = &p->ast_arena;
    char c = peek(p);
    advance(p);

    switch (c) {
        // Simple escapes
        case 'n': return ast_literal_char(arena, '\n');
        case 't': return ast_literal_char(arena, '\t');
        case 'r': return ast_literal_char(arena, '\r');
        case 'f': return ast_literal_char(arena, '\f');
        case 'v': return ast_literal_char(arena, '\v');
        case 'a': return ast_literal_char(arena, '\a');
        case '0': return ast_literal_char(arena, '\0');

        // Metacharacter escapes
        case '\\':
        case '.':
        case '*':
        case '+':
        case '?':
        case '[':
        case ']':
        case '(':
        case ')':
        case '{':
        case '}':
        case '|':
        case '^':
        case '$':
            return ast_literal_char(arena, c);

        // Predefined character classes
        case 'd': {
            XrAstNode *cc = ast_char_class(arena);
            ast_cc_add_range(arena, cc, '0', '9');
            return cc;
        }
        case 'D': {
            XrAstNode *cc = ast_char_class(arena);
            ast_cc_add_range(arena, cc, 0, '0' - 1);
            ast_cc_add_range(arena, cc, '9' + 1, 255);
            return cc;
        }
        case 'w': {
            XrAstNode *cc = ast_char_class(arena);
            ast_cc_add_range(arena, cc, 'a', 'z');
            ast_cc_add_range(arena, cc, 'A', 'Z');
            ast_cc_add_range(arena, cc, '0', '9');
            ast_cc_add_range(arena, cc, '_', '_');
            return cc;
        }
        case 'W': {
            XrAstNode *cc = ast_char_class(arena);
            cc->cc.negated = true;
            ast_cc_add_range(arena, cc, 'a', 'z');
            ast_cc_add_range(arena, cc, 'A', 'Z');
            ast_cc_add_range(arena, cc, '0', '9');
            ast_cc_add_range(arena, cc, '_', '_');
            return cc;
        }
        case 's': {
            XrAstNode *cc = ast_char_class(arena);
            ast_cc_add_range(arena, cc, ' ', ' ');
            ast_cc_add_range(arena, cc, '\t', '\t');
            ast_cc_add_range(arena, cc, '\n', '\n');
            ast_cc_add_range(arena, cc, '\r', '\r');
            ast_cc_add_range(arena, cc, '\f', '\f');
            ast_cc_add_range(arena, cc, '\v', '\v');
            return cc;
        }
        case 'S': {
            XrAstNode *cc = ast_char_class(arena);
            cc->cc.negated = true;
            ast_cc_add_range(arena, cc, ' ', ' ');
            ast_cc_add_range(arena, cc, '\t', '\t');
            ast_cc_add_range(arena, cc, '\n', '\n');
            ast_cc_add_range(arena, cc, '\r', '\r');
            ast_cc_add_range(arena, cc, '\f', '\f');
            ast_cc_add_range(arena, cc, '\v', '\v');
            return cc;
        }

        // Assertions
        case 'A': return ast_empty_match(arena, XR_EMPTY_BEGIN_TEXT);
        case 'z': return ast_empty_match(arena, XR_EMPTY_END_TEXT);
        case 'b': return ast_empty_match(arena, XR_EMPTY_WORD_BOUNDARY);
        case 'B': return ast_empty_match(arena, XR_EMPTY_NOT_WORD_BOUND);

        // Hexadecimal \xNN
        case 'x': {
            int h1 = hex_digit(peek(p));
            if (h1 < 0) {
                parser_error(p, XR_RE_ERR_INVALID_ESCAPE, "invalid \\x escape");
                return NULL;
            }
            advance(p);
            int h2 = hex_digit(peek(p));
            if (h2 < 0) {
                parser_error(p, XR_RE_ERR_INVALID_ESCAPE, "invalid \\x escape");
                return NULL;
            }
            advance(p);
            return ast_literal_char(arena, (char)((h1 << 4) | h2));
        }

        // Unicode property \p{...} and \P{...}
        case 'p':
        case 'P': {
            bool negated = (c == 'P');

            if (peek(p) != '{') {
                parser_error(p, XR_RE_ERR_INVALID_ESCAPE, "\\p or \\P must be followed by {");
                return NULL;
            }
            advance(p);  // skip '{'

            // Record property name start position
            const char *name_start = p->p;
            int name_len = 0;

            // Read property name until '}'
            while (!at_end(p) && peek(p) != '}') {
                advance(p);
                name_len++;
            }

            if (at_end(p) || peek(p) != '}') {
                parser_error(p, XR_RE_ERR_INVALID_ESCAPE, "unclosed \\p{...}");
                return NULL;
            }
            advance(p);  // skip '}'

            if (name_len == 0) {
                parser_error(p, XR_RE_ERR_INVALID_ESCAPE, "empty property name in \\p{}");
                return NULL;
            }

            // Lookup Unicode property
            XrUnicodeProperty prop = xr_unicode_property_lookup(name_start, name_len);
            if (prop == XR_UP_INVALID) {
                parser_error(p, XR_RE_ERR_INVALID_ESCAPE, "unknown Unicode property");
                return NULL;
            }

            // Create character class node, set Unicode property
            XrAstNode *cc = ast_char_class(arena);
            cc->cc.negated = negated;
            ast_cc_set_unicode_prop(cc, (uint32_t)prop);

            return cc;
        }

        default:
            parser_error(p, XR_RE_ERR_INVALID_ESCAPE, "invalid escape sequence");
            return NULL;
    }
}

/* ========================================================================
 * Character Class Parsing [...]
 * ======================================================================== */

// Parse single element in character class
// Return character value, -1 for error
static int parse_cc_char(XrParser *p) {
    if (at_end(p)) return -1;

    char c = peek(p);
    advance(p);

    if (c == '\\') {
        if (at_end(p)) {
            parser_error(p, XR_RE_ERR_INVALID_ESCAPE, "trailing backslash in char class");
            return -1;
        }
        char e = peek(p);
        advance(p);
        switch (e) {
            case 'n': return '\n';
            case 't': return '\t';
            case 'r': return '\r';
            case 'f': return '\f';
            case 'v': return '\v';
            case '\\': return '\\';
            case ']': return ']';
            case '[': return '[';
            case '^': return '^';
            case '-': return '-';
            case 'x': {
                int h1 = hex_digit(peek(p));
                if (h1 < 0) return -1;
                advance(p);
                int h2 = hex_digit(peek(p));
                if (h2 < 0) return -1;
                advance(p);
                return (h1 << 4) | h2;
            }
            default:
                return e;
        }
    }

    return (unsigned char)c;
}

// Parse character class [...]
static XrAstNode* parse_char_class(XrParser *p) {
    XrArena *arena = &p->ast_arena;
    XrAstNode *cc = ast_char_class(arena);

    // Negated?
    if (match(p, '^')) {
        cc->cc.negated = true;
    }

    // Special case: ] at beginning is literal
    if (peek(p) == ']') {
        ast_cc_add_range(arena, cc, ']', ']');
        advance(p);
    }

    while (!at_end(p) && peek(p) != ']') {
        int c1 = parse_cc_char(p);
        if (c1 < 0) {
            xr_regex_ast_free(cc);
            return NULL;
        }

        // Check if it's a range a-z
        if (peek(p) == '-' && peek_next(p) != ']') {
            advance(p);  // skip '-'
            int c2 = parse_cc_char(p);
            if (c2 < 0) {
                xr_regex_ast_free(cc);
                return NULL;
            }
            if (c1 > c2) {
                parser_error(p, XR_RE_ERR_INVALID_RANGE, "invalid char range [z-a]");
                xr_regex_ast_free(cc);
                return NULL;
            }
            ast_cc_add_range(arena, cc, (uint8_t)c1, (uint8_t)c2);
        } else {
            ast_cc_add_range(arena, cc, (uint8_t)c1, (uint8_t)c1);
        }
    }

    if (!match(p, ']')) {
        parser_error(p, XR_RE_ERR_UNMATCHED_BRACKET, "unclosed character class");
        xr_regex_ast_free(cc);
        return NULL;
    }

    return cc;
}

/* ========================================================================
 * Quantifier Parsing
 * ======================================================================== */

// Parse number
static int parse_number(XrParser *p) {
    int n = 0;
    while (!at_end(p) && peek(p) >= '0' && peek(p) <= '9') {
        n = n * 10 + (peek(p) - '0');
        if (n > XR_RE_MAX_REPEAT) {
            parser_error(p, XR_RE_ERR_INVALID_REPEAT, "repeat count too large");
            return -1;
        }
        advance(p);
    }
    return n;
}

// Parse repeat {n}, {n,}, {n,m}
static bool parse_repeat_range(XrParser *p, int *min, int *max) {
    *min = parse_number(p);
    if (*min < 0) return false;

    if (match(p, ',')) {
        if (peek(p) == '}') {
            *max = -1;  // {n,}
        } else {
            *max = parse_number(p);
            if (*max < 0) return false;
            if (*max < *min) {
                parser_error(p, XR_RE_ERR_INVALID_REPEAT, "invalid repeat {n,m} where n > m");
                return false;
            }
        }
    } else {
        *max = *min;  // {n}
    }

    if (!match(p, '}')) {
        parser_error(p, XR_RE_ERR_SYNTAX, "expected '}'");
        return false;
    }

    return true;
}

/* ========================================================================
 * Recursive Descent Parsing
 * ======================================================================== */

static XrAstNode* parse_expr(XrParser *p);

// Parse group (...), (?:...), (?P<name>...)
static XrAstNode* parse_group(XrParser *p) {
    bool capturing = true;
    char *name = NULL;
    XrRegexFlags saved_flags = p->flags;

    if (match(p, '?')) {
        // Check inline flags (?i), (?m), (?s), (?ims) etc.
        bool has_flags = false;
        while (!at_end(p)) {
            char c = peek(p);
            if (c == 'i') {
                p->flags |= XR_RE_IGNORECASE;
                advance(p);
                has_flags = true;
            } else if (c == 'm') {
                p->flags |= XR_RE_MULTILINE;
                advance(p);
                has_flags = true;
            } else if (c == 's') {
                p->flags |= XR_RE_DOTALL;
                advance(p);
                has_flags = true;
            } else if (c == 'U') {
                p->flags |= XR_RE_UNGREEDY;
                advance(p);
                has_flags = true;
            } else {
                break;
            }
        }

        if (has_flags) {
            if (match(p, ')')) {
                // Only set flags (?i) - permanently affects following content
                // Don't restore flags
                return ast_new(&p->ast_arena, XR_AST_EMPTY);
            } else if (match(p, ':')) {
                // Flags + non-capturing group (?i:...) - scope limited to group
                capturing = false;
            } else {
                // Syntax error
                parser_error(p, XR_RE_ERR_INVALID_GROUP, "expected ':' or ')' after flags");
                p->flags = saved_flags;
                return NULL;
            }
        } else if (match(p, ':')) {
            // Non-capturing group (?:...)
            capturing = false;
        } else if (match(p, 'P') || peek(p) == '<') {
            // Named capture group: (?P<name>...) or (?<name>...)
            if (p->p[-1] == 'P') {
                // (?P<name>...) syntax
                if (!match(p, '<')) {
                    parser_error(p, XR_RE_ERR_INVALID_GROUP, "expected '<' after (?P");
                    p->flags = saved_flags;
                    return NULL;
                }
            } else {
                // (?<name>...) syntax
                advance(p); // skip '<'
            }
            // Read name
            const char *name_start = p->p;
            while (!at_end(p) && peek(p) != '>') {
                advance(p);
            }
            int name_len = (int)(p->p - name_start);
            if (name_len == 0) {
                parser_error(p, XR_RE_ERR_INVALID_GROUP, "empty group name");
                p->flags = saved_flags;
                return NULL;
            }
            if (!match(p, '>')) {
                parser_error(p, XR_RE_ERR_INVALID_GROUP, "expected '>'");
                p->flags = saved_flags;
                return NULL;
            }
            name = (char*)xr_re_alloc(name_len + 1);
            memcpy(name, name_start, name_len);
            name[name_len] = '\0';
        } else {
            parser_error(p, XR_RE_ERR_INVALID_GROUP, "invalid group syntax");
            p->flags = saved_flags;
            return NULL;
        }
    }

    // Parse group content
    XrAstNode *expr = parse_expr(p);
    if (!expr) {
        xr_re_free(name);
        return NULL;
    }

    if (!match(p, ')')) {
        parser_error(p, XR_RE_ERR_UNMATCHED_PAREN, "unclosed group");
        xr_regex_ast_free(expr);
        xr_re_free(name);
        p->flags = saved_flags;
        return NULL;
    }

    // Restore flags (scope limited to group)
    p->flags = saved_flags;

    if (capturing) {
        int index = p->capture_index++;
        const char *cap_name = NULL;
        if (name) {
            parser_add_capture_name(p, index, name);
            cap_name = p->capture_names[index];
            xr_re_free(name);
            name = NULL;
        }
        return ast_capture(&p->ast_arena, expr, index, cap_name);
    }

    xr_re_free(name);
    return expr;
}

// Parse atom (single matching unit)
static XrAstNode* parse_atom(XrParser *p) {
    if (at_end(p)) return NULL;

    char c = peek(p);

    switch (c) {
        case '(':
            advance(p);
            return parse_group(p);

        case '[':
            advance(p);
            return parse_char_class(p);

        case '.': {
            advance(p);
            return ast_any(&p->ast_arena, (p->flags & XR_RE_DOTALL) != 0);
        }

        case '^': {
            advance(p);
            XrArena *a = &p->ast_arena;
            if (p->flags & XR_RE_MULTILINE) {
                return ast_empty_match(a, XR_EMPTY_BEGIN_LINE);
            }
            return ast_empty_match(a, XR_EMPTY_BEGIN_TEXT);
        }

        case '$': {
            advance(p);
            XrArena *a = &p->ast_arena;
            if (p->flags & XR_RE_MULTILINE) {
                return ast_empty_match(a, XR_EMPTY_END_LINE);
            }
            return ast_empty_match(a, XR_EMPTY_END_TEXT);
        }

        case '\\':
            advance(p);
            return parse_escape(p);

        // These characters are invalid at atom position
        case '|':
        case ')':
        case '*':
        case '+':
        case '?':
        case '{':
            return NULL;

        default:
            // Literal character
            advance(p);
            return ast_literal_char(&p->ast_arena, c);
    }
}

// Parse quantifier (*, +, ?, {n,m} after atom)
static XrAstNode* parse_quantified(XrParser *p) {
    XrAstNode *atom = parse_atom(p);
    if (!atom) return NULL;

    // Check quantifier
    int min = 1, max = 1;
    bool has_quantifier = false;

    switch (peek(p)) {
        case '*':
            advance(p);
            min = 0; max = -1;
            has_quantifier = true;
            break;
        case '+':
            advance(p);
            min = 1; max = -1;
            has_quantifier = true;
            break;
        case '?':
            advance(p);
            min = 0; max = 1;
            has_quantifier = true;
            break;
        case '{':
            advance(p);
            if (!parse_repeat_range(p, &min, &max)) {
                xr_regex_ast_free(atom);
                return NULL;
            }
            has_quantifier = true;
            break;
    }

    if (!has_quantifier) return atom;

    // Check non-greedy modifier
    bool greedy = true;
    if (match(p, '?')) {
        greedy = false;
    }

    // If global setting is non-greedy, invert
    if (p->flags & XR_RE_UNGREEDY) {
        greedy = !greedy;
    }

    return ast_repeat(&p->ast_arena, atom, min, max, greedy);
}

// Parse concatenation sequence
static XrAstNode* parse_concat(XrParser *p) {
    XrArena *arena = &p->ast_arena;
    XrAstNode *concat = ast_concat(arena);

    while (!at_end(p) && peek(p) != '|' && peek(p) != ')') {
        XrAstNode *item = parse_quantified(p);
        if (!item) {
            if (p->error != XR_RE_OK) {
                xr_regex_ast_free(concat);
                return NULL;
            }
            break;  // No more atoms
        }
        ast_list_add(arena, concat, item);
    }

    // Simplify: if only one child, return child itself
    if (concat->list.count == 0) {
        xr_regex_ast_free(concat);
        return ast_new(arena, XR_AST_EMPTY);
    }
    if (concat->list.count == 1) {
        XrAstNode *child = concat->list.children[0];
        concat->list.children[0] = NULL;
        concat->list.count = 0;
        xr_regex_ast_free(concat);
        return child;
    }

    return concat;
}

// Parse alternation expression a|b|c
static XrAstNode* parse_expr(XrParser *p) {
    XrAstNode *left = parse_concat(p);
    if (!left) return NULL;

    if (peek(p) != '|') return left;

    XrArena *arena = &p->ast_arena;
    XrAstNode *alt = ast_alt(arena);
    ast_list_add(arena, alt, left);

    while (match(p, '|')) {
        XrAstNode *right = parse_concat(p);
        if (!right) {
            if (p->error == XR_RE_OK) {
                // Empty alternation branch
                right = ast_new(arena, XR_AST_EMPTY);
            } else {
                xr_regex_ast_free(alt);
                return NULL;
            }
        }
        ast_list_add(arena, alt, right);
    }

    // Simplify: if only one branch
    if (alt->list.count == 1) {
        XrAstNode *child = alt->list.children[0];
        alt->list.children[0] = NULL;
        alt->list.count = 0;
        xr_regex_ast_free(alt);
        return child;
    }

    return alt;
}

/* ========================================================================
 * Public Interface
 * ======================================================================== */

XrAstNode* xr_regex_parse(const char *pattern, XrRegexFlags flags, XrParser *parser) {
    memset(parser, 0, sizeof(XrParser));
    parser->pattern = pattern;
    parser->p = pattern;
    parser->end = pattern + strlen(pattern);
    parser->flags = flags;
    parser->capture_index = 0;
    parser->capture_names = NULL;
    parser->names_count = 0;
    parser->names_cap = 0;
    parser->error = XR_RE_OK;

    // Initialize AST Arena (4KB is enough for most regexes)
    xr_arena_init(&parser->ast_arena, 4096);

    XrAstNode *ast = parse_expr(parser);

    if (!ast && parser->error == XR_RE_OK) {
        // Empty pattern
        ast = ast_new(&parser->ast_arena, XR_AST_EMPTY);
    }

    if (ast && !at_end(parser)) {
        parser_error(parser, XR_RE_ERR_SYNTAX, "unexpected character");
        ast = NULL;
    }

    return ast;
}

/* ========================================================================
 * AST Debug Output
 * ======================================================================== */

static void print_indent(int indent) {
    for (int i = 0; i < indent; i++) printf("  ");
}

void xr_regex_ast_dump(XrAstNode *node, int indent) {
    if (!node) {
        print_indent(indent);
        printf("(null)\n");
        return;
    }

    print_indent(indent);

    switch (node->type) {
        case XR_AST_EMPTY:
            printf("EMPTY\n");
            break;

        case XR_AST_LITERAL:
            printf("LITERAL \"%.*s\"\n", node->literal.len, node->literal.str);
            break;

        case XR_AST_CHAR_CLASS:
            printf("CHAR_CLASS%s [", node->cc.negated ? " (negated)" : "");
            for (int i = 0; i < node->cc.count; i++) {
                if (i > 0) printf(", ");
                if (node->cc.ranges[i].lo == node->cc.ranges[i].hi) {
                    printf("%d", node->cc.ranges[i].lo);
                } else {
                    printf("%d-%d", node->cc.ranges[i].lo, node->cc.ranges[i].hi);
                }
            }
            printf("]\n");
            break;

        case XR_AST_ANY:
            printf("ANY%s\n", node->any_dotall ? " (dotall)" : "");
            break;

        case XR_AST_CONCAT:
            printf("CONCAT\n");
            for (int i = 0; i < node->list.count; i++) {
                xr_regex_ast_dump(node->list.children[i], indent + 1);
            }
            break;

        case XR_AST_ALT:
            printf("ALT\n");
            for (int i = 0; i < node->list.count; i++) {
                xr_regex_ast_dump(node->list.children[i], indent + 1);
            }
            break;

        case XR_AST_REPEAT:
            printf("REPEAT {%d,%d} %s\n",
                   node->repeat.min,
                   node->repeat.max,
                   node->repeat.greedy ? "greedy" : "lazy");
            xr_regex_ast_dump(node->repeat.child, indent + 1);
            break;

        case XR_AST_CAPTURE:
            printf("CAPTURE %d", node->capture.index);
            if (node->capture.name) {
                printf(" <%s>", node->capture.name);
            }
            printf("\n");
            xr_regex_ast_dump(node->capture.child, indent + 1);
            break;

        case XR_AST_EMPTY_MATCH:
            printf("EMPTY_MATCH flags=0x%x\n", node->empty_flags);
            break;
    }
}
