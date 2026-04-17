/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xregex.c - Regex library public API implementation
 *
 * KEY CONCEPT:
 *   Main entry point integrating parser, compiler and execution engine.
 */

#include "xregex_internal.h"

/* ========================================================================
 * Error Messages
 * ======================================================================== */

static const char* error_messages[] = {
    [XR_RE_OK]                  = "success",
    [XR_RE_ERR_SYNTAX]          = "syntax error",
    [XR_RE_ERR_UNMATCHED_PAREN] = "unmatched parenthesis",
    [XR_RE_ERR_UNMATCHED_BRACKET] = "unmatched bracket",
    [XR_RE_ERR_INVALID_ESCAPE]  = "invalid escape sequence",
    [XR_RE_ERR_INVALID_RANGE]   = "invalid character range",
    [XR_RE_ERR_INVALID_REPEAT]  = "invalid repeat quantifier",
    [XR_RE_ERR_MISSING_OPERAND] = "missing operand",
    [XR_RE_ERR_TOO_COMPLEX]     = "pattern too complex",
    [XR_RE_ERR_TOO_MANY_CAPTURES] = "too many capture groups",
    [XR_RE_ERR_INVALID_GROUP]   = "invalid group syntax",
    [XR_RE_ERR_NOMEM]           = "out of memory",
};

const char* xr_regex_error_str(XrRegexError error) {
    if (error >= 0 && error <= XR_RE_ERR_NOMEM) {
        return error_messages[error];
    }
    return "unknown error";
}

/* ========================================================================
 * Compile
 * ======================================================================== */

XrRegex* xr_regex_compile(const char *pattern, XrRegexFlags flags,
                          XrRegexError *error) {
    return xr_regex_compile_ex(pattern, flags, error, NULL, NULL, 0);
}

XrRegex* xr_regex_compile_ex(const char *pattern, XrRegexFlags flags,
                             XrRegexError *error, int *error_pos,
                             char *error_msg, size_t msg_size) {
    if (!pattern) {
        if (error) *error = XR_RE_ERR_SYNTAX;
        if (error_msg && msg_size > 0) {
            snprintf(error_msg, msg_size, "NULL pattern");
        }
        return NULL;
    }

    // 1. Parse
    XrParser parser;
    XrAstNode *ast = xr_regex_parse(pattern, flags, &parser);

    if (parser.error != XR_RE_OK) {
        if (error) *error = parser.error;
        if (error_pos) *error_pos = parser.error_pos;
        if (error_msg && msg_size > 0) {
            snprintf(error_msg, msg_size, "%s", parser.error_msg);
        }
        // Free AST arena
        xr_arena_destroy(&parser.ast_arena);
        // Free parser resources
        if (parser.capture_names) {
            for (int i = 0; i < parser.names_count; i++) {
                xr_re_free(parser.capture_names[i]);
            }
            xr_re_free(parser.capture_names);
        }
        return NULL;
    }

    // 2. Compile (using parsed flags, including inline flags like (?i))
    XrProg *prog = xr_regex_compile_prog(ast, parser.flags);

    // Free AST arena (free all AST nodes at once)
    xr_arena_destroy(&parser.ast_arena);

    if (!prog) {
        if (error) *error = XR_RE_ERR_NOMEM;
        if (error_msg && msg_size > 0) {
            snprintf(error_msg, msg_size, "compilation failed");
        }
        // Free parser resources
        if (parser.capture_names) {
            for (int i = 0; i < parser.names_count; i++) {
                xr_re_free(parser.capture_names[i]);
            }
            xr_re_free(parser.capture_names);
        }
        return NULL;
    }

    // Transfer capture group name ownership
    prog->capture_names = parser.capture_names;
    parser.capture_names = NULL;

    // 3. Create regex object
    XrRegex *re = (XrRegex*)xr_re_alloc(sizeof(XrRegex));
    re->pattern = xr_re_strdup(pattern);
    re->flags = flags;
    re->prog = prog;

    // 4. Create DFA if safe (no position-dependent assertions, no Unicode properties)
    bool dfa_safe = !prog->has_empty_width && prog->unicode_range_count == 0;
    re->dfa = dfa_safe ? xr_dfa_new(prog) : NULL;

    if (error) *error = XR_RE_OK;
    return re;
}

void xr_regex_free(XrRegex *re) {
    if (!re) return;

    xr_re_free(re->pattern);
    xr_prog_free(re->prog);
    if (re->dfa) {
        xr_dfa_free(re->dfa);
    }
    xr_re_free(re);
}

/* ========================================================================
 * Property Query
 * ======================================================================== */

const char* xr_regex_pattern(const XrRegex *re) {
    return re ? re->pattern : NULL;
}

int xr_regex_capture_count(const XrRegex *re) {
    return re ? re->prog->capture_count : 0;
}

int xr_regex_named_group(const XrRegex *re, const char *name) {
    if (!re || !name || !re->prog->capture_names) return -1;

    for (int i = 0; i < re->prog->capture_count; i++) {
        if (re->prog->capture_names[i] &&
            strcmp(re->prog->capture_names[i], name) == 0) {
            return i + 1;  // 1-based
        }
    }
    return -1;
}

const char* xr_regex_group_name(const XrRegex *re, int index) {
    if (!re || index < 1 || index > re->prog->capture_count) return NULL;
    if (!re->prog->capture_names) return NULL;
    return re->prog->capture_names[index - 1];
}

/* ========================================================================
 * Match Operations
 * ======================================================================== */

/**
 * Convert internal capture array to XrMatch structure
 */
static void captures_to_match(const char **caps, int cap_count, XrMatch *match) {
    match->group_count = cap_count + 1;  // Full match

    // groups[0] is full match
    match->groups[0].start = caps[0];
    match->groups[0].end = caps[1];

    // Handle named capture groups
    for (int i = 0; i < cap_count && i < XR_RE_MAX_CAPTURES - 1; i++) {
        match->groups[i + 1].start = caps[i * 2 + 2];
        match->groups[i + 1].end = caps[i * 2 + 3];
    }
}

bool xr_regex_test(const XrRegex *re, const char *text, int len) {
    if (!re || !text) return false;
    if (len < 0) len = (int)strlen(text);

    // Try DFA acceleration (created at compile time, thread-safe)
    if (re->dfa) {
        int result = xr_dfa_search(re->dfa, text, len, NULL, NULL);
        if (result >= 0) {
            return result == 1;
        }
        // result < 0: DFA failed, fall back to NFA
    }

    // Fall back to NFA
    const char *caps[XR_RE_MAX_CAPTURES * 2] = {0};
    caps[0] = text;
    caps[1] = NULL;

    return xr_nfa_search(re->prog, text, len, caps, 2);
}

bool xr_regex_match(const XrRegex *re, const char *text, int len, XrMatch *match) {
    return xr_regex_match_at(re, text, len, 0, match);
}

bool xr_regex_match_at(const XrRegex *re, const char *text, int len,
                       int start_pos, XrMatch *match) {
    if (!re || !text) return false;
    if (len < 0) len = (int)strlen(text);
    if (start_pos < 0 || start_pos > len) return false;

    int cap_count = re->prog->capture_count;
    int ncaps = (cap_count + 1) * 2;  // +1 for full match

    const char *caps[XR_RE_MAX_CAPTURES * 2];
    memset(caps, 0, sizeof(caps));

    bool found = xr_nfa_search(re->prog, text + start_pos, len - start_pos,
                                caps, ncaps);

    if (found && match) {
        captures_to_match(caps, cap_count, match);
    }

    return found;
}

bool xr_regex_full_match(const XrRegex *re, const char *text, int len,
                         XrMatch *match) {
    if (!re || !text) return false;
    if (len < 0) len = (int)strlen(text);

    int cap_count = re->prog->capture_count;
    int ncaps = (cap_count + 1) * 2;

    const char *caps[XR_RE_MAX_CAPTURES * 2];
    memset(caps, 0, sizeof(caps));

    // Use anchored match
    bool found = xr_nfa_match(re->prog, text, len, caps, ncaps);

    // Check if full match
    if (found) {
        if (caps[0] != text || caps[1] != text + len) {
            found = false;
        }
    }

    if (found && match) {
        captures_to_match(caps, cap_count, match);
    }

    return found;
}

/* ========================================================================
 * Iterator
 * ======================================================================== */

XrMatchIter* xr_regex_iter_new(const XrRegex *re, const char *text, int len) {
    if (!re || !text) return NULL;

    XrMatchIter *iter = (XrMatchIter*)xr_re_alloc(sizeof(XrMatchIter));
    iter->re = re;
    iter->text = text;
    iter->len = (len < 0) ? (int)strlen(text) : len;
    iter->pos = 0;
    iter->done = false;

    return iter;
}

bool xr_regex_iter_next(XrMatchIter *iter, XrMatch *match) {
    if (!iter || iter->done) return false;

    if (iter->pos > iter->len) {
        iter->done = true;
        return false;
    }

    XrMatch m;
    // xr_regex_match_at internally calls xr_nfa_search, searching the entire remaining text
    // If it returns false, there are no more matches, no need to continue trying
    if (xr_regex_match_at(iter->re, iter->text, iter->len, iter->pos, &m)) {
        // Found match
        if (match) *match = m;

        // Move position to match end
        int match_end = (int)(m.groups[0].end - iter->text);
        if (match_end == iter->pos) {
            // Advance one UTF-8 character on empty match to avoid infinite loop
            iter->pos += xr_re_utf8_charlen(iter->text + iter->pos, iter->text + iter->len);
        } else {
            iter->pos = match_end;
        }

        return true;
    }

    // No more matches
    iter->done = true;
    return false;
}

void xr_regex_iter_reset(XrMatchIter *iter) {
    if (iter) {
        iter->pos = 0;
        iter->done = false;
    }
}

void xr_regex_iter_free(XrMatchIter *iter) {
    if (!iter) return;
    xr_re_free(iter);
}

/* ========================================================================
 * Count and Batch Find
 * ======================================================================== */

/**
 * Count matches (prefer DFA, efficient without memory allocation)
 */
int xr_regex_count(const XrRegex *re, const char *text, int len) {
    if (!re || !text) return 0;
    if (len < 0) len = (int)strlen(text);

    // Use DFA if available (created at compile time)
    XrDFA *dfa = re->dfa;
    bool use_dfa = (dfa != NULL);

    int count = 0;
    const char *p = text;
    const char *end = text + len;

    while (p <= end) {
        const char *match_start = NULL;
        const char *match_end = NULL;
        bool found = false;

        if (use_dfa) {
            int result = xr_dfa_search(dfa, p, end - p, &match_start, &match_end);
            if (result > 0) {
                found = true;
            } else if (result < 0) {
                use_dfa = false;  // DFA failed, fall back to NFA
            }
        }

        if (!found && !use_dfa) {
            const char *caps[2] = {NULL, NULL};
            found = xr_nfa_search(re->prog, p, end - p, caps, 2);
            if (found) {
                match_start = caps[0];
                match_end = caps[1];
            }
        }

        if (!found) break;

        count++;

        // Handle empty match: advance by one UTF-8 character
        if (match_end == match_start) {
            p = match_start + xr_re_utf8_charlen(match_start, end);
        } else {
            p = match_end;
        }
    }

    return count;
}

/**
 * Find all matches (return array)
 * @param limit Maximum number of matches to return, -1 for no limit
 * @param out_count Output actual number of matches
 * @return Match array (caller must free)
 */
XrMatch* xr_regex_find_all(const XrRegex *re, const char *text, int len,
                            int limit, int *out_count) {
    if (!re || !text) {
        if (out_count) *out_count = 0;
        return NULL;
    }
    if (len < 0) len = (int)strlen(text);
    if (limit == 0) {
        if (out_count) *out_count = 0;
        return NULL;
    }

    // Initial capacity
    int capacity = 16;
    if (limit > 0 && limit < capacity) capacity = limit;

    XrMatch *matches = (XrMatch*)xr_re_alloc(capacity * sizeof(XrMatch));
    int count = 0;
    int pos = 0;

    while (pos <= len) {
        if (limit > 0 && count >= limit) break;

        XrMatch m;
        if (!xr_regex_match_at(re, text, len, pos, &m)) break;

        // Expand capacity
        if (count >= capacity) {
            capacity *= 2;
            if (limit > 0 && capacity > limit) capacity = limit;
            XR_REALLOC_OR_ABORT(matches,
                                (size_t)capacity * sizeof(XrMatch),
                                "regex find_all matches grow");
        }

        matches[count++] = m;

        // Calculate next position
        int match_end = (int)(m.groups[0].end - text);
        if (match_end == pos) {
            pos += xr_re_utf8_charlen(text + pos, text + len);
        } else {
            pos = match_end;
        }
    }

    if (out_count) *out_count = count;
    return matches;
}

/**
 * Free find_all return array
 */
void xr_regex_find_all_free(XrMatch *matches) {
    xr_re_free(matches);
}

/* ========================================================================
 * Dynamic Buffer (single-pass replace, no retry loops)
 * ======================================================================== */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} XrDynBuf;

static void dynbuf_init(XrDynBuf *buf, size_t initial_cap) {
    buf->cap = initial_cap < 64 ? 64 : initial_cap;
    buf->data = (char*)xr_re_alloc(buf->cap);
    buf->len = 0;
}

static inline void dynbuf_ensure(XrDynBuf *buf, size_t extra) {
    if (buf->len + extra > buf->cap) {
        while (buf->len + extra > buf->cap) buf->cap *= 2;
        XR_REALLOC_OR_ABORT(buf->data, buf->cap, "regex dynbuf grow");
    }
}

static void dynbuf_append(XrDynBuf *buf, const char *data, size_t n) {
    dynbuf_ensure(buf, n);
    memcpy(buf->data + buf->len, data, n);
    buf->len += n;
}

static void dynbuf_append_char(XrDynBuf *buf, char c) {
    dynbuf_ensure(buf, 1);
    buf->data[buf->len++] = c;
}

/* ========================================================================
 * Replace Operations
 * ======================================================================== */

/*
 * Expand $0, $1, ${name} references into dynamic buffer.
 * Never truncates — buffer grows as needed.
 */
static void expand_replacement(XrDynBuf *buf, const char *replacement,
                               const XrMatch *match, const XrRegex *re) {
    const char *p = replacement;

    while (*p) {
        if (*p == '$') {
            p++;
            if (*p == '$') {
                dynbuf_append_char(buf, '$');
                p++;
            } else if (*p >= '0' && *p <= '9') {
                int n = 0;
                while (*p >= '0' && *p <= '9') {
                    n = n * 10 + (*p - '0');
                    p++;
                }
                if (n < match->group_count && match->groups[n].start) {
                    int len = (int)(match->groups[n].end - match->groups[n].start);
                    dynbuf_append(buf, match->groups[n].start, len);
                }
            } else if (*p == '{') {
                p++;
                const char *name_start = p;
                while (*p && *p != '}') p++;
                if (*p == '}') {
                    int name_len = (int)(p - name_start);
                    char name[64];
                    if (name_len < 64) {
                        memcpy(name, name_start, name_len);
                        name[name_len] = '\0';
                        int idx = xr_regex_named_group(re, name);
                        if (idx > 0 && idx < match->group_count && match->groups[idx].start) {
                            int glen = (int)(match->groups[idx].end - match->groups[idx].start);
                            dynbuf_append(buf, match->groups[idx].start, glen);
                        }
                    }
                    p++;
                }
            } else {
                dynbuf_append_char(buf, '$');
            }
        } else {
            dynbuf_append_char(buf, *p++);
        }
    }
}

static bool is_simple_replacement(const char *replacement) {
    for (const char *p = replacement; *p; p++) {
        if (*p == '$') return false;
    }
    return true;
}

/*
 * Fast replace-all path: no capture groups, simple replacement.
 * Uses DFA/NFA directly instead of XrMatchIter.
 */
static char* replace_all_fast(const XrRegex *re, const char *text, int len,
                              const char *replacement, int rep_len) {
    XrProg *prog = re->prog;
    const char *p = text;
    const char *text_end = text + len;

    XrDynBuf buf;
    dynbuf_init(&buf, len + rep_len * 4 + 1);

    XrDFA *dfa = re->dfa;
    bool use_dfa = (dfa != NULL);

    while (p < text_end) {
        const char *match_start = NULL;
        const char *match_end = NULL;
        bool found = false;

        if (use_dfa) {
            int result = xr_dfa_search(dfa, p, text_end - p, &match_start, &match_end);
            if (result > 0) {
                found = true;
            } else if (result < 0) {
                use_dfa = false;
            }
        }

        if (!found && !use_dfa) {
            const char *captures[2] = {NULL, NULL};
            found = xr_nfa_search(prog, p, text_end - p, captures, 2);
            if (found) {
                match_start = captures[0];
                match_end = captures[1];
            }
        }

        if (!found) {
            dynbuf_append(&buf, p, text_end - p);
            break;
        }

        dynbuf_append(&buf, p, match_start - p);
        dynbuf_append(&buf, replacement, rep_len);

        // Handle empty match
        if (match_end == match_start) {
            if (p < text_end) {
                int clen = xr_re_utf8_charlen(p, text_end);
                dynbuf_append(&buf, p, clen);
            }
            p = match_start + xr_re_utf8_charlen(match_start, text_end);
        } else {
            p = match_end;
        }
    }

    dynbuf_append_char(&buf, '\0');
    return buf.data;
}

char* xr_regex_replace_alloc(const XrRegex *re, const char *text, int len,
                             const char *replacement, bool all) {
    if (!re || !text || !replacement) return NULL;
    if (len < 0) len = (int)strlen(text);

    int rep_len = (int)strlen(replacement);

    // Fast path for replace_all with simple replacement (no captures)
    if (all && re->prog->capture_count == 0 && is_simple_replacement(replacement)) {
        return replace_all_fast(re, text, len, replacement, rep_len);
    }

    XrDynBuf buf;
    dynbuf_init(&buf, len + rep_len + 1);

    if (!all) {
        // Replace first match only
        XrMatch match;
        if (!xr_regex_match(re, text, len, &match)) {
            dynbuf_append(&buf, text, len);
            dynbuf_append_char(&buf, '\0');
            return buf.data;
        }

        dynbuf_append(&buf, text, match.groups[0].start - text);
        expand_replacement(&buf, replacement, &match, re);
        dynbuf_append(&buf, match.groups[0].end, text + len - match.groups[0].end);
        dynbuf_append_char(&buf, '\0');
        return buf.data;
    }

    // Replace all matches
    XrMatchIter *iter = xr_regex_iter_new(re, text, len);
    if (!iter) {
        xr_re_free(buf.data);
        return NULL;
    }

    const char *p = text;
    XrMatch match;
    while (xr_regex_iter_next(iter, &match)) {
        dynbuf_append(&buf, p, match.groups[0].start - p);
        expand_replacement(&buf, replacement, &match, re);
        p = match.groups[0].end;
    }

    xr_regex_iter_free(iter);
    dynbuf_append(&buf, p, text + len - p);
    dynbuf_append_char(&buf, '\0');
    return buf.data;
}

/* ========================================================================
 * Split
 * ======================================================================== */

int xr_regex_split(const XrRegex *re, const char *text, int len,
                   XrSplitPart *parts, int max_parts, int limit) {
    if (!re || !text || !parts || max_parts <= 0) return 0;
    if (len < 0) len = (int)strlen(text);
    if (limit == 0) limit = -1;

    int count = 0;
    const char *p = text;
    const char *end = text + len;

    XrMatchIter *iter = xr_regex_iter_new(re, text, len);
    if (!iter) {
        // Cannot create iterator, return entire string
        parts[0].str = text;
        parts[0].len = len;
        return 1;
    }

    XrMatch match;
    while (xr_regex_iter_next(iter, &match)) {
        if (limit > 0 && count >= limit - 1) break;
        if (count >= max_parts) break;

        // Add separator prefix
        parts[count].str = p;
        parts[count].len = (int)(match.groups[0].start - p);
        count++;

        p = match.groups[0].end;
    }

    xr_regex_iter_free(iter);

    // Add remaining part
    if (count < max_parts && p <= end) {
        parts[count].str = p;
        parts[count].len = (int)(end - p);
        count++;
    }

    return count;
}

/* ========================================================================
 * Utility Functions
 * ======================================================================== */

int xr_regex_escape(const char *text, int len, char *out, size_t out_size) {
    if (!text || !out || out_size == 0) return -1;
    if (len < 0) len = (int)strlen(text);

    const char *p = text;
    const char *end = text + len;
    char *o = out;
    char *oend = out + out_size - 1;

    while (p < end && o < oend) {
        char c = *p++;
        // Escape special characters
        if (strchr("\\.+*?[](){}|^$", c)) {
            if (o + 1 >= oend) return -1;
            *o++ = '\\';
        }
        *o++ = c;
    }

    *o = '\0';
    return (int)(o - out);
}

bool xr_regex_is_valid(const char *pattern, XrRegexFlags flags) {
    if (!pattern) return false;
    // Only parse, no need for full compilation
    XrParser parser;
    XrAstNode *ast = xr_regex_parse(pattern, flags, &parser);
    bool valid = (ast != NULL && parser.error == XR_RE_OK);
    xr_arena_destroy(&parser.ast_arena);
    if (parser.capture_names) {
        for (int i = 0; i < parser.names_count; i++) {
            xr_re_free(parser.capture_names[i]);
        }
        xr_re_free(parser.capture_names);
    }
    return valid;
}

/* ========================================================================
 * Debug
 * ======================================================================== */

void xr_regex_dump(const XrRegex *re) {
    if (!re) {
        printf("(null regex)\n");
        return;
    }

    printf("=== Regex ===\n");
    printf("Pattern: %s\n", re->pattern);
    printf("Flags: 0x%x\n", re->flags);
    printf("Captures: %d\n", re->prog->capture_count);

    if (re->prog->capture_names) {
        for (int i = 0; i < re->prog->capture_count; i++) {
            if (re->prog->capture_names[i]) {
                printf("  Group %d: %s\n", i + 1, re->prog->capture_names[i]);
            }
        }
    }

    printf("\n");
    xr_prog_dump(re->prog);
}
