/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xregex_dfa.c - DFA cache implementation
 *
 * KEY CONCEPT:
 *   Build DFA states on demand, cache state transitions, accelerate regex matching.
 *
 * CORE IDEAS:
 *   - DFA state = NFA state set (a group of PC values)
 *   - State transition: given DFA state + input byte -> next DFA state
 *   - Cache computed state transitions to avoid repeated computation
 */

#include "xregex_internal.h"
#include "../../src/base/xhash.h"
#include <string.h>
#include <stdlib.h>

// DFA configuration
#define DFA_CACHE_INIT_SIZE 1024          // initial hash table size
#define DFA_MEM_BUDGET (8 * 1024 * 1024)  // 8MB memory budget
#define DFA_MAX_STATES 10000              // max state count

// qsort comparison function
static int int_compare(const void *a, const void *b) {
    return (*(const int *) a) - (*(const int *) b);
}

/* ========================================================================
 * DFA State Hash
 * ======================================================================== */

// Compute hash value of NFA state set (uses unified xr_hash_bytes64)
static uint64_t state_hash(const int *inst, int ninst) {
    return xr_hash_bytes64(inst, ninst * sizeof(int));
}

// Compare if two NFA state sets are identical
static bool state_equal(const int *a, int na, const int *b, int nb) {
    if (na != nb)
        return false;
    return memcmp(a, b, na * sizeof(int)) == 0;
}

/* ========================================================================
 * DFA State Management
 * ======================================================================== */

// Create DFA state
static XrDFAState *dfa_state_new(XrDFA *dfa, const int *inst, int ninst, uint32_t flag) {
    size_t mem =
        sizeof(XrDFAState) + ninst * sizeof(int) + dfa->prog->bytemap_range * sizeof(XrDFAState *);

    if (dfa->mem_used + (int64_t) mem > dfa->mem_budget) {
        return NULL;  // memory limit exceeded
    }

    XrDFAState *state = (XrDFAState *) xr_re_alloc(sizeof(XrDFAState));
    if (!state)
        return NULL;
    state->ninst = ninst;
    state->flag = flag;
    state->inst = NULL;
    state->next = NULL;

    // Copy NFA state set
    state->inst = (int *) xr_re_alloc(ninst * sizeof(int));
    if (!state->inst) {
        xr_re_free(state);
        return NULL;
    }
    memcpy(state->inst, inst, ninst * sizeof(int));

    // Allocate transition table (initially all NULL, means not computed)
    int range = dfa->prog->bytemap_range;
    state->next = (XrDFAState **) xr_re_alloc(range * sizeof(XrDFAState *));
    if (!state->next) {
        xr_re_free(state->inst);
        xr_re_free(state);
        return NULL;
    }
    memset(state->next, 0, range * sizeof(XrDFAState *));

    dfa->mem_used += mem;
    return state;
}

// Free DFA state
static void dfa_state_free(XrDFAState *state) {
    if (!state || state == XR_DFA_DEAD_STATE || state == XR_DFA_FULL_MATCH)
        return;
    xr_re_free(state->inst);
    xr_re_free(state->next);
    xr_re_free(state);
}

// Lookup or insert state in cache
static XrDFAState *dfa_cache_lookup(XrDFA *dfa, const int *inst, int ninst, uint32_t flag) {
    uint64_t h = state_hash(inst, ninst);
    int idx = h % dfa->cache_size;

    // Linear probing
    for (int i = 0; i < dfa->cache_size; i++) {
        int probe = (idx + i) % dfa->cache_size;
        XrDFAState *state = dfa->state_cache[probe];

        if (state == NULL) {
            // Empty slot, insert new state
            state = dfa_state_new(dfa, inst, ninst, flag);
            if (state == NULL)
                return NULL;  // memory limit exceeded
            dfa->state_cache[probe] = state;
            return state;
        }

        if (state != XR_DFA_DEAD_STATE && state != XR_DFA_FULL_MATCH &&
            state_equal(state->inst, state->ninst, inst, ninst)) {
            // Found existing state
            return state;
        }
    }

    // Hash table full, cannot insert
    return NULL;
}

/* ========================================================================
 * NFA State Set Computation
 * ======================================================================== */

/*
 * Compute epsilon closure from NFA state set
 * Handle NOP, ALT, CAPTURE, EMPTY_WIDTH and other epsilon transitions
 */
static void compute_closure(XrProg *prog, XrSparseSet *q, const char *text, const char *p,
                            const char *end, bool *is_match) {
    *is_match = false;

    // Process epsilon transitions for all states in queue
    for (int i = 0; i < q->size; i++) {
        int pc = q->dense[i];
        if (pc < 0 || pc >= prog->inst_count)
            continue;

        XrInst *ip = &prog->inst[pc];
        XrOpcode op = XR_INST_OP(ip);

        switch (op) {
            case XR_OP_MATCH:
                *is_match = true;
                break;

            case XR_OP_NOP:
                if (!xr_sparse_set_contains(q, XR_INST_OUT(ip))) {
                    xr_sparse_set_insert(q, XR_INST_OUT(ip));
                }
                break;

            case XR_OP_ALT:
                if (!xr_sparse_set_contains(q, XR_INST_OUT(ip))) {
                    xr_sparse_set_insert(q, XR_INST_OUT(ip));
                }
                if (!xr_sparse_set_contains(q, ip->out1)) {
                    xr_sparse_set_insert(q, ip->out1);
                }
                break;

            case XR_OP_CAPTURE:
                // Ignore capture in DFA mode
                if (!xr_sparse_set_contains(q, XR_INST_OUT(ip))) {
                    xr_sparse_set_insert(q, XR_INST_OUT(ip));
                }
                break;

            case XR_OP_EMPTY_WIDTH: {
                if (xr_re_check_empty_width(ip->empty, text, p, end)) {
                    if (!xr_sparse_set_contains(q, XR_INST_OUT(ip))) {
                        xr_sparse_set_insert(q, XR_INST_OUT(ip));
                    }
                }
                break;
            }

            default:
                // Non-epsilon instruction, keep
                break;
        }
    }
}

/*
 * Compute state transition: given current state set and input byte, compute next state set
 */
static void step_nfa(XrProg *prog, XrSparseSet *curr, int byte_class, int c, XrSparseSet *next,
                     const char *text, const char *p, const char *end) {
    (void) byte_class;
    xr_sparse_set_clear(next);

    // Traverse current state set
    for (int i = 0; i < curr->size; i++) {
        int pc = curr->dense[i];
        if (pc < 0 || pc >= prog->inst_count)
            continue;

        XrInst *ip = &prog->inst[pc];
        XrOpcode op = XR_INST_OP(ip);

        switch (op) {
            case XR_OP_BYTE:
                if (c == ip->byte) {
                    xr_sparse_set_insert(next, XR_INST_OUT(ip));
                }
                break;

            case XR_OP_BYTE_RANGE:
                if (c >= ip->lo && c <= ip->hi) {
                    xr_sparse_set_insert(next, XR_INST_OUT(ip));
                }
                break;

            case XR_OP_ANY_BYTE:
                if (c >= 0 && c != '\n') {
                    xr_sparse_set_insert(next, XR_INST_OUT(ip));
                }
                break;

            case XR_OP_ANY_BYTE_NL:
                if (c >= 0) {
                    xr_sparse_set_insert(next, XR_INST_OUT(ip));
                }
                break;

            default:
                break;
        }
    }

    // Compute epsilon closure
    bool is_match;
    compute_closure(prog, next, text, p + 1, end, &is_match);
}

/* ========================================================================
 * DFA Public API
 * ======================================================================== */

// Create DFA
XR_FUNC XrDFA *xr_dfa_new(XrProg *prog) {
    XrDFA *dfa = (XrDFA *) xr_re_alloc(sizeof(XrDFA));
    if (!dfa)
        return NULL;
    memset(dfa, 0, sizeof(XrDFA));

    dfa->prog = prog;
    dfa->cache_size = DFA_CACHE_INIT_SIZE;
    dfa->mem_budget = DFA_MEM_BUDGET;
    dfa->mem_used = 0;

    // Allocate state cache
    dfa->state_cache = (XrDFAState **) xr_re_alloc(dfa->cache_size * sizeof(XrDFAState *));
    if (!dfa->state_cache) {
        xr_re_free(dfa);
        return NULL;
    }
    memset(dfa->state_cache, 0, dfa->cache_size * sizeof(XrDFAState *));

    // Initialize work queues
    xr_sparse_set_init(&dfa->q[0], prog->inst_count);
    xr_sparse_set_init(&dfa->q[1], prog->inst_count);

    dfa->mem_used += sizeof(XrDFA) + dfa->cache_size * sizeof(XrDFAState *);

    return dfa;
}

// Free DFA
XR_FUNC void xr_dfa_free(XrDFA *dfa) {
    if (!dfa)
        return;

    // Free all states
    for (int i = 0; i < dfa->cache_size; i++) {
        dfa_state_free(dfa->state_cache[i]);
    }
    xr_re_free(dfa->state_cache);

    xr_sparse_set_free(&dfa->q[0]);
    xr_sparse_set_free(&dfa->q[1]);

    xr_re_free(dfa);
}

// Get or create start state
static XrDFAState *dfa_start_state(XrDFA *dfa, const char *text, const char *p, const char *end) {
    if (dfa->start_state) {
        return dfa->start_state;
    }

    XrProg *prog = dfa->prog;
    XrSparseSet *q = &dfa->q[0];

    // Compute initial state set from start instruction
    xr_sparse_set_clear(q);
    xr_sparse_set_insert(q, prog->start);

    // Compute epsilon closure
    bool is_match;
    compute_closure(prog, q, text, p, end, &is_match);

    // Build sorted PC array
    int ninst = q->size;
    int *inst = (int *) xr_re_alloc(ninst * sizeof(int));
    if (!inst)
        return NULL;
    memcpy(inst, q->dense, ninst * sizeof(int));

    // Use qsort for sorting (faster than bubble sort)
    if (ninst > 1) {
        qsort(inst, ninst, sizeof(int), int_compare);
    }

    uint32_t flag = is_match ? XR_DFA_FLAG_MATCH : 0;
    dfa->start_state = dfa_cache_lookup(dfa, inst, ninst, flag);

    xr_re_free(inst);
    return dfa->start_state;
}

// Compute state transition
static XrDFAState *dfa_next_state(XrDFA *dfa, XrDFAState *state, int c, const char *text,
                                  const char *p, const char *end) {
    XrProg *prog = dfa->prog;
    int byte_class = prog->bytemap[(unsigned char) c];

    // Check cache
    if (state->next[byte_class] != NULL) {
        return state->next[byte_class];
    }

    // Compute next state from current state
    XrSparseSet *curr = &dfa->q[0];
    XrSparseSet *next = &dfa->q[1];

    // Load current state
    xr_sparse_set_clear(curr);
    for (int i = 0; i < state->ninst; i++) {
        xr_sparse_set_insert(curr, state->inst[i]);
    }

    // Compute transition
    step_nfa(prog, curr, byte_class, c, next, text, p, end);

    if (next->size == 0) {
        // Dead state
        state->next[byte_class] = XR_DFA_DEAD_STATE;
        return XR_DFA_DEAD_STATE;
    }

    // Build sorted PC array
    int ninst = next->size;
    int *inst = (int *) xr_re_alloc(ninst * sizeof(int));
    if (!inst)
        return NULL;
    memcpy(inst, next->dense, ninst * sizeof(int));

    // Use qsort for sorting (faster than bubble sort)
    if (ninst > 1) {
        qsort(inst, ninst, sizeof(int), int_compare);
    }

    // Check if this is a match state
    bool is_match = false;
    for (int i = 0; i < ninst; i++) {
        if (inst[i] >= 0 && inst[i] < prog->inst_count) {
            if (XR_INST_OP(&prog->inst[inst[i]]) == XR_OP_MATCH) {
                is_match = true;
                break;
            }
        }
    }

    uint32_t flag = is_match ? XR_DFA_FLAG_MATCH : 0;
    XrDFAState *next_state = dfa_cache_lookup(dfa, inst, ninst, flag);

    xr_re_free(inst);

    if (next_state == NULL) {
        // Memory limit exceeded, cannot create new state
        return NULL;
    }

    state->next[byte_class] = next_state;
    return next_state;
}

/*
 * Use DFA to search for match
 * Return: 1=match found, 0=no match, -1=DFA failed (need to fallback to NFA)
 */
XR_FUNC int xr_dfa_search(XrDFA *dfa, const char *text, int len, const char **match_start,
                  const char **match_end) {
    if (!dfa || len < 0)
        return -1;

    XrProg *prog = dfa->prog;
    const char *p = text;
    const char *end = text + len;

    // Prefix optimization
    if (prog->prefix && prog->prefix_len > 0) {
        p = (const char *) memmem(text, len, prog->prefix, prog->prefix_len);
        if (!p)
            return 0;  // no match
    }

    // Try match at each position
    while (p <= end) {
        XrDFAState *state = dfa_start_state(dfa, text, p, end);
        if (state == NULL)
            return -1;  // DFA failed

        const char *match_p = p;
        const char *curr_match_end = NULL;

        // If start state is already a match state
        if (state->flag & XR_DFA_FLAG_MATCH) {
            curr_match_end = match_p;
        }

        // Follow DFA transitions
        while (match_p < end) {
            int c = (unsigned char) *match_p;
            state = dfa_next_state(dfa, state, c, text, match_p, end);

            if (state == NULL)
                return -1;  // DFA failed
            if (state == XR_DFA_DEAD_STATE)
                break;  // dead state

            match_p++;

            if (state->flag & XR_DFA_FLAG_MATCH) {
                curr_match_end = match_p;
            }
        }

        if (curr_match_end != NULL) {
            // Found match
            if (match_start)
                *match_start = p;
            if (match_end)
                *match_end = curr_match_end;
            return 1;
        }

        // Advance to next position
        p++;
    }

    return 0;  // no match
}

/*
 * Use DFA to test if matches (no position info needed)
 * Return: 1=match, 0=no match, -1=DFA failed
 */
XR_FUNC int xr_dfa_test(XrDFA *dfa, const char *text, int len) {
    return xr_dfa_search(dfa, text, len, NULL, NULL);
}
