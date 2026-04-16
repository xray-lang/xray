/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xregex_nfa.c - NFA execution engine
 *
 * KEY CONCEPT:
 *   Uses Thompson algorithm to simulate NFA execution.
 *
 * FEATURES:
 *   - Linear time complexity O(n*m), n=text length, m=instruction count
 *   - Supports capture groups
 *   - Parallel tracking of all possible states
 *   - Supports Unicode character classes \p{...}
 */

#define _GNU_SOURCE // memmem
#include "xregex_internal.h"
#include "../../src/base/xunicode.h"
#include <string.h>

// UTF-8 decode: return decoded character count and codepoint
static int decode_utf8(const char *s, const char *end, uint32_t *out_cp) {
    if (s >= end) {
        *out_cp = 0;
        return 0;
    }
    
    uint8_t b0 = (uint8_t)*s;
    uint32_t cp;
    int len;
    
    if ((b0 & 0x80) == 0) {
        // ASCII
        *out_cp = b0;
        return 1;
    } else if ((b0 & 0xE0) == 0xC0) {
        // 2 bytes
        if (s + 1 >= end) goto invalid;
        uint8_t b1 = (uint8_t)s[1];
        if ((b1 & 0xC0) != 0x80) goto invalid;
        cp = ((b0 & 0x1F) << 6) | (b1 & 0x3F);
        len = 2;
    } else if ((b0 & 0xF0) == 0xE0) {
        // 3 bytes
        if (s + 2 >= end) goto invalid;
        uint8_t b1 = (uint8_t)s[1];
        uint8_t b2 = (uint8_t)s[2];
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) goto invalid;
        cp = ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
        len = 3;
    } else if ((b0 & 0xF8) == 0xF0) {
        // 4 bytes
        if (s + 3 >= end) goto invalid;
        uint8_t b1 = (uint8_t)s[1];
        uint8_t b2 = (uint8_t)s[2];
        uint8_t b3 = (uint8_t)s[3];
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) goto invalid;
        cp = ((b0 & 0x07) << 18) | ((b1 & 0x3F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F);
        len = 4;
    } else {
        goto invalid;
    }
    
    *out_cp = cp;
    return len;

invalid:
    *out_cp = 0xFFFD; // Replacement character
    return 1;
}

/* ========================================================================
 * NFA Thread
 * 
 * Each thread represents an execution path with current instruction and capture state.
 * ======================================================================== */

typedef struct {
    int pc;                             // current instruction index
    const char *p;                      // current text position
    const char *captures[XR_RE_MAX_CAPTURES * 2];  // capture positions
} XrThread;

// Thread list capacity
#define XR_THREAD_POOL_SIZE 256

typedef struct {
    XrThread *threads;
    int count;
    int cap;
} XrThreadList;

/* ========================================================================
 * NFA Execution Context (thread-local storage, avoid repeated allocation)
 * ======================================================================== */

typedef struct {
    XrThreadList runq;
    XrThreadList nextq;
    XrSparseSet visited;
    XrSparseSet fast_curr;   // for nfa_search_fast
    XrSparseSet fast_next;   // for nfa_search_fast
    int capacity;
    bool initialized;
} XrNFAContext;

// Thread-local NFA context
static __thread XrNFAContext g_nfa_ctx = {0};

// Get or initialize NFA context
static XrNFAContext* nfa_context_get(int inst_count) {
    if (!g_nfa_ctx.initialized || g_nfa_ctx.capacity < inst_count) {
        // Free old resources
        if (g_nfa_ctx.initialized) {
            xr_re_free(g_nfa_ctx.runq.threads);
            xr_re_free(g_nfa_ctx.nextq.threads);
            xr_sparse_set_free(&g_nfa_ctx.visited);
            xr_sparse_set_free(&g_nfa_ctx.fast_curr);
            xr_sparse_set_free(&g_nfa_ctx.fast_next);
        }
        
        // Allocate new resources
        int cap = inst_count > XR_THREAD_POOL_SIZE ? inst_count : XR_THREAD_POOL_SIZE;
        g_nfa_ctx.runq.threads = (XrThread*)xr_re_alloc(cap * sizeof(XrThread));
        g_nfa_ctx.runq.cap = cap;
        g_nfa_ctx.nextq.threads = (XrThread*)xr_re_alloc(cap * sizeof(XrThread));
        g_nfa_ctx.nextq.cap = cap;
        xr_sparse_set_init(&g_nfa_ctx.visited, inst_count);
        xr_sparse_set_init(&g_nfa_ctx.fast_curr, inst_count);
        xr_sparse_set_init(&g_nfa_ctx.fast_next, inst_count);
        g_nfa_ctx.capacity = inst_count;
        g_nfa_ctx.initialized = true;
    }
    
    // Reset counts
    g_nfa_ctx.runq.count = 0;
    g_nfa_ctx.nextq.count = 0;
    xr_sparse_set_clear(&g_nfa_ctx.visited);
    xr_sparse_set_clear(&g_nfa_ctx.fast_curr);
    xr_sparse_set_clear(&g_nfa_ctx.fast_next);
    
    return &g_nfa_ctx;
}

static void thread_list_clear(XrThreadList *list) {
    list->count = 0;
}

static XrThread* thread_list_add(XrThreadList *list) {
    if (list->count >= list->cap) {
        // Expand
        int new_cap = list->cap * 2;
        list->threads = (XrThread*)xr_re_realloc(list->threads, new_cap * sizeof(XrThread));
        list->cap = new_cap;
    }
    return &list->threads[list->count++];
}

/* ========================================================================
 * Zero-width Assertion Check
 * ======================================================================== */

static inline bool is_word_char(int c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '_';
}

/*
 * Check if zero-width assertion is satisfied
 * @param flags assertion flags
 * @param text  text start
 * @param p     current position
 * @param end   text end
 */
bool xr_re_check_empty_width(uint32_t flags, const char *text, 
                              const char *p, const char *end) {
    if (flags & XR_EMPTY_BEGIN_TEXT) {
        if (p != text) return false;
    }
    if (flags & XR_EMPTY_END_TEXT) {
        if (p != end) return false;
    }
    if (flags & XR_EMPTY_BEGIN_LINE) {
        if (p != text && p[-1] != '\n') return false;
    }
    if (flags & XR_EMPTY_END_LINE) {
        if (p != end && *p != '\n') return false;
    }
    if (flags & XR_EMPTY_WORD_BOUNDARY) {
        bool prev_word = (p > text) && is_word_char((unsigned char)p[-1]);
        bool curr_word = (p < end) && is_word_char((unsigned char)*p);
        if (prev_word == curr_word) return false;
    }
    if (flags & XR_EMPTY_NOT_WORD_BOUND) {
        bool prev_word = (p > text) && is_word_char((unsigned char)p[-1]);
        bool curr_word = (p < end) && is_word_char((unsigned char)*p);
        if (prev_word != curr_word) return false;
    }
    return true;
}

/* ========================================================================
 * Add Thread (handle epsilon transitions)
 * ======================================================================== */

/*
 * Add thread, recursively handle all epsilon transitions
 * @param list    target thread list
 * @param visited visited instructions (prevent cycles)
 * @param prog    program
 * @param pc      instruction index
 * @param caps    capture state
 * @param text    text start
 * @param p       current position
 * @param end     text end
 */
static void add_thread(XrThreadList *list, XrSparseSet *visited,
                       XrProg *prog, int pc, const char **caps,
                       const char *text, const char *p, const char *end,
                       int ncaps) {
    // Prevent duplicate visits
    if (xr_sparse_set_contains(visited, pc)) return;
    xr_sparse_set_insert(visited, pc);
    
    XrInst *ip = &prog->inst[pc];
    XrOpcode op = XR_INST_OP(ip);
    size_t caps_size = ncaps * sizeof(const char*);
    
    switch (op) {
        case XR_OP_NOP:
            add_thread(list, visited, prog, XR_INST_OUT(ip), caps, text, p, end, ncaps);
            break;
            
        case XR_OP_ALT:
            add_thread(list, visited, prog, XR_INST_OUT(ip), caps, text, p, end, ncaps);
            add_thread(list, visited, prog, ip->out1, caps, text, p, end, ncaps);
            break;
            
        case XR_OP_CAPTURE: {
            const char *new_caps[XR_RE_MAX_CAPTURES * 2];
            memcpy(new_caps, caps, caps_size);
            new_caps[ip->cap] = p;
            add_thread(list, visited, prog, XR_INST_OUT(ip), new_caps, text, p, end, ncaps);
            break;
        }
        
        case XR_OP_EMPTY_WIDTH:
            if (xr_re_check_empty_width(ip->empty, text, p, end)) {
                add_thread(list, visited, prog, XR_INST_OUT(ip), caps, text, p, end, ncaps);
            }
            break;
            
        default: {
            XrThread *t = thread_list_add(list);
            t->pc = pc;
            t->p = p;
            memcpy(t->captures, caps, caps_size);
            break;
        }
    }
}

/* ========================================================================
 * Prefix Fast Search
 * ======================================================================== */

// Fast search prefix (using memmem)
static const char* find_prefix(const char *text, int len, 
                                const char *prefix, int prefix_len) {
    if (prefix_len == 0) return text;
    if (prefix_len > len) return NULL;
    
    // Use memmem for fast search (10-100x faster than char-by-char)
    return (const char*)memmem(text, len, prefix, prefix_len);
}

/* ========================================================================
 * OnePass Fast Matching (single-thread execution for unambiguous regex)
 * 
 * For regex detected as OnePass, each byte has only one possible next step,
 * so can use simplified single-thread execution, avoiding multi-thread list.
 * ======================================================================== */

/*
 * OnePass single-thread matching (simplified, no capture group support)
 * @param prog       compiled regex program
 * @param text_start original text start (for assertion context)
 * @param p_start    current match start position
 * @param end        text end
 * @param end_pos    output match end position
 * @return true if match successful
 */
static bool onepass_match_simple(XrProg *prog, const char *text_start,
                                  const char *p_start, const char *end,
                                  const char **end_pos) {
    const char *text = text_start;
    const char *p = p_start;
    int pc = prog->start;
    
    while (pc >= 0 && pc < prog->inst_count) {
        XrInst *ip = &prog->inst[pc];
        XrOpcode op = XR_INST_OP(ip);
        
        switch (op) {
            case XR_OP_MATCH:
                // Match successful
                if (end_pos) *end_pos = p;
                return true;
                
            case XR_OP_BYTE:
                if (p < end && (unsigned char)*p == ip->byte) {
                    p++;
                    pc = XR_INST_OUT(ip);
                } else {
                    return false;
                }
                break;
                
            case XR_OP_BYTE_RANGE:
                if (p < end && (unsigned char)*p >= ip->lo && 
                    (unsigned char)*p <= ip->hi) {
                    p++;
                    pc = XR_INST_OUT(ip);
                } else {
                    return false;
                }
                break;
                
            case XR_OP_ANY_BYTE:
                if (p < end && *p != '\n') {
                    p++;
                    pc = XR_INST_OUT(ip);
                } else {
                    return false;
                }
                break;
                
            case XR_OP_ANY_BYTE_NL:
                if (p < end) {
                    p++;
                    pc = XR_INST_OUT(ip);
                } else {
                    return false;
                }
                break;
                
            case XR_OP_NOP:
                pc = XR_INST_OUT(ip);
                break;
                
            case XR_OP_CAPTURE:
                // OnePass simplified version skips capture
                pc = XR_INST_OUT(ip);
                break;
                
            case XR_OP_EMPTY_WIDTH:
                if (xr_re_check_empty_width(ip->empty, text, p, end)) {
                    pc = XR_INST_OUT(ip);
                } else {
                    return false;
                }
                break;
                
            case XR_OP_ALT:
                // OnePass should not have ALT, but if it does, try first branch
                pc = XR_INST_OUT(ip);
                break;
                
            case XR_OP_UNICODE_RANGE: {
                uint32_t unicode_cp;
                int char_len = decode_utf8(p, end, &unicode_cp);
                if (char_len > 0) {
                    uint32_t prop_idx = ip->unicode_idx;
                    XrProgUnicodeRange *prop = &prog->unicode_ranges[prop_idx];
                    bool in_prop = xr_unicode_is_property(unicode_cp, 
                                                          (XrUnicodeProperty)prop->prop_id);
                    bool match_ok = prop->negated ? !in_prop : in_prop;
                    if (match_ok) {
                        p += char_len;
                        pc = XR_INST_OUT(ip);
                    } else {
                        return false;
                    }
                } else {
                    return false;
                }
                break;
            }
                
            case XR_OP_FAIL:
            default:
                return false;
        }
    }
    
    return false;
}

// OnePass search (search for match in text)
static bool onepass_search(XrProg *prog, const char *text, int len,
                           const char **captures, int ncaptures) {
    const char *p = text;
    const char *end = text + len;
    const char *match_end = NULL;
    
    // Use prefix optimization
    if (prog->prefix && prog->prefix_len > 0) {
        p = find_prefix(text, len, prog->prefix, prog->prefix_len);
        if (!p) return false;
    }
    
    // Try each position
    while (p <= end) {
        if (onepass_match_simple(prog, text, p, end, &match_end)) {
            if (captures && ncaptures >= 2) {
                captures[0] = p;
                captures[1] = match_end;
            }
            return true;
        }
        
        // Advance to next character (UTF-8 aware)
        uint32_t cp;
        int char_len = decode_utf8(p, end, &cp);
        p += (char_len > 0) ? char_len : 1;
    }
    
    return false;
}

/* ========================================================================
 * NFA Match Main Loop
 * ======================================================================== */

bool xr_nfa_match(XrProg *prog, const char *text, int len,
                  const char **captures, int ncaptures) {
    if (len < 0) len = (int)strlen(text);
    
    const char *p = text;
    const char *end = text + len;
    
    // Use thread-local context (avoid repeated allocation)
    XrNFAContext *ctx = nfa_context_get(prog->inst_count);
    XrThreadList *runq = &ctx->runq;
    XrThreadList *nextq = &ctx->nextq;
    XrSparseSet *visited = &ctx->visited;
    
    // Clamp ncaptures to max
    if (ncaptures > XR_RE_MAX_CAPTURES * 2) ncaptures = XR_RE_MAX_CAPTURES * 2;
    size_t caps_size = ncaptures * sizeof(const char*);
    
    // Initial capture state
    const char *init_caps[XR_RE_MAX_CAPTURES * 2];
    memset(init_caps, 0, caps_size);
    init_caps[0] = text;  // Match start position
    
    // Best match
    const char *best_caps[XR_RE_MAX_CAPTURES * 2];
    memset(best_caps, 0, caps_size);
    bool matched = false;
    
    // Add initial thread from start position
    xr_sparse_set_clear(visited);
    add_thread(runq, visited, prog, prog->start, init_caps, text, p, end, ncaptures);
    
    // Main loop: standard Thompson NFA byte-by-byte advancement.
    // No min_p scan needed — all byte-level ops advance by 1.
    // UNICODE_RANGE threads that jump ahead are deferred until reached.
    for (p = text; p <= end && runq->count > 0; p++) {
        int c = (p < end) ? (unsigned char)*p : -1;
        
        thread_list_clear(nextq);
        xr_sparse_set_clear(visited);
        
        for (int i = 0; i < runq->count; i++) {
            XrThread *t = &runq->threads[i];
            
            // Deferred thread (from UNICODE_RANGE multi-byte advance)
            if (t->p != p) {
                XrThread *nt = thread_list_add(nextq);
                nt->pc = t->pc;
                nt->p = t->p;
                memcpy(nt->captures, t->captures, caps_size);
                continue;
            }
            
            XrInst *ip = &prog->inst[t->pc];
            XrOpcode op = XR_INST_OP(ip);
            
            switch (op) {
                case XR_OP_MATCH:
                    if (!matched || (prog->flags & XR_RE_UNGREEDY)) {
                        memcpy(best_caps, t->captures, caps_size);
                        best_caps[1] = p;
                        matched = true;
                        if (prog->flags & XR_RE_UNGREEDY) {
                            goto done;
                        }
                    } else {
                        memcpy(best_caps, t->captures, caps_size);
                        best_caps[1] = p;
                    }
                    break;
                    
                case XR_OP_BYTE:
                    if (c == ip->byte)
                        add_thread(nextq, visited, prog, XR_INST_OUT(ip),
                                   t->captures, text, p + 1, end, ncaptures);
                    break;
                    
                case XR_OP_BYTE_RANGE:
                    if (c >= ip->lo && c <= ip->hi)
                        add_thread(nextq, visited, prog, XR_INST_OUT(ip),
                                   t->captures, text, p + 1, end, ncaptures);
                    break;
                    
                case XR_OP_ANY_BYTE:
                    if (c >= 0 && c != '\n')
                        add_thread(nextq, visited, prog, XR_INST_OUT(ip),
                                   t->captures, text, p + 1, end, ncaptures);
                    break;
                    
                case XR_OP_ANY_BYTE_NL:
                    if (c >= 0)
                        add_thread(nextq, visited, prog, XR_INST_OUT(ip),
                                   t->captures, text, p + 1, end, ncaptures);
                    break;
                    
                case XR_OP_UNICODE_RANGE: {
                    uint32_t unicode_cp;
                    int char_len = decode_utf8(p, end, &unicode_cp);
                    if (char_len > 0) {
                        uint32_t prop_idx = ip->unicode_idx;
                        XrProgUnicodeRange *prop = &prog->unicode_ranges[prop_idx];
                        bool in_prop = xr_unicode_is_property(unicode_cp, 
                                                              (XrUnicodeProperty)prop->prop_id);
                        bool match_ok = prop->negated ? !in_prop : in_prop;
                        if (match_ok)
                            add_thread(nextq, visited, prog, XR_INST_OUT(ip),
                                       t->captures, text, p + char_len, end, ncaptures);
                    }
                    break;
                }
                    
                default:
                    break;
            }
        }
        
        if (matched && nextq->count == 0) break;
        
        // Swap queues
        XrThreadList *tmp = runq;
        runq = nextq;
        nextq = tmp;
    }

done:
    // Copy capture results
    if (matched && captures && ncaptures > 0) {
        int copy_count = ncaptures;
        if (copy_count > XR_RE_MAX_CAPTURES * 2) {
            copy_count = XR_RE_MAX_CAPTURES * 2;
        }
        memcpy(captures, best_caps, copy_count * sizeof(char*));
    }
    
    // Context reuse, don't free resources
    return matched;
}

/* ========================================================================
 * No-capture Fast Search
 * Only track PC set, don't track capture positions, faster
 * ======================================================================== */

// Add state to set (handle epsilon transitions)
static void add_state_fast(XrSparseSet *states, XrProg *prog, int pc,
                           const char *text, const char *p, const char *end) {
    if (xr_sparse_set_contains(states, pc)) return;
    xr_sparse_set_insert(states, pc);
    
    XrInst *ip = &prog->inst[pc];
    XrOpcode op = XR_INST_OP(ip);
    
    switch (op) {
        case XR_OP_NOP:
            add_state_fast(states, prog, XR_INST_OUT(ip), text, p, end);
            break;
        case XR_OP_ALT:
            add_state_fast(states, prog, XR_INST_OUT(ip), text, p, end);
            add_state_fast(states, prog, ip->out1, text, p, end);
            break;
        case XR_OP_CAPTURE:
            // Ignore capture, continue
            add_state_fast(states, prog, XR_INST_OUT(ip), text, p, end);
            break;
        case XR_OP_EMPTY_WIDTH:
            if (xr_re_check_empty_width(ip->empty, text, p, end)) {
                add_state_fast(states, prog, XR_INST_OUT(ip), text, p, end);
            }
            break;
        default:
            // Non-epsilon instruction, keep in set
            break;
    }
}

/*
 * No-capture fast search - only determine if match, don't extract position.
 * Only called when unicode_range_count == 0 (byte-oriented operations only).
 * Uses TLS context to avoid per-call malloc/free.
 */
static bool nfa_search_fast(XrProg *prog, const char *text, int len,
                            const char **match_start, const char **match_end) {
    const char *p = text;
    const char *end = text + len;
    
    // Prefix optimization
    if (prog->prefix && prog->prefix_len > 0) {
        p = find_prefix(text, len, prog->prefix, prog->prefix_len);
        if (!p) return false;
    }
    
    // Use TLS context (avoid repeated allocation)
    XrNFAContext *ctx = nfa_context_get(prog->inst_count);
    XrSparseSet *curr = &ctx->fast_curr;
    XrSparseSet *next = &ctx->fast_next;
    
    const char *found_start = NULL;
    const char *found_end = NULL;
    
    // Try match at each position
    while (p <= end) {
        // Add start state
        xr_sparse_set_clear(curr);
        add_state_fast(curr, prog, prog->start, text, p, end);
        
        const char *try_start = p;
        const char *try_p = p;
        
        // Simulate NFA execution (byte-oriented only)
        while (curr->size > 0 && try_p <= end) {
            int c = (try_p < end) ? (unsigned char)*try_p : -1;
            
            xr_sparse_set_clear(next);
            
            // Check if there's a match state
            for (int i = 0; i < curr->size; i++) {
                int pc = curr->dense[i];
                XrInst *ip = &prog->inst[pc];
                if (XR_INST_OP(ip) == XR_OP_MATCH) {
                    found_start = try_start;
                    found_end = try_p;
                }
            }
            
            if (c < 0) break;  // Reached end of text
            
            // State transition (byte-level operations only)
            for (int i = 0; i < curr->size; i++) {
                int pc = curr->dense[i];
                XrInst *ip = &prog->inst[pc];
                XrOpcode op = XR_INST_OP(ip);
                
                switch (op) {
                    case XR_OP_BYTE:
                        if (c == ip->byte) {
                            add_state_fast(next, prog, XR_INST_OUT(ip), text, try_p + 1, end);
                        }
                        break;
                    case XR_OP_BYTE_RANGE:
                        if (c >= ip->lo && c <= ip->hi) {
                            add_state_fast(next, prog, XR_INST_OUT(ip), text, try_p + 1, end);
                        }
                        break;
                    case XR_OP_ANY_BYTE:
                        if (c >= 0 && c != '\n') {
                            add_state_fast(next, prog, XR_INST_OUT(ip), text, try_p + 1, end);
                        }
                        break;
                    case XR_OP_ANY_BYTE_NL:
                        if (c >= 0) {
                            add_state_fast(next, prog, XR_INST_OUT(ip), text, try_p + 1, end);
                        }
                        break;
                    default:
                        break;
                }
            }
            
            // Swap
            XrSparseSet *tmp = curr;
            curr = next;
            next = tmp;
            try_p++;
        }
        
        // Check if final state matches
        for (int i = 0; i < curr->size; i++) {
            int pc = curr->dense[i];
            XrInst *ip = &prog->inst[pc];
            if (XR_INST_OP(ip) == XR_OP_MATCH) {
                found_start = try_start;
                found_end = try_p;
            }
        }
        
        if (found_start) {
            if (match_start) *match_start = found_start;
            if (match_end) *match_end = found_end;
            return true;
        }
        
        // Prefix skip optimization: jump to next prefix position
        if (prog->prefix && prog->prefix_len > 0 && p + 1 < end) {
            const char *next_prefix = find_prefix(p + 1, end - p - 1, 
                                                  prog->prefix, prog->prefix_len);
            if (next_prefix) {
                p = next_prefix;
            } else {
                break;
            }
        } else {
            p++;
        }
    }
    
    return false;
}

/* ========================================================================
 * Search Match (from any position)
 * ======================================================================== */

// Search for match in text (non-anchored)
bool xr_nfa_search(XrProg *prog, const char *text, int len,
                   const char **captures, int ncaptures) {
    if (len < 0) len = (int)strlen(text);
    
    const char *p = text;
    const char *end = text + len;
    
    // Optimization 0: literal fast path (use memmem directly)
    if (prog->is_literal && prog->literal_len > 0) {
        const char *found = (const char*)memmem(text, len, prog->literal, prog->literal_len);
        if (found) {
            if (captures && ncaptures >= 2) {
                captures[0] = found;
                captures[1] = found + prog->literal_len;
            }
            return true;
        }
        return false;
    }
    
    // Optimization 1: anchored mode only tries at position 0
    if (prog->is_anchored) {
        return xr_nfa_match(prog, text, len, captures, ncaptures);
    }
    
    // Optimization 2: OnePass fast path (unambiguous regex)
    // Skip OnePass when captures are needed (OnePass doesn't track captures)
    if (prog->is_onepass && (ncaptures <= 2 || prog->capture_count == 0)) {
        return onepass_search(prog, text, len, captures, ncaptures);
    }
    
    // Optimization 3: prefix fast search
    const char *search_start = text;
    if (prog->prefix && prog->prefix_len > 0) {
        search_start = find_prefix(text, len, prog->prefix, prog->prefix_len);
        if (!search_start) {
            // Prefix not found, cannot match
            return false;
        }
    }
    
    // Optimization 4: no-capture fast path
    // Skip fast path for Unicode properties (byte-oriented fast search doesn't handle UTF-8)
    // Skip fast path for non-greedy patterns (fast path doesn't respect ALT priority)
    if (prog->capture_count == 0 && prog->unicode_range_count == 0 &&
        !(prog->flags & XR_RE_UNGREEDY)) {
        const char *match_start = NULL;
        const char *match_end = NULL;
        bool found = nfa_search_fast(prog, text, len, &match_start, &match_end);
        if (found && captures && ncaptures >= 2) {
            captures[0] = match_start;
            captures[1] = match_end;
        }
        return found;
    }
    
    // Use thread-local context (avoid repeated allocation)
    XrNFAContext *ctx = nfa_context_get(prog->inst_count);
    XrThreadList *runq = &ctx->runq;
    XrThreadList *nextq = &ctx->nextq;
    XrSparseSet *visited = &ctx->visited;
    
    // Clamp ncaptures to max
    if (ncaptures > XR_RE_MAX_CAPTURES * 2) ncaptures = XR_RE_MAX_CAPTURES * 2;
    size_t caps_size = ncaptures * sizeof(const char*);
    
    const char *init_caps[XR_RE_MAX_CAPTURES * 2];
    memset(init_caps, 0, caps_size);
    
    const char *best_caps[XR_RE_MAX_CAPTURES * 2];
    memset(best_caps, 0, caps_size);
    bool matched = false;
    
    // Main loop: byte-by-byte advancement (standard Thompson NFA).
    // search_pos tracks next UTF-8 character boundary for seeding new attempts.
    // p advances by 1 byte each step for NFA state transitions.
    const char *search_pos = search_start;
    p = search_start;
    
    while (p <= end) {
        // Seed new match attempt when queue empty and no match yet
        if (!matched && runq->count == 0) {
            if (search_pos > end) break;
            p = search_pos;
            init_caps[0] = p;
            xr_sparse_set_clear(visited);
            add_thread(runq, visited, prog, prog->start, init_caps, text, p, end, ncaptures);
            // Advance search_pos to next UTF-8 character boundary
            if (search_pos < end) {
                uint32_t cp;
                int char_len = decode_utf8(search_pos, end, &cp);
                search_pos += (char_len > 0) ? char_len : 1;
            } else {
                search_pos = end + 1;
            }
        }
        
        if (runq->count == 0) { p++; continue; }
        
        int c = (p < end) ? (unsigned char)*p : -1;
        
        thread_list_clear(nextq);
        xr_sparse_set_clear(visited);
        
        for (int i = 0; i < runq->count; i++) {
            XrThread *t = &runq->threads[i];
            
            // Deferred thread (from UNICODE_RANGE multi-byte advance)
            if (t->p != p) {
                XrThread *nt = thread_list_add(nextq);
                nt->pc = t->pc;
                nt->p = t->p;
                memcpy(nt->captures, t->captures, caps_size);
                continue;
            }
            
            XrInst *ip = &prog->inst[t->pc];
            XrOpcode op = XR_INST_OP(ip);
            
            switch (op) {
                case XR_OP_MATCH:
                    if (!matched || (prog->flags & XR_RE_UNGREEDY)) {
                        memcpy(best_caps, t->captures, caps_size);
                        best_caps[1] = p;
                        matched = true;
                        if (prog->flags & XR_RE_UNGREEDY) {
                            goto find_done;
                        }
                    } else {
                        memcpy(best_caps, t->captures, caps_size);
                        best_caps[1] = p;
                    }
                    break;
                    
                case XR_OP_BYTE:
                    if (c == ip->byte)
                        add_thread(nextq, visited, prog, XR_INST_OUT(ip),
                                   t->captures, text, p + 1, end, ncaptures);
                    break;
                    
                case XR_OP_BYTE_RANGE:
                    if (c >= ip->lo && c <= ip->hi)
                        add_thread(nextq, visited, prog, XR_INST_OUT(ip),
                                   t->captures, text, p + 1, end, ncaptures);
                    break;
                    
                case XR_OP_ANY_BYTE:
                    if (c >= 0 && c != '\n')
                        add_thread(nextq, visited, prog, XR_INST_OUT(ip),
                                   t->captures, text, p + 1, end, ncaptures);
                    break;
                    
                case XR_OP_ANY_BYTE_NL:
                    if (c >= 0)
                        add_thread(nextq, visited, prog, XR_INST_OUT(ip),
                                   t->captures, text, p + 1, end, ncaptures);
                    break;
                    
                case XR_OP_UNICODE_RANGE: {
                    uint32_t unicode_cp;
                    int char_len = decode_utf8(p, end, &unicode_cp);
                    if (char_len > 0) {
                        uint32_t prop_idx = ip->unicode_idx;
                        XrProgUnicodeRange *prop = &prog->unicode_ranges[prop_idx];
                        bool in_prop = xr_unicode_is_property(unicode_cp, 
                                                              (XrUnicodeProperty)prop->prop_id);
                        bool match_ok = prop->negated ? !in_prop : in_prop;
                        if (match_ok)
                            add_thread(nextq, visited, prog, XR_INST_OUT(ip),
                                       t->captures, text, p + char_len, end, ncaptures);
                    }
                    break;
                }
                    
                default:
                    break;
            }
        }
        
        XrThreadList *tmp = runq;
        runq = nextq;
        nextq = tmp;
        
        if (runq->count == 0 && matched) break;
        
        p++;
    }

find_done:
    if (matched && captures && ncaptures > 0) {
        int copy_count = ncaptures;
        if (copy_count > XR_RE_MAX_CAPTURES * 2) {
            copy_count = XR_RE_MAX_CAPTURES * 2;
        }
        memcpy(captures, best_caps, copy_count * sizeof(char*));
    }
    
    // Context reuse, do not free resources
    return matched;
}
