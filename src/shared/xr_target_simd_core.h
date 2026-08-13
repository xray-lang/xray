/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_simd_core.h - Runtime-neutral answers to the target vector queries.
 *
 * KEY CONCEPT:
 *   The language exposes four questions about the machine a program is built
 *   for: how many bytes a portable vector holds, whether the machine has
 *   vector hardware at all, whether the width is settled at run time, and
 *   whether that run-time width is a scalable vector register.  Each answer
 *   follows from two target facts - how the width is selected, and which
 *   vector features the target carries - and from nothing else.
 *
 *   Both statements of those answers used to be written out by hand: AOT CGen
 *   folded them from the link target, and the bytecode VM read them from
 *   integer and boolean literals in the simd module.  Nothing held the two in
 *   agreement.  This owner states each answer once; a backend maps its own
 *   target description into the selection and feature vocabulary here and asks.
 *
 *   Reading a build manifest, naming a run-time width helper and emitting the
 *   answer stay with the backend.  A width that only the machine knows is not
 *   a number here: the owner reports which run-time source settles it, and the
 *   backend spells that source in its own profile.
 *
 *   Self-contained: depends only on <stdint.h> and the generated owner IDs, so
 *   it stays includable from the freestanding AOT runtime.
 */

#ifndef XR_TARGET_SIMD_CORE_H
#define XR_TARGET_SIMD_CORE_H

#include "xr_semantic_owner_ids_gen.h"
#include <stdint.h>

/* How the target settles its vector width. */
typedef enum XrTargetSimdSelection {
    /* One width is chosen when the program is built and never changes. */
    XR_TARGET_SIMD_SELECTION_STATIC = 0,
    /* The machine reports the width of a scalable vector register at run time. */
    XR_TARGET_SIMD_SELECTION_SCALABLE = 1,
    /* A run-time capability check picks among several built widths. */
    XR_TARGET_SIMD_SELECTION_DISPATCH = 2,
    XR_TARGET_SIMD_SELECTION_COUNT = 3
} XrTargetSimdSelection;

/* Vector feature bits.  A backend hands its own target feature bitset here; the
 * two wide bits below are the only ones this owner reads by name, and the
 * adapter that supplies the bitset pins them with a static assertion so a
 * renumbering breaks the build rather than the answer.  Every other bit only
 * has to be nonzero to mean "this machine has vector hardware". */
#define XR_TARGET_SIMD_FEATURE_WIDE_256 (UINT32_C(1) << 2)
#define XR_TARGET_SIMD_FEATURE_WIDE_512 (UINT32_C(1) << 4)

/* The portable baseline: no vector features, one width fixed at build time.
 * The bytecode VM executes exactly this machine, and the simd module's
 * Capabilities declarations must state the same answers. */
#define XR_TARGET_SIMD_BASELINE_SELECTION XR_TARGET_SIMD_SELECTION_STATIC
#define XR_TARGET_SIMD_BASELINE_FEATURES UINT32_C(0)
#define XR_TARGET_SIMD_BASELINE_BYTES 16

typedef enum XrTargetSimdQueryKind {
    XR_TARGET_SIMD_QUERY_BYTES = 0,
    XR_TARGET_SIMD_QUERY_ACCELERATED = 1,
    XR_TARGET_SIMD_QUERY_RUNTIME_SELECTED = 2,
    XR_TARGET_SIMD_QUERY_SCALABLE = 3,
    XR_TARGET_SIMD_QUERY_KIND_COUNT = 4
} XrTargetSimdQueryKind;

typedef enum XrTargetSimdQueryStatus {
    XR_TARGET_SIMD_QUERY_OK = 0,
    XR_TARGET_SIMD_QUERY_INVALID_KIND = 1,
    XR_TARGET_SIMD_QUERY_INVALID_SELECTION = 2
} XrTargetSimdQueryStatus;

/* Where the byte width of a portable vector comes from. */
typedef enum XrTargetSimdWidthSource {
    /* The owner settled it: `bytes` carries the answer. */
    XR_TARGET_SIMD_WIDTH_STATIC = 0,
    /* A run-time capability check settles it; `bytes` is 0. */
    XR_TARGET_SIMD_WIDTH_RUNTIME_DISPATCH = 1,
    /* A scalable vector register settles it; `bytes` is 0. */
    XR_TARGET_SIMD_WIDTH_RUNTIME_SCALABLE = 2
} XrTargetSimdWidthSource;

typedef struct XrTargetSimdQueryResult {
    uint8_t status;        /* XrTargetSimdQueryStatus */
    uint8_t width_source;  /* XrTargetSimdWidthSource, only for the BYTES query */
    uint8_t answer;        /* 0/1, only for the predicate queries */
    int32_t bytes;         /* static vector byte width, 0 when a run-time source settles it */
} XrTargetSimdQueryResult;

#define XR_TARGET_SIMD_QUERY_OWNER_GUARD(owner_hi, owner_lo)                                       \
    ((void) sizeof(struct {                                                                        \
        unsigned int owner_id_must_be_shared_target_simd_query                                     \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_TARGET_SIMD_QUERY_HI &&            \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_TARGET_SIMD_QUERY_LO)              \
                   ? 1                                                                             \
                   : -1);                                                                          \
    }))

#define XR_TARGET_SIMD_QUERY_CONSUMER_GUARD(consumer_bit)                                          \
    ((void) sizeof(struct {                                                                        \
        unsigned int consumer_must_be_declared_for_shared_target_simd_query                        \
            : (((uint32_t) (consumer_bit) != 0 &&                                                  \
                (((uint32_t) (consumer_bit) & ((uint32_t) (consumer_bit) - 1)) == 0) &&            \
                (XR_SEM_OWNER_ID_SHARED_TARGET_SIMD_QUERY_CONSUMERS &                              \
                 (uint32_t) (consumer_bit)) != 0)                                                  \
                   ? 1                                                                             \
                   : -1);                                                                          \
    }))

#define XR_TARGET_SIMD_QUERY_KIND_GUARD(kind)                                                      \
    ((void) sizeof(struct {                                                                        \
        unsigned int kind_must_be_a_declared_target_simd_query                                     \
            : (((int) (kind) >= 0 && (int) (kind) < (int) XR_TARGET_SIMD_QUERY_KIND_COUNT) ? 1     \
                                                                                          : -1);   \
    }))

#define XR_TARGET_SIMD_QUERY_OWNER_APPLY(owner_hi, owner_lo, consumer_bit, kind, selection,        \
                                         features)                                                 \
    (XR_TARGET_SIMD_QUERY_OWNER_GUARD((owner_hi), (owner_lo)),                                     \
     XR_TARGET_SIMD_QUERY_CONSUMER_GUARD((consumer_bit)),                                          \
     XR_TARGET_SIMD_QUERY_KIND_GUARD((kind)),                                                      \
     xr_target_simd_query_core((kind), (selection), (features)))

/* The vector byte width a statically selected target holds.  A wider register
 * file is only usable when the target actually carries it, so the widest
 * declared feature wins and the portable 128-bit baseline is the floor. */
static inline int32_t xr_target_simd_static_bytes_core(uint32_t features) {
    if ((features & XR_TARGET_SIMD_FEATURE_WIDE_512) != 0)
        return 64;
    if ((features & XR_TARGET_SIMD_FEATURE_WIDE_256) != 0)
        return 32;
    return XR_TARGET_SIMD_BASELINE_BYTES;
}

static inline XrTargetSimdWidthSource xr_target_simd_width_source_core(uint8_t selection) {
    if (selection == (uint8_t) XR_TARGET_SIMD_SELECTION_SCALABLE)
        return XR_TARGET_SIMD_WIDTH_RUNTIME_SCALABLE;
    if (selection == (uint8_t) XR_TARGET_SIMD_SELECTION_DISPATCH)
        return XR_TARGET_SIMD_WIDTH_RUNTIME_DISPATCH;
    return XR_TARGET_SIMD_WIDTH_STATIC;
}

static inline XrTargetSimdQueryResult xr_target_simd_query_core(XrTargetSimdQueryKind kind,
                                                                uint8_t selection,
                                                                uint32_t features) {
    XrTargetSimdQueryResult result;
    result.status = (uint8_t) XR_TARGET_SIMD_QUERY_OK;
    result.width_source = (uint8_t) XR_TARGET_SIMD_WIDTH_STATIC;
    result.answer = 0;
    result.bytes = 0;

    if (selection >= (uint8_t) XR_TARGET_SIMD_SELECTION_COUNT) {
        result.status = (uint8_t) XR_TARGET_SIMD_QUERY_INVALID_SELECTION;
        return result;
    }
    switch (kind) {
        case XR_TARGET_SIMD_QUERY_BYTES:
            result.width_source = (uint8_t) xr_target_simd_width_source_core(selection);
            if (result.width_source == (uint8_t) XR_TARGET_SIMD_WIDTH_STATIC)
                result.bytes = xr_target_simd_static_bytes_core(features);
            return result;
        case XR_TARGET_SIMD_QUERY_ACCELERATED:
            /* Any declared vector feature means the machine executes vectors in
             * hardware.  The selection does not enter: a dispatch build still
             * carries the features it dispatches between. */
            result.answer = features != 0 ? 1u : 0u;
            return result;
        case XR_TARGET_SIMD_QUERY_RUNTIME_SELECTED:
            result.answer =
                (selection == (uint8_t) XR_TARGET_SIMD_SELECTION_DISPATCH ||
                 selection == (uint8_t) XR_TARGET_SIMD_SELECTION_SCALABLE)
                    ? 1u
                    : 0u;
            return result;
        case XR_TARGET_SIMD_QUERY_SCALABLE:
            result.answer = selection == (uint8_t) XR_TARGET_SIMD_SELECTION_SCALABLE ? 1u : 0u;
            return result;
        case XR_TARGET_SIMD_QUERY_KIND_COUNT:
        default:
            result.status = (uint8_t) XR_TARGET_SIMD_QUERY_INVALID_KIND;
            return result;
    }
}

/* Whether a query answer is the portable baseline this owner defines.  A
 * profile with no vector target of its own - the bytecode VM - executes that
 * baseline, and its adapter fails closed when the owner no longer agrees. */
static inline int xr_target_simd_query_is_baseline_core(XrTargetSimdQueryKind kind,
                                                        XrTargetSimdQueryResult result) {
    if (result.status != (uint8_t) XR_TARGET_SIMD_QUERY_OK)
        return 0;
    if (kind == XR_TARGET_SIMD_QUERY_BYTES)
        return result.width_source == (uint8_t) XR_TARGET_SIMD_WIDTH_STATIC &&
               result.bytes == XR_TARGET_SIMD_BASELINE_BYTES;
    return result.answer == 0;
}

#endif /* XR_TARGET_SIMD_CORE_H */
