/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xregex.h - Regular expression library public API
 *
 * KEY CONCEPT:
 *   Based on RE2 design principles:
 *   - Linear time complexity guarantee, prevents ReDoS attacks
 *   - Multi-engine architecture (DFA/NFA/OnePass), auto-selects optimal
 *   - No backtracking features (backreferences, lookahead/lookbehind)
 */

#ifndef XREGEX_H
#define XREGEX_H

#include "../../src/base/xdefs.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Type Definitions
 * ======================================================================== */

// Regex object (opaque pointer)
typedef struct XrRegex XrRegex;

// Match iterator (opaque pointer)
typedef struct XrMatchIter XrMatchIter;

// Compile flags
typedef enum {
    XR_RE_NONE = 0,
    XR_RE_IGNORECASE = 1 << 0,  // i: case-insensitive
    XR_RE_MULTILINE = 1 << 1,   // m: multiline mode (^ $ match each line)
    XR_RE_DOTALL = 1 << 2,      // s: . matches newline
    XR_RE_EXTENDED = 1 << 3,    // x: extended mode (ignore whitespace and comments)
    XR_RE_UNICODE = 1 << 4,     // u: Unicode mode
    XR_RE_UNGREEDY = 1 << 5,    // U: default non-greedy
} XrRegexFlags;

// Error codes
typedef enum {
    XR_RE_OK = 0,
    XR_RE_ERR_SYNTAX,             // syntax error
    XR_RE_ERR_UNMATCHED_PAREN,    // unmatched parenthesis
    XR_RE_ERR_UNMATCHED_BRACKET,  // unmatched bracket
    XR_RE_ERR_INVALID_ESCAPE,     // invalid escape sequence
    XR_RE_ERR_INVALID_RANGE,      // invalid character range [z-a]
    XR_RE_ERR_INVALID_REPEAT,     // invalid repeat {5,3}
    XR_RE_ERR_MISSING_OPERAND,    // missing operand
    XR_RE_ERR_TOO_COMPLEX,        // pattern too complex
    XR_RE_ERR_TOO_MANY_CAPTURES,  // too many capture groups
    XR_RE_ERR_INVALID_GROUP,      // invalid group syntax
    XR_RE_ERR_NOMEM,              // out of memory
} XrRegexError;

// Single capture group
typedef struct {
    const char *start;  // start position (NULL if not matched)
    const char *end;    // end position
} XrCapture;

/*
 * Match result
 * groups[0] is full match, groups[1..n] are capture groups
 */
#define XR_RE_MAX_CAPTURES 32

typedef struct {
    int group_count;  // actual capture group count
    XrCapture groups[XR_RE_MAX_CAPTURES];
} XrMatch;

/* ========================================================================
 * Compile and Free
 * ======================================================================== */

/*
 * Compile regular expression
 * @param pattern   regex pattern (UTF-8 string)
 * @param flags     compile flags
 * @param error     output error code (can be NULL)
 * @return regex object on success, NULL on failure
 */
XR_FUNC XrRegex *xr_regex_compile(const char *pattern, XrRegexFlags flags, XrRegexError *error);

/*
 * Compile regular expression (with detailed error info)
 * @param pattern   regex pattern
 * @param flags     compile flags
 * @param error     output error code
 * @param error_pos output error position (character offset)
 * @param error_msg output error message buffer
 * @param msg_size  buffer size
 * @return regex object on success, NULL on failure
 */
XR_FUNC XrRegex *xr_regex_compile_ex(const char *pattern, XrRegexFlags flags, XrRegexError *error,
                             int *error_pos, char *error_msg, size_t msg_size);

// Free regex object
XR_FUNC void xr_regex_free(XrRegex *re);

/* ========================================================================
 * Property Query
 * ======================================================================== */

// Get original pattern string
XR_FUNC const char *xr_regex_pattern(const XrRegex *re);

// Get capture group count (excluding full match)
XR_FUNC int xr_regex_capture_count(const XrRegex *re);

/*
 * Get index of named capture group
 * @return group index (1-based), -1 if not found
 */
XR_FUNC int xr_regex_named_group(const XrRegex *re, const char *name);

/*
 * Get name of capture group
 * @param index group index (1-based)
 * @return name string, NULL if unnamed
 */
XR_FUNC const char *xr_regex_group_name(const XrRegex *re, int index);

/* ========================================================================
 * Match Operations
 * ======================================================================== */

/*
 * Test if matches (fastest, no position returned)
 * @param re    regex object
 * @param text  text to match
 * @param len   text length (-1 for strlen)
 * @return whether it matches
 */
XR_FUNC bool xr_regex_test(const XrRegex *re, const char *text, int len);

/*
 * Find first match
 * @param re    regex object
 * @param text  text to match
 * @param len   text length (-1 for strlen)
 * @param match output match result
 * @return whether match found
 */
XR_FUNC bool xr_regex_match(const XrRegex *re, const char *text, int len, XrMatch *match);

/*
 * Find from specified position
 * @param start_pos start position (byte offset)
 */
XR_FUNC bool xr_regex_match_at(const XrRegex *re, const char *text, int len, int start_pos, XrMatch *match);

// Full match (entire text must match pattern)
XR_FUNC bool xr_regex_full_match(const XrRegex *re, const char *text, int len, XrMatch *match);

/* ========================================================================
 * Iterator (find all matches)
 * ======================================================================== */

// Create match iterator
XR_FUNC XrMatchIter *xr_regex_iter_new(const XrRegex *re, const char *text, int len);

/*
 * Get next match
 * @return whether there are more matches
 */
XR_FUNC bool xr_regex_iter_next(XrMatchIter *iter, XrMatch *match);

// Reset iterator to start position
XR_FUNC void xr_regex_iter_reset(XrMatchIter *iter);

// Free iterator
XR_FUNC void xr_regex_iter_free(XrMatchIter *iter);

/* ========================================================================
 * Batch Find
 * ======================================================================== */

/*
 * Count matches (efficient, no substring allocation)
 * @param re   regex object
 * @param text text to match
 * @param len  text length (-1 for strlen)
 * @return match count
 */
XR_FUNC int xr_regex_count(const XrRegex *re, const char *text, int len);

/*
 * Find all matches (returns array)
 * @param re        regex object
 * @param text      text to match
 * @param len       text length (-1 for strlen)
 * @param limit     max return count, -1 for unlimited
 * @param out_count output actual match count
 * @return match array (must call xr_regex_find_all_free to free)
 */
XR_FUNC XrMatch *xr_regex_find_all(const XrRegex *re, const char *text, int len, int limit, int *out_count);

// Free array returned by xr_regex_find_all
XR_FUNC void xr_regex_find_all_free(XrMatch *matches);

/* ========================================================================
 * Replace Operations
 * ======================================================================== */

/*
 * Replace matches (single-pass, dynamic allocation)
 * @param re          regex object
 * @param text        original text
 * @param len         text length (-1 for strlen)
 * @param replacement replacement string (supports $0, $1, ${name})
 * @param all         true = replace all, false = replace first
 * @return newly allocated string, caller must xr_re_free()
 */
XR_FUNC char *xr_regex_replace_alloc(const XrRegex *re, const char *text, int len, const char *replacement,
                             bool all);

/* ========================================================================
 * Split Operations
 * ======================================================================== */

// Split result
typedef struct {
    const char *str;  // split segment start
    int len;          // segment length
} XrSplitPart;

/*
 * Split string by regex
 * @param re        regex object
 * @param text      original text
 * @param len       text length
 * @param parts     output split result array
 * @param max_parts max array capacity
 * @param limit     max split count (-1 for unlimited)
 * @return actual split count
 */
XR_FUNC int xr_regex_split(const XrRegex *re, const char *text, int len, XrSplitPart *parts, int max_parts,
                   int limit);

/* ========================================================================
 * Utility Functions
 * ======================================================================== */

/*
 * Escape special characters
 * @param text    original text
 * @param len     text length (-1 for strlen)
 * @param out     output buffer
 * @param out_size buffer size
 * @return result length, -1 if buffer insufficient
 */
XR_FUNC int xr_regex_escape(const char *text, int len, char *out, size_t out_size);

// Validate if pattern is valid
XR_FUNC bool xr_regex_is_valid(const char *pattern, XrRegexFlags flags);

// Get error description
XR_FUNC const char *xr_regex_error_str(XrRegexError error);

/* ========================================================================
 * Debug
 * ======================================================================== */

// Print compiled program (for debugging)
XR_FUNC void xr_regex_dump(const XrRegex *re);

#ifdef __cplusplus
}
#endif

#endif  // XREGEX_H
