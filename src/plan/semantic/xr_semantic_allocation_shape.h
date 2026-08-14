/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_allocation_shape.h - Shared proof for an operation's allocation identity
 *
 * KEY CONCEPT:
 *   A heap-allocating operation owns its result only if the plan can prove the
 *   allocation identity rather than take the builder's word for it. The proof
 *   is the same for every allocating opcode: the allocation key is the
 *   operation's own canonical key with one fixed suffix, and the stable id is
 *   that key's own digest. Rebuilding both here is what makes the result a
 *   proved fresh owner.
 *
 *   Every layer that has to answer this asks the one judgement below, so the
 *   semantic verifier, both target layers, the AOT refinement and the C
 *   emission plan cannot drift into similar-looking rules with different
 *   guards. Each layer still arrives here from its own records and does its own
 *   work with the answer.
 */

#ifndef XR_SEMANTIC_ALLOCATION_SHAPE_H
#define XR_SEMANTIC_ALLOCATION_SHAPE_H

#include "xr_semantic_plan.h"
#include "xr_semantic_ids.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The length guard is load bearing: canonical_length comes from a key in the
 * frozen plan, and without it the suffix arithmetic below can wrap and let a
 * malformed key satisfy the comparison it should fail. */
static inline bool xr_semantic_allocation_identity_is_canonical(
    const XrSemanticOperationRecord *operation) {
    static const char suffix[] = "/allocation";
    XrStableId expected;
    XrFingerprint digest;

    if (!operation || !operation->canonical_key || !operation->allocation_key)
        return false;

    size_t canonical_length = strlen(operation->canonical_key);
    size_t allocation_length = strlen(operation->allocation_key);
    if (canonical_length > SIZE_MAX - sizeof(suffix) ||
        allocation_length != canonical_length + sizeof(suffix) - 1u ||
        memcmp(operation->allocation_key, operation->canonical_key, canonical_length) != 0 ||
        memcmp(operation->allocation_key + canonical_length, suffix, sizeof(suffix)) != 0)
        return false;

    return xr_stable_id_from_key(operation->allocation_key, &expected, &digest) &&
           xr_stable_id_equal(expected, operation->allocation_id);
}

#endif  // XR_SEMANTIC_ALLOCATION_SHAPE_H
