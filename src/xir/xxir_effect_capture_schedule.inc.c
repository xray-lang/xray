/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_capture_schedule.inc.c - Constructor clause scheduling
 *
 * KEY CONCEPT:
 *   Pending ordinals select repeated construction work, never permission facts.
 *   The positive domain starts with every node and edge. Changes revisit the local
 *   node and each incident edge; the independent receiver reads no queue.
 */
struct EffectCaptureSchedule {
    uint32_t nodes,edges,capacity,front,back,count;
    uint32_t *queue;
    uint64_t *incident_offsets;
    uint64_t *change_epochs;
    uint64_t epoch;
    uint32_t *incident_edges;
    unsigned char *pending;
};

/* Local value, reference and Cell clauses read only their node's facts,
 * including its independent initial rows. Cross-node reads belong to edges
 * at their current caller and target; every affected clause gets a new job. */

/* Exact-row Class gates can change as facts grow. Preserve each full sweep's
 * ordinal order; only a previously successful clause with unchanged local
 * dependencies may be omitted. A job dirtied during or after its visit stays
 * pending for the next sweep, including the job that emitted the change. */
