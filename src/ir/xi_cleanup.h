/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cleanup.h - Source-owned static cleanup boundary identities
 *
 * KEY CONCEPT:
 *   Boundary identities belong to an emitted lexical frontier, not to a
 *   source statement or executor. Cloning must remap their complete graph.
 */

#ifndef XI_CLEANUP_H
#define XI_CLEANUP_H

#include "xi.h"

#include <stddef.h>
#include <stdint.h>

struct XgGlobalEvidence;
struct XrVMRuntime;

/* Query the declared edge of an already-verified function. */
XR_FUNC bool xi_value_has_panic_continuation(const XiFunc *function, const XiValue *point);

/* A malformed graph or allocation failure conservatively reports an active
 * region. Callers must never bypass a handler when its extent is uncertain. */
XR_FUNC bool xi_try_region_reaches_point(const XiFunc *function, const XiValue *registration,
                                          const XiValue *point);

/* Expose call and primitive panic results as typed Xi continuations
 * after import and call resolution. Explicit ownership frontiers precede the
 * failure points, and cloned lexical cleanup retains its ordered obligations. */
XR_FUNC bool xi_normalize_panic_exits(XiFunc *root, const struct XgGlobalEvidence *evidence,
                                      struct XrVMRuntime *isolate, char *error,
                                      size_t error_size);

typedef enum XiCleanupBoundaryKind {
    XI_CLEANUP_BOUNDARY_INVALID = 0,
    XI_CLEANUP_BOUNDARY_CLOSED = 1,
    XI_CLEANUP_BOUNDARY_FATAL = 2,
} XiCleanupBoundaryKind;

/* One arena-owned record is shared by its enter and optional leave markers.
 * Rank counts this item and the remaining items in this emitted frontier.
 * A NULL remaining ends only this frontier; outer lexical obligations are
 * represented separately. FATAL records deliberately have no normal leave;
 * they neither catch nor change a cleanup body's fatal language outcome. */
typedef struct XiCleanupBoundary {
    XiValue *enter;
    XiValue *leave;
    XiValue *remaining;
    XiValue *frontier;
    uint32_t rank;
    uint8_t kind;
    uint8_t reserved[3];
} XiCleanupBoundary;

typedef struct XiCleanupValueMapping {
    const XiValue *source;
    XiValue *target;
} XiCleanupValueMapping;

typedef struct XiCleanupRemap {
    const XiCleanupValueMapping *values;
    uint32_t count;
} XiCleanupRemap;

/* Attach a copied record after all of its marker identities exist. Records
 * may be attached in any order; verify the complete frontier before use.
 * This validates identity relations, not a cleanup body's CFG or effects. */
XR_FUNC bool xi_cleanup_boundary_attach(XiFunc *function, const XiCleanupBoundary *boundary,
                                        char *error, size_t error_size);

/* Validate every live marker, pair, frontier and decreasing-rank relation.
 * Arena membership is checked before reading a record, and live instruction
 * membership before dereferencing any marker named by that record. */
XR_FUNC bool xi_cleanup_verify(const XiFunc *function, char *error, size_t error_size);

/* The source subset must contain complete frontiers and marker pairs. Each
 * relation requires an explicit mapping, including forward references; there
 * is no fallback to a source pointer. Targets must be fresh marker instances.
 * Allocation and validation finish before any target attachment is published.
 * The caller owns the surrounding Xi edit session and evidence invalidation. */
XR_FUNC bool xi_cleanup_remap(XiFunc *target, const XiFunc *source, const XiCleanupRemap *remap,
                              char *error, size_t error_size);

#endif  // XI_CLEANUP_H
