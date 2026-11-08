/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_capture_complete.inc.c - Universal callable source evidence
 *
 * KEY CONCEPT:
 *   Complete sources use the same finite relation as origins. Each actual
 *   alternative is an obligation, and every dependency has a smaller rank.
 *   The receiving walk neither allocates nor reconstructs a flow solution.
 */
typedef struct EffectCaptureComplete {
    uint64_t rank;
    bool complete,admitted;
} EffectCaptureComplete;

/* A zero column names the value itself. Other columns name the unique actual
 * captured formal, even when its current descriptor contains no origin bits.
 * Empty columns need source evidence too: an unknown callback may hide a
 * nested environment and cannot prove absence by an empty physical row. */
typedef struct EffectCaptureCompleteTypes {
    XrXirType values[64];
    uint32_t count;
} EffectCaptureCompleteTypes;

/* These are current parameter type values, not a Complete verdict. The
 * uninterrupted owned-body read ends before any allocator or release callback;
 * the next open reacquires every type from the actual capture catalogue. */

typedef struct EffectCaptureCompleteCoordinate {
    uint32_t node,value,column;
} EffectCaptureCompleteCoordinate;
typedef struct EffectCaptureCompleteJobs {
    EffectCaptureCompleteCoordinate values[128];
    uint32_t count;
    bool overflow;
} EffectCaptureCompleteJobs;

/* Every source loan ends before a growing emit. In-place Complete writes
 * preserve the current body and origin projections used by the next job. */

/* Admission depends on the fixed origin/type domain, not on Complete facts.
 * Ordinals collected by the first full sweep select subsequent builder work;
 * every job recomputes its current rule and rank. Receiving still walks the
 * entire actual domain, independently of these constructor proposals. */

/* The view contains current Input identity only. In-place fact writes have no
 * external callback; a possible growth discards both views before emitting and
 * frames the actual Input and Final again afterwards. The caller receives no
 * retained stack address, on either success or failure. */

/* Only Complete flags change during this construction interval. Source
 * descriptors therefore remain current until a growing emit invokes a
 * callback; the walk drops every projection first and rebuilds afterwards. */
