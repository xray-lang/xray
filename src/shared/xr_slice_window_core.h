/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_slice_window_core.h - Canonical strict contiguous-window semantics.
 *
 * One statement of what xi.slice.window means. The operation carries a promise
 * the other slicing forms do not: it never clamps, it never reads a negative
 * bound as an offset from the end, and one successful check proves that every
 * access inside [0, count) of the result is valid. The kernel owns exactly the
 * three answers that promise reduces to:
 *
 *   1. Admissibility. Given the source length, the requested start and count,
 *      and whether the source has storage, is the window inside the source.
 *      Storage is part of the question, not a separate integrity probe: a
 *      window with a positive count whose source holds no address cannot keep
 *      the promise, so it is out of bounds like any other unreachable element.
 *
 *   2. Which operand is at fault, so every profile names the same one when it
 *      reports the failure in its own channel.
 *
 *   3. Whether the derived view advances its base. An empty window keeps the
 *      source address, which is what makes a zero-length window over an empty
 *      source well defined without inventing a pointer.
 *
 * Producing the operands and publishing the failure stay with the consumer:
 * only the backend knows how to read a slice header, how wide an element is,
 * and whether an out-of-bounds window becomes a VM runtime error, a hosted
 * throw or a no-libc trap. Nothing about the answer is restated on the way
 * through.
 */

#ifndef XR_SLICE_WINDOW_CORE_H
#define XR_SLICE_WINDOW_CORE_H

#if !defined(XR_SLICE_WINDOW_C90)
#include "xr_semantic_owner_ids_gen.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define XR_SLICE_WINDOW_INLINE static inline
#else
/* Restricted C90 provides bool, the fixed-width integers and NULL before
 * including this core, and states the rule without the owner guards. */
#define XR_SLICE_WINDOW_INLINE static
#endif

/* What the caller has already proven about the window. A proof is a promise the
 * plan discharged, so it selects a strictly narrower rule:
 *   NONE    nothing proven; admissibility belongs to the kernel
 *   BOUNDS  the plan proved the window admissible against this exact source,
 *           so only the derivation is left */
#define XR_SLICE_WINDOW_PROOF_NONE 0
#define XR_SLICE_WINDOW_PROOF_BOUNDS 1

/* Admissibility, as one expression over the four facts a window depends on.
 * Kept a macro so consumers that must place the answer inside generated C - an
 * optimizer assumption, a branch condition - evaluate the owner rather than a
 * copy of it.
 *
 * A start no further than the length is implied rather than restated: a
 * non-negative count that fits in (length - start) already proves the start is
 * inside, and both terms of that difference are non-negative by the time it is
 * evaluated, so it cannot wrap. */
#define XR_SLICE_WINDOW_ADMITS(length, start, count, data)                                         \
    ((length) >= 0 && (start) >= 0 && (count) >= 0 && (count) <= (length) - (start) &&             \
     ((count) <= 0 || (data) != NULL))

/* The operand a rejected window names. A start outside [0, length] is the
 * operand at fault; otherwise the count is what did not fit. */
#define XR_SLICE_WINDOW_FAULT_OPERAND(length, start, count)                                        \
    (((start) < 0 || (start) > (length)) ? (start) : (count))

/* An empty window keeps the source address instead of forming one past a base
 * it may not have. */
#define XR_SLICE_WINDOW_ADVANCES(count) ((count) > 0)

typedef struct XrSliceWindowPlan {
    bool admitted;
    bool advances;
    int64_t fault_operand;
    int64_t byte_offset;
    int64_t length;
} XrSliceWindowPlan;

/* Full rule including the admissibility probe the backend must publish. */
XR_SLICE_WINDOW_INLINE XrSliceWindowPlan xr_slice_window_eval(int64_t length, int64_t start,
                                                              int64_t count, const void *data,
                                                              int64_t element_size, int proof) {
    XrSliceWindowPlan plan;
    plan.admitted = proof == XR_SLICE_WINDOW_PROOF_BOUNDS ||
                    XR_SLICE_WINDOW_ADMITS(length, start, count, data);
    plan.advances = false;
    plan.fault_operand = 0;
    plan.byte_offset = 0;
    plan.length = 0;
    if (!plan.admitted) {
        plan.fault_operand = XR_SLICE_WINDOW_FAULT_OPERAND(length, start, count);
        return plan;
    }
    plan.advances = XR_SLICE_WINDOW_ADVANCES(count);
    plan.byte_offset = plan.advances ? start * element_size : 0;
    plan.length = count;
    return plan;
}

#if !defined(XR_SLICE_WINDOW_C90)
#define XR_SLICE_WINDOW_OWNER_GUARD(owner_hi, owner_lo)                                            \
    ((void) sizeof(struct {                                                                        \
        unsigned int owner_id_must_be_shared_slice_window                                          \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_SLICE_WINDOW_HI &&                 \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_SLICE_WINDOW_LO)                   \
                   ? 1                                                                             \
                   : -1);                                                                          \
    }))

#define XR_SLICE_WINDOW_CONSUMER_GUARD(consumer_bit)                                               \
    ((void) sizeof(struct {                                                                        \
        unsigned int consumer_must_be_declared_for_shared_slice_window                             \
            : (((uint32_t) (consumer_bit) != 0 &&                                                  \
                (((uint32_t) (consumer_bit) & ((uint32_t) (consumer_bit) - 1)) == 0) &&            \
                (XR_SEM_OWNER_ID_SHARED_SLICE_WINDOW_CONSUMERS & (uint32_t) (consumer_bit)) != 0)  \
                   ? 1                                                                             \
                   : -1);                                                                          \
    }))

#define XR_SLICE_WINDOW_PROOF_GUARD(proof)                                                         \
    ((void) sizeof(struct {                                                                        \
        unsigned int proof_must_be_a_declared_slice_window_proof                                   \
            : (((proof) == XR_SLICE_WINDOW_PROOF_NONE ||                                           \
                (proof) == XR_SLICE_WINDOW_PROOF_BOUNDS)                                           \
                   ? 1                                                                             \
                   : -1);                                                                          \
    }))

/* Skipping the admissibility probe requires a proof the plan discharged.
 * Emitting an unchecked window without one is a compile-time failure rather
 * than a silent unchecked view. */
#define XR_SLICE_WINDOW_PROVEN_GUARD(proof)                                                        \
    ((void) sizeof(struct {                                                                        \
        unsigned int window_proof_must_be_discharged                                               \
            : (((proof) == XR_SLICE_WINDOW_PROOF_BOUNDS) ? 1 : -1);                                \
    }))

#define XR_SLICE_WINDOW_OWNER_APPLY(owner_hi, owner_lo, consumer_bit, proof, length, start, count, \
                                    data, element_size)                                            \
    (XR_SLICE_WINDOW_OWNER_GUARD((owner_hi), (owner_lo)),                                          \
     XR_SLICE_WINDOW_CONSUMER_GUARD((consumer_bit)), XR_SLICE_WINDOW_PROOF_GUARD((proof)),         \
     xr_slice_window_eval((int64_t) (length), (int64_t) (start), (int64_t) (count),                \
                          (const void *) (data), (int64_t) (element_size), (proof)))

#define XR_SLICE_WINDOW_OWNER_APPLY_PROVEN(owner_hi, owner_lo, consumer_bit, proof, length, start, \
                                           count, data, element_size)                              \
    (XR_SLICE_WINDOW_OWNER_GUARD((owner_hi), (owner_lo)),                                          \
     XR_SLICE_WINDOW_CONSUMER_GUARD((consumer_bit)), XR_SLICE_WINDOW_PROVEN_GUARD((proof)),        \
     xr_slice_window_eval((int64_t) (length), (int64_t) (start), (int64_t) (count),                \
                          (const void *) (data), (int64_t) (element_size), (proof)))
#endif

#undef XR_SLICE_WINDOW_INLINE

#endif /* XR_SLICE_WINDOW_CORE_H */
