/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_compare_core.h - Canonical equality and order relations shared by VM/AOT.
 *
 * One statement of what xi.eq / xi.ne / xi.lt / xi.le / xi.gt / xi.ge mean.
 * The kernel owns two things and nothing else:
 *
 *   1. The relation. Given a kind and a lane, which boolean comes out. The
 *      lanes are the machine domains a comparison can land on - signed 64-bit,
 *      unsigned 64-bit, IEEE double, address identity - plus the two answers a
 *      domain comparator hands back: a three-way ordering (big integers,
 *      strings) or a bare equality verdict (aggregates, enum payloads).
 *
 *   2. The route. Given what each operand is, which lane the pair evaluates on,
 *      or that the pair has no lossless common type at all.
 *
 * Carrier inspection stays with the consumer: only the backend knows how to read
 * a tag, walk an aggregate, or call a big-integer comparator. It maps its
 * operands into XrCompareOperandClass, asks the kernel for the route, produces
 * the lane values, and asks the kernel for the relation. Nothing about the
 * answer is restated on the way through.
 */

#ifndef XR_COMPARE_CORE_H
#define XR_COMPARE_CORE_H

#if !defined(XR_COMPARE_C90)
#include "xr_semantic_owner_ids_gen.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define XR_COMPARE_INLINE static inline
#else
/* Restricted C90 provides bool, the fixed-width integers and NULL before
 * including this core, and states the relation without the owner guards. */
#define XR_COMPARE_INLINE static
#endif

/* The six relations, in Xi operation order. */
typedef enum XrCompareKind {
    XR_COMPARE_EQ = 0,
    XR_COMPARE_NE = 1,
    XR_COMPARE_LT = 2,
    XR_COMPARE_LE = 3,
    XR_COMPARE_GT = 4,
    XR_COMPARE_GE = 5
} XrCompareKind;

/* What an operand is, as far as the relation is concerned. A consumer maps its
 * own carrier - a VM tag, an AOT tag, an Xi representation - onto this. */
typedef enum XrCompareOperandClass {
    XR_COMPARE_OPERAND_OTHER = 0,  /* not a number: strings, aggregates, null   */
    XR_COMPARE_OPERAND_INT = 1,    /* machine integer in the i64 value slot     */
    XR_COMPARE_OPERAND_FLOAT = 2,  /* IEEE double                               */
    XR_COMPARE_OPERAND_BIGINT = 3  /* arbitrary-precision integer               */
} XrCompareOperandClass;

/* Which lane a pair of numeric operands evaluates on. */
typedef enum XrCompareRoute {
    XR_COMPARE_ROUTE_OTHER = 0,       /* not a numeric pair; consumer's own rule */
    XR_COMPARE_ROUTE_I64 = 1,         /* both machine integers                   */
    XR_COMPARE_ROUTE_F64 = 2,         /* evaluate as IEEE doubles                */
    XR_COMPARE_ROUTE_BIGINT = 3,      /* both arbitrary precision                */
    XR_COMPARE_ROUTE_BIGINT_INT = 4,  /* big on the left, machine int on the right */
    XR_COMPARE_ROUTE_INT_BIGINT = 5,  /* machine int on the left, big on the right */
    XR_COMPARE_ROUTE_UNRELATED = 6    /* no lossless common type                 */
} XrCompareRoute;

/* Signed 64-bit lane. */
XR_COMPARE_INLINE bool xr_compare_i64_core(XrCompareKind kind, int64_t a, int64_t b) {
    switch (kind) {
        case XR_COMPARE_EQ:
            return a == b;
        case XR_COMPARE_NE:
            return a != b;
        case XR_COMPARE_LT:
            return a < b;
        case XR_COMPARE_LE:
            return a <= b;
        case XR_COMPARE_GT:
            return a > b;
        case XR_COMPARE_GE:
            return a >= b;
    }
    return false;
}

/* Unsigned 64-bit lane. The i64 value slot carries no signedness tag, so a
 * u64/usize operand with the top bit set must order as unsigned; narrower
 * unsigned payloads are zero-extended, so both lanes agree there. */
XR_COMPARE_INLINE bool xr_compare_u64_core(XrCompareKind kind, uint64_t a, uint64_t b) {
    switch (kind) {
        case XR_COMPARE_EQ:
            return a == b;
        case XR_COMPARE_NE:
            return a != b;
        case XR_COMPARE_LT:
            return a < b;
        case XR_COMPARE_LE:
            return a <= b;
        case XR_COMPARE_GT:
            return a > b;
        case XR_COMPARE_GE:
            return a >= b;
    }
    return false;
}

/* IEEE double lane. This is the `==` relation, not a key relation: every
 * comparison against NaN is false, including NaN == NaN, and NE against NaN is
 * true. Container keying uses its own reflexive rule elsewhere. */
XR_COMPARE_INLINE bool xr_compare_f64_core(XrCompareKind kind, double a, double b) {
    switch (kind) {
        case XR_COMPARE_EQ:
            return a == b;
        case XR_COMPARE_NE:
            return a != b;
        case XR_COMPARE_LT:
            return a < b;
        case XR_COMPARE_LE:
            return a <= b;
        case XR_COMPARE_GT:
            return a > b;
        case XR_COMPARE_GE:
            return a >= b;
    }
    return false;
}

/* Address identity lane, for raw pointers and for heap operands a consumer
 * compares by reference rather than by content. */
XR_COMPARE_INLINE bool xr_compare_ptr_core(XrCompareKind kind, const void *a, const void *b) {
    switch (kind) {
        case XR_COMPARE_EQ:
            return a == b;
        case XR_COMPARE_NE:
            return a != b;
        case XR_COMPARE_LT:
            return (uintptr_t) a < (uintptr_t) b;
        case XR_COMPARE_LE:
            return (uintptr_t) a <= (uintptr_t) b;
        case XR_COMPARE_GT:
            return (uintptr_t) a > (uintptr_t) b;
        case XR_COMPARE_GE:
            return (uintptr_t) a >= (uintptr_t) b;
    }
    return false;
}

/* Three-way lane: the answer of a domain comparator that reports negative,
 * zero or positive - big integers and strings. */
XR_COMPARE_INLINE bool xr_compare_ordering_core(XrCompareKind kind, int ordering) {
    switch (kind) {
        case XR_COMPARE_EQ:
            return ordering == 0;
        case XR_COMPARE_NE:
            return ordering != 0;
        case XR_COMPARE_LT:
            return ordering < 0;
        case XR_COMPARE_LE:
            return ordering <= 0;
        case XR_COMPARE_GT:
            return ordering > 0;
        case XR_COMPARE_GE:
            return ordering >= 0;
    }
    return false;
}

/* Equality-only lane: the answer of a domain that can say whether two operands
 * are the same but cannot order them - aggregates, enum payloads, null. An
 * order relation over such a domain is unanswerable and reports false. */
XR_COMPARE_INLINE bool xr_compare_equal_core(XrCompareKind kind, bool equal) {
    switch (kind) {
        case XR_COMPARE_EQ:
            return equal;
        case XR_COMPARE_NE:
            return !equal;
        case XR_COMPARE_LT:
        case XR_COMPARE_LE:
        case XR_COMPARE_GT:
        case XR_COMPARE_GE:
            return false;
    }
    return false;
}

XR_COMPARE_INLINE bool xr_compare_kind_is_equality_core(XrCompareKind kind) {
    return kind == XR_COMPARE_EQ || kind == XR_COMPARE_NE;
}

/* GT and GE are the mirror of LT and LE. A consumer that owns only the two
 * lower relations - the AOT tagged adapters, the VM's ordering helpers - gets
 * the mirrored kind here instead of writing the swap out again. */
XR_COMPARE_INLINE XrCompareKind xr_compare_kind_mirrored_core(XrCompareKind kind) {
    switch (kind) {
        case XR_COMPARE_LT:
            return XR_COMPARE_GT;
        case XR_COMPARE_LE:
            return XR_COMPARE_GE;
        case XR_COMPARE_GT:
            return XR_COMPARE_LT;
        case XR_COMPARE_GE:
            return XR_COMPARE_LE;
        case XR_COMPARE_EQ:
        case XR_COMPARE_NE:
            return kind;
    }
    return kind;
}

/* Which lane a pair of operands evaluates on.
 *
 * Equality and order do not route a mixed integer/float pair the same way. The
 * language requires an explicit conversion between an integer and a float and
 * says so for the equality operator specifically: `==` performs no implicit
 * int-to-float promotion, so a tagged integer never equals a tagged float, and
 * a runtime that answered otherwise would disagree with the static rule that
 * rejects the spelling outright. An order relation still evaluates the pair in
 * double, which is what every executor does today.
 *
 * An integer and a big integer do have a lossless common type, so both the
 * equality and the order relation widen to the big lane. */
XR_COMPARE_INLINE XrCompareRoute xr_compare_route_core(XrCompareKind kind,
                                                       XrCompareOperandClass left,
                                                       XrCompareOperandClass right) {
    if (left == XR_COMPARE_OPERAND_BIGINT || right == XR_COMPARE_OPERAND_BIGINT) {
        if (left == XR_COMPARE_OPERAND_BIGINT && right == XR_COMPARE_OPERAND_BIGINT)
            return XR_COMPARE_ROUTE_BIGINT;
        if (left == XR_COMPARE_OPERAND_BIGINT && right == XR_COMPARE_OPERAND_INT)
            return XR_COMPARE_ROUTE_BIGINT_INT;
        if (left == XR_COMPARE_OPERAND_INT && right == XR_COMPARE_OPERAND_BIGINT)
            return XR_COMPARE_ROUTE_INT_BIGINT;
        return XR_COMPARE_ROUTE_OTHER;
    }
    if (left == XR_COMPARE_OPERAND_INT && right == XR_COMPARE_OPERAND_INT)
        return XR_COMPARE_ROUTE_I64;
    if (left == XR_COMPARE_OPERAND_FLOAT && right == XR_COMPARE_OPERAND_FLOAT)
        return XR_COMPARE_ROUTE_F64;
    if ((left == XR_COMPARE_OPERAND_INT && right == XR_COMPARE_OPERAND_FLOAT) ||
        (left == XR_COMPARE_OPERAND_FLOAT && right == XR_COMPARE_OPERAND_INT))
        return xr_compare_kind_is_equality_core(kind) ? XR_COMPARE_ROUTE_UNRELATED
                                                      : XR_COMPARE_ROUTE_F64;
    return XR_COMPARE_ROUTE_OTHER;
}

#if !defined(XR_COMPARE_C90)

#define XR_COMPARE_OWNER_GUARD(owner_hi, owner_lo)                                                 \
    ((void) sizeof(struct {                                                                        \
        unsigned int owner_id_must_be_shared_compare                                               \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_COMPARE_HI &&                      \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_COMPARE_LO)                        \
                   ? 1                                                                             \
                   : -1);                                                                          \
    }))

#define XR_COMPARE_CONSUMER_GUARD(consumer_bit)                                                    \
    ((void) sizeof(struct {                                                                        \
        unsigned int consumer_must_be_declared_for_shared_compare                                  \
            : (((uint32_t) (consumer_bit) != 0 &&                                                  \
                (((uint32_t) (consumer_bit) & ((uint32_t) (consumer_bit) - 1)) == 0) &&            \
                (XR_SEM_OWNER_ID_SHARED_COMPARE_CONSUMERS & (uint32_t) (consumer_bit)) != 0)       \
                   ? 1                                                                             \
                   : -1);                                                                          \
    }))

/* A relation named as a constant - the token CGen writes into generated C - is
 * checked here; a relation carried as a value is checked by the XrCompareKind
 * enum at every adapter boundary and by the kernel's exhaustive switch. */
#define XR_COMPARE_KIND_GUARD(kind)                                                                \
    ((void) sizeof(struct {                                                                        \
        unsigned int kind_must_be_a_declared_compare_relation                                      \
            : (((kind) == XR_COMPARE_EQ || (kind) == XR_COMPARE_NE || (kind) == XR_COMPARE_LT ||   \
                (kind) == XR_COMPARE_LE || (kind) == XR_COMPARE_GT || (kind) == XR_COMPARE_GE)     \
                   ? 1                                                                             \
                   : -1);                                                                          \
    }))

#define XR_COMPARE_OWNER_APPLY_I64(owner_hi, owner_lo, consumer_bit, kind, a, b)                   \
    (XR_COMPARE_OWNER_GUARD((owner_hi), (owner_lo)),                                               \
     XR_COMPARE_CONSUMER_GUARD((consumer_bit)),                                                    \
     xr_compare_i64_core((XrCompareKind) (kind), (int64_t) (a), (int64_t) (b)))

#define XR_COMPARE_OWNER_APPLY_U64(owner_hi, owner_lo, consumer_bit, kind, a, b)                   \
    (XR_COMPARE_OWNER_GUARD((owner_hi), (owner_lo)),                                               \
     XR_COMPARE_CONSUMER_GUARD((consumer_bit)),                                                    \
     xr_compare_u64_core((XrCompareKind) (kind), (uint64_t) (a), (uint64_t) (b)))

#define XR_COMPARE_OWNER_APPLY_F64(owner_hi, owner_lo, consumer_bit, kind, a, b)                   \
    (XR_COMPARE_OWNER_GUARD((owner_hi), (owner_lo)),                                               \
     XR_COMPARE_CONSUMER_GUARD((consumer_bit)),                                                    \
     xr_compare_f64_core((XrCompareKind) (kind), (double) (a), (double) (b)))

#define XR_COMPARE_OWNER_APPLY_PTR(owner_hi, owner_lo, consumer_bit, kind, a, b)                   \
    (XR_COMPARE_OWNER_GUARD((owner_hi), (owner_lo)),                                               \
     XR_COMPARE_CONSUMER_GUARD((consumer_bit)),                                                    \
     xr_compare_ptr_core((XrCompareKind) (kind), (const void *) (a), (const void *) (b)))

#define XR_COMPARE_OWNER_APPLY_ORDERING(owner_hi, owner_lo, consumer_bit, kind, ordering)          \
    (XR_COMPARE_OWNER_GUARD((owner_hi), (owner_lo)),                                               \
     XR_COMPARE_CONSUMER_GUARD((consumer_bit)),                                                    \
     xr_compare_ordering_core((XrCompareKind) (kind), (int) (ordering)))

#define XR_COMPARE_OWNER_APPLY_EQUAL(owner_hi, owner_lo, consumer_bit, kind, equal)                \
    (XR_COMPARE_OWNER_GUARD((owner_hi), (owner_lo)),                                               \
     XR_COMPARE_CONSUMER_GUARD((consumer_bit)),                                                    \
     xr_compare_equal_core((XrCompareKind) (kind), (bool) (equal)))

#define XR_COMPARE_OWNER_ROUTE(owner_hi, owner_lo, consumer_bit, kind, left, right)                \
    (XR_COMPARE_OWNER_GUARD((owner_hi), (owner_lo)),                                               \
     XR_COMPARE_CONSUMER_GUARD((consumer_bit)),                                                    \
     xr_compare_route_core((XrCompareKind) (kind), (XrCompareOperandClass) (left),                 \
                           (XrCompareOperandClass) (right)))

#endif /* !XR_COMPARE_C90 */

/* Relation spelled directly on a C operand pair.
 *
 * AOT CGen lowers a proven scalar comparison to a machine compare whose operand
 * type is the plan's own storage type - int64_t, uint64_t, double, float,
 * int8_t, uint32_t and so on. A lane function would have to pick one of those
 * and would change what the target compares. These per-relation forms keep the
 * operand type the plan chose and still state the relation once, here, so a
 * generated comparison is the owner's relation and not a private spelling.
 * The token pasted below is the relation suffix, so an undeclared relation is a
 * preprocessing failure rather than a silent fallback. */
#define XR_COMPARE_RELATION_EQ(a, b) ((a) == (b))
#define XR_COMPARE_RELATION_NE(a, b) ((a) != (b))
#define XR_COMPARE_RELATION_LT(a, b) ((a) < (b))
#define XR_COMPARE_RELATION_LE(a, b) ((a) <= (b))
#define XR_COMPARE_RELATION_GT(a, b) ((a) > (b))
#define XR_COMPARE_RELATION_GE(a, b) ((a) >= (b))

#if !defined(XR_COMPARE_C90)
#define XR_COMPARE_OWNER_APPLY_NATIVE(owner_hi, owner_lo, consumer_bit, relation, a, b)            \
    (XR_COMPARE_OWNER_GUARD((owner_hi), (owner_lo)),                                               \
     XR_COMPARE_CONSUMER_GUARD((consumer_bit)),                                                    \
     XR_COMPARE_KIND_GUARD(XR_COMPARE_##relation), XR_COMPARE_RELATION_##relation((a), (b)))
#else
#define XR_COMPARE_OWNER_APPLY_NATIVE(owner_hi, owner_lo, consumer_bit, relation, a, b)            \
    XR_COMPARE_RELATION_##relation((a), (b))
#endif

#endif /* XR_COMPARE_CORE_H */
