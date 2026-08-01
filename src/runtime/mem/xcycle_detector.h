/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcycle_detector.h - Development-mode reference-cycle detector.
 *
 * Xray does not reclaim reference cycles (spec 16.8). `weak` breaks them and
 * the coroutine heap bounds the leak, but neither helps you FIND one. This
 * reports them: it walks the live heap, identifies dead cycles, prints the
 * objects and the edges that close each one, and exits non-zero.
 *
 * It is NOT a collector. It frees nothing and changes no program behaviour
 * beyond the exit code.
 *
 * VM only, and compiled out by default. There is no runtime flag and no
 * stdlib entry point — the production binary does not contain it. Enable with
 * -DXR_ENABLE_CYCLE_DETECTOR=ON; `xray run --detect-cycles` and `xray test`
 * drive it from there.
 *
 * AOT deliberately has no equivalent. After task 247 phase E both backends are
 * plain RC with trivially equivalent semantics (enforced by the differential
 * suite), so a cycle found on the VM is a cycle on AOT. AOT also never
 * maintains objsize (xrt_bump_header_init zeroes it), which whole-heap
 * traversal depends on.
 */

#ifndef XR_CYCLE_DETECTOR_H
#define XR_CYCLE_DETECTOR_H

#include <stdbool.h>
#include <stdint.h>

struct XrCoroHeap;
struct XrClass;

#ifdef XR_ENABLE_CYCLE_DETECTOR

/* Where the detector reports, and whether it has found anything. */
typedef struct XrCycleReport {
    uint32_t cycle_count;  /* distinct dead cycles found */
    uint32_t object_count; /* objects across all of them */
    uint64_t byte_count;   /* bytes those objects occupy */
    bool traversal_failed; /* a self-check tripped; counts are not meaningful */
} XrCycleReport;

/* Scan `heap` and report every dead cycle to stderr in both a human-readable
 * and a machine-readable form. Returns true when at least one was found.
 *
 * Call sites are ordered by usefulness: before a coroutine's heap is bulk-freed
 * (the leak the heap boundary would otherwise hide), and before the process
 * exits on the main coroutine. */
bool xr_cycle_detector_scan(struct XrCoroHeap *heap, XrCycleReport *out);

/* Count live objects by walking the heap, for the traversal self-check.
 *
 * Whole-block linear traversal is new code with no precedent in the tree, and
 * its failure mode is silent: a desync just starts reading neighbouring bytes
 * as headers. So the walk is validated against an independently maintained
 * number (heap->object_count) BEFORE any of the cycle logic is trusted.
 *
 * Returns false if a self-check tripped during the walk. */
bool xr_cycle_detector_count_live(struct XrCoroHeap *heap, uint32_t *out_count);

/* Process-wide accumulation, so `xray test` can fail the run once at the end
 * rather than per coroutine. */
void xr_cycle_detector_accumulate(const XrCycleReport *report);
bool xr_cycle_detector_any_found(void);
void xr_cycle_detector_reset(void);

/* Snapshot a cycle-candidate class's name at construction time.
 *
 * A class name is an interned symbol whose storage can outlive neither the
 * heap it was allocated on nor the symbol table — both of which may be gone by
 * the time a coroutine's heap is torn down and scanned. Reading cls->name then
 * yields whatever bytes are left there. Registering here, where the descriptor
 * still holds a valid name, is what lets a report name the types on a cycle.
 *
 * Only candidate classes are registered, so a program with no cyclic types
 * pays nothing. */
void xr_cycle_detector_register_class(const struct XrClass *cls, const char *name);

/* The snapshotted name for a class, or NULL if it was never registered. */
const char *xr_cycle_detector_class_name(const struct XrClass *cls);

#else /* !XR_ENABLE_CYCLE_DETECTOR */

/* No stub bodies, no no-op API: the symbols do not exist in a default build,
 * and `nm` is asserted on that. Call sites are guarded by the same macro. */

#endif /* XR_ENABLE_CYCLE_DETECTOR */

#endif /* XR_CYCLE_DETECTOR_H */
