/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_capture_edges.inc.c - Actual argument and return origin clauses
 *
 * KEY CONCEPT:
 *   Captured prefixes project the descriptor at the real call. Initial keys
 *   retain supported historical subsets; each actual frontier still requires
 *   one complete current key, including all residual and capture columns.
 */
typedef struct EffectCaptureActual {
    uint32_t node,value,first,kind,mask;
    uint64_t rank;
    bool captured,unbound,opaque,active;
} EffectCaptureActual;

struct EffectCaptureActualRead {
    uint32_t edges,offsets[65];
    EffectCaptureActual values[64];
    unsigned char known[64],origin_known[64];
    uint64_t origin_rank[64];
    bool origin_present[64];
};

/* Presence and rank are current origin data. No actual input row, universal
 * Complete column or frontier verdict is retained with this projection. */

/* The table holds current source coordinates and rank facts, never an edge
 * acceptance. Every initial-row, support and frontier consumer independently
 * checks its own required bits and equations against these projections.
 * First reads retain their original order; only successful data projections
 * are materialized, and no failed read or permission verdict is retained. */

/* Nested columns need one actual non-leaf origin as well as their own source
 * fact. Empty prefixes wait during construction and cannot close a real use. */

/* The known-descriptor bit proves that the actual captured Class column is
 * present. Its root and Unknown bits remain independent payload facts. */

/* Heads describe the current Input, not capture facts. Only an edge-local
 * constructor interval borrows them, and an emitting growth clears this loan
 * before it can invoke a callback. Later reads reopen the actual heads. */
