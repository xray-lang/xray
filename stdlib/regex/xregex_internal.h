/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xregex_internal.h - Regex library internal data structures
 *
 * KEY CONCEPT:
 *   Defines compiled program, instructions, AST and other internal structures.
 *   For internal library use only, not exposed externally.
 */

#ifndef XREGEX_INTERNAL_H
#define XREGEX_INTERNAL_H

#include "xregex.h"
#include "../../src/base/xarena.h"
#include "../../src/base/xposix_compat.h"  // memmem shim for MSVC
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

/* ========================================================================
 * Configuration Constants
 * ======================================================================== */

#define XR_RE_MAX_PROG_INST 10000       // max instruction count
#define XR_RE_MAX_PROG_SIZE (1 << 20)   // max program memory 1MB
#define XR_RE_DFA_MEM_BUDGET (8 << 20)  // DFA cache budget 8MB
#define XR_RE_MAX_REPEAT 1000           // max repeat count {n,m}
#define XR_RE_MAX_NESTED_DEPTH 100      // max nesting depth

/* ========================================================================
 * Memory Allocation Macros
 * ======================================================================== */

#include "../../src/base/xmalloc.h"

#define xr_re_alloc(size) xr_malloc(size)
#define xr_re_realloc(p, size) xr_realloc(p, size)
#define xr_re_free(p) xr_free(p)
#define xr_re_strdup(s) xr_strdup(s)

/* ========================================================================
 * UTF-8 Helpers
 * ======================================================================== */

// Return byte length of UTF-8 character at p (1-4), or 1 if invalid
static inline int xr_re_utf8_charlen(const char *p, const char *end) {
    if (p >= end)
        return 1;
    uint8_t b = (uint8_t) *p;
    if (b < 0x80)
        return 1;
    if ((b & 0xE0) == 0xC0 && p + 1 < end)
        return 2;
    if ((b & 0xF0) == 0xE0 && p + 2 < end)
        return 3;
    if ((b & 0xF8) == 0xF0 && p + 3 < end)
        return 4;
    return 1;
}

/* ========================================================================
 * Instruction Opcodes
 * ======================================================================== */

typedef enum {
    XR_OP_NOP = 0,        // no-op, jump to out
    XR_OP_MATCH,          // match success
    XR_OP_FAIL,           // match failure
    XR_OP_BYTE,           // match single byte
    XR_OP_BYTE_RANGE,     // match byte range [lo, hi]
    XR_OP_ANY_BYTE,       // match any byte (except newline)
    XR_OP_ANY_BYTE_NL,    // match any byte (including newline)
    XR_OP_ALT,            // branch: out or out1
    XR_OP_CAPTURE,        // capture: record position to cap
    XR_OP_EMPTY_WIDTH,    // zero-width assertion
    XR_OP_UNICODE_RANGE,  // match Unicode codepoint range [lo, hi]
} XrOpcode;

// Zero-width assertion types (for XR_OP_EMPTY_WIDTH)
typedef enum {
    XR_EMPTY_BEGIN_LINE = 1 << 0,      // ^ line start
    XR_EMPTY_END_LINE = 1 << 1,        // $ line end
    XR_EMPTY_BEGIN_TEXT = 1 << 2,      // \A text start
    XR_EMPTY_END_TEXT = 1 << 3,        // \z text end
    XR_EMPTY_WORD_BOUNDARY = 1 << 4,   // \b word boundary
    XR_EMPTY_NOT_WORD_BOUND = 1 << 5,  // \B non-word boundary
} XrEmptyWidth;

/* ========================================================================
 * Program Instruction (8 bytes)
 * ======================================================================== */

/*
 * Single instruction structure
 *
 * Layout:
 *   [31:5] out   - next instruction index (27 bits, max 2^27-1)
 *   [4]    last  - whether last in list
 *   [3:0]  op    - opcode (4 bits, max 16 opcodes)
 *   [63:32] arg  - operand (meaning depends on op)
 */
typedef struct {
    uint32_t out_op;  // 27b out + 1b last + 4b opcode
    union {
        uint32_t out1;  // ALT: other branch
        struct {
            uint8_t lo;
            uint8_t hi;
            uint16_t _pad;
        };  // BYTE_RANGE: [lo, hi]
        uint8_t byte;          // BYTE: single byte
        int32_t cap;           // CAPTURE: capture index (even=start, odd=end)
        uint32_t empty;        // EMPTY_WIDTH: assertion type mask
        uint32_t unicode_idx;  // UNICODE_RANGE: index to unicode_ranges table
    };
} XrInst;

// Instruction field access macros
// Layout: [31:5] out | [4] last | [3:0] opcode
// Supports up to 16 opcodes, out max 2^27-1
#define XR_INST_OP(inst) ((inst)->out_op & 0xF)
#define XR_INST_OUT(inst) ((inst)->out_op >> 5)
#define XR_INST_LAST(inst) (((inst)->out_op >> 4) & 0x1)

#define XR_INST_SET(inst, op_, out_, last_)                                                        \
    ((inst)->out_op = ((out_) << 5) | ((last_) << 4) | (op_))

/* ========================================================================
 * Compiled Program
 * ======================================================================== */

// Unicode property (for XR_OP_UNICODE_RANGE)
typedef struct {
    uint32_t prop_id;  // Unicode property ID (XrUnicodeProperty)
    bool negated;      // whether negated
} XrProgUnicodeRange;

typedef struct XrProg {
    XrInst *inst;    // instruction array
    int inst_count;  // instruction count
    int inst_cap;    // array capacity

    int start;             // start instruction (anchored mode)
    int start_unanchored;  // start instruction (unanchored, with .* prefix)

    // ByteMap: compress 256 bytes into fewer byte classes
    uint8_t bytemap[256];
    int bytemap_range;  // byte class count

    // Unicode range table (for \p{...})
    XrProgUnicodeRange *unicode_ranges;
    int unicode_range_count;
    int unicode_range_cap;

    // Capture groups
    int capture_count;     // capture group count (excluding full match)
    char **capture_names;  // named capture group name array

    // Optimization info
    bool is_anchored;      // whether anchored at start
    bool is_onepass;       // whether OnePass mode
    bool is_literal;       // whether pure literal (use strstr directly)
    bool has_empty_width;  // whether has EMPTY_WIDTH instructions (DFA unsafe)
    char *prefix;          // fixed prefix (accelerate search)
    int prefix_len;
    char *literal;  // pure literal content (valid when is_literal)
    int literal_len;

    // DFA cache (simplified: cache recent state transitions)
    struct {
        uint64_t state_hash;  // NFA state set hash
        int *next_pcs;        // next state PC list
        int next_count;       // next state count
        int byte;             // transition byte
    } *dfa_cache;
    int dfa_cache_count;
    int dfa_cache_cap;

    // Flags
    XrRegexFlags flags;
} XrProg;

/* ========================================================================
 * AST Node Types
 * ======================================================================== */

typedef enum {
    XR_AST_EMPTY,        // empty match
    XR_AST_LITERAL,      // literal character
    XR_AST_CHAR_CLASS,   // character class [abc]
    XR_AST_ANY,          // any character .
    XR_AST_CONCAT,       // concatenation AB
    XR_AST_ALT,          // alternation A|B
    XR_AST_REPEAT,       // repetition A{n,m}
    XR_AST_CAPTURE,      // capture (A)
    XR_AST_EMPTY_MATCH,  // zero-width assertion
} XrAstType;

// Character range (single byte)
typedef struct {
    uint8_t lo;
    uint8_t hi;
} XrCharRange;

// Unicode character range (32-bit codepoint)
typedef struct {
    uint32_t lo;
    uint32_t hi;
} XrUnicodeCharRange;

// AST node
typedef struct XrAstNode {
    XrAstType type;
    union {
        // LITERAL
        struct {
            char *str;
            int len;
        } literal;

        // CHAR_CLASS
        struct {
            XrCharRange *ranges;  // single byte ranges (ASCII)
            int count;
            int cap;
            bool negated;
            // Unicode property (multi-byte)
            uint32_t unicode_prop;  // Unicode property ID
            bool has_unicode_prop;  // whether has Unicode property
        } cc;

        // CONCAT, ALT
        struct {
            struct XrAstNode **children;
            int count;
            int cap;
        } list;

        // REPEAT
        struct {
            struct XrAstNode *child;
            int min;
            int max;  // -1 means unlimited
            bool greedy;
        } repeat;

        // CAPTURE
        struct {
            struct XrAstNode *child;
            int index;   // capture group index
            char *name;  // named capture group (can be NULL)
        } capture;

        // EMPTY_MATCH
        XrEmptyWidth empty_flags;

        // ANY
        bool any_dotall;  // whether matches newline
    };
} XrAstNode;

/* ========================================================================
 * Parser State
 * ======================================================================== */

typedef struct {
    const char *pattern;  // original pattern
    const char *p;        // current position
    const char *end;      // end position
    XrRegexFlags flags;

    int capture_index;     // next capture group index
    char **capture_names;  // named capture groups
    int names_count;
    int names_cap;

    XrRegexError error;
    int error_pos;
    char error_msg[256];

    XrArena ast_arena;  // AST node allocator
} XrParser;

/* ========================================================================
 * SparseSet - O(1) clearing set
 * ======================================================================== */

typedef struct {
    int *sparse;
    int *dense;
    int size;
    int capacity;
} XrSparseSet;

static inline void xr_sparse_set_init(XrSparseSet *s, int capacity) {
    s->sparse = (int *) xr_re_alloc(capacity * sizeof(int));
    s->dense = (int *) xr_re_alloc(capacity * sizeof(int));
    // Briggs/Torczon sparse-set normally leaves sparse[] uninitialised
    // and relies on the contains() guard `sparse[i] < size` to filter
    // out stray entries. That trick is correct, but MSan reports the
    // read as use-of-uninitialized-value because it cannot follow the
    // data-flow invariant. Zeroing sparse[] keeps the algorithm intact
    // (sparse[i] = 0 still satisfies `0 < size` => the dense check
    // dense[0] == i decides membership exactly when i == dense[0],
    // which is the same as before any insert wrote sparse[i]). Dense
    // remains uninitialised: it is only read at sparse[i] which is
    // either 0 (handled by size guard) or was set by a prior insert
    // that also wrote dense[size].
    memset(s->sparse, 0, capacity * sizeof(int));
    s->size = 0;
    s->capacity = capacity;
}

static inline void xr_sparse_set_free(XrSparseSet *s) {
    xr_re_free(s->sparse);
    xr_re_free(s->dense);
    s->sparse = s->dense = NULL;
}

static inline void xr_sparse_set_clear(XrSparseSet *s) {
    s->size = 0;
}

static inline bool xr_sparse_set_contains(XrSparseSet *s, int i) {
    return (unsigned) i < (unsigned) s->capacity && (unsigned) s->sparse[i] < (unsigned) s->size &&
           s->dense[s->sparse[i]] == i;
}

static inline bool xr_sparse_set_insert(XrSparseSet *s, int i) {
    if (xr_sparse_set_contains(s, i))
        return false;
    s->sparse[i] = s->size;
    s->dense[s->size++] = i;
    return true;
}

/* ========================================================================
 * DFA State
 * ======================================================================== */

#define XR_DFA_FLAG_MATCH 0x0100
#define XR_DFA_DEAD_STATE ((XrDFAState *) 1)
#define XR_DFA_FULL_MATCH ((XrDFAState *) 2)

typedef struct XrDFAState {
    int *inst;  // NFA state set
    int ninst;
    uint32_t flag;
    struct XrDFAState **next;  // transition table [bytemap_range]
} XrDFAState;

typedef struct XrDFA {
    XrProg *prog;
    XrDFAState **state_cache;  // state hash table
    int cache_size;
    int64_t mem_budget;
    int64_t mem_used;
    XrDFAState *start_state;
    XrSparseSet q[2];  // work queues
} XrDFA;

/* ========================================================================
 * Complete Regex Structure
 * ======================================================================== */

struct XrRegex {
    char *pattern;  // original pattern
    XrRegexFlags flags;
    XrProg *prog;
    XrDFA *dfa;  // DFA cache (created on demand)
};

/* ========================================================================
 * Iterator Structure
 * ======================================================================== */

struct XrMatchIter {
    const XrRegex *re;
    const char *text;
    int len;
    int pos;  // current search position
    bool done;
};

/* ========================================================================
 * Internal Function Declarations
 * ======================================================================== */

// Parser
XR_FUNC XrAstNode *xr_regex_parse(const char *pattern, XrRegexFlags flags, XrParser *parser);
XR_FUNC void xr_regex_ast_dump(XrAstNode *node, int indent);

// Compiler
XR_FUNC XrProg *xr_regex_compile_prog(XrAstNode *ast, XrRegexFlags flags);
XR_FUNC void xr_prog_free(XrProg *prog);
XR_FUNC void xr_prog_dump(XrProg *prog);

// ByteMap
XR_FUNC void xr_prog_compute_bytemap(XrProg *prog);

// Zero-width assertion check (shared by NFA and DFA)
XR_FUNC bool xr_re_check_empty_width(uint32_t flags, const char *text, const char *p,
                                     const char *end);

// Execution engine
XR_FUNC bool xr_nfa_match(XrProg *prog, const char *text, int len, const char **captures,
                          int ncaptures);
XR_FUNC bool xr_nfa_search(XrProg *prog, const char *text, int len, const char **captures,
                           int ncaptures);
XR_FUNC bool xr_dfa_match(XrDFA *dfa, const char *text, int len, const char **match_end,
                          bool anchored);

// DFA
XR_FUNC XrDFA *xr_dfa_new(XrProg *prog);
XR_FUNC void xr_dfa_free(XrDFA *dfa);
XR_FUNC void xr_dfa_reset(XrDFA *dfa);
XR_FUNC int xr_dfa_search(XrDFA *dfa, const char *text, int len, const char **match_start,
                          const char **match_end);
XR_FUNC int xr_dfa_test(XrDFA *dfa, const char *text, int len);

// Optimization
XR_FUNC void xr_prog_optimize(XrProg *prog);
XR_FUNC bool xr_prog_is_onepass(XrProg *prog);

#endif  // XREGEX_INTERNAL_H
