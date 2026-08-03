/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_effect_db.h - Canonical analyzer-owned semantic effect summaries
 */

#ifndef XA_EFFECT_DB_H
#define XA_EFFECT_DB_H

#include "../../base/xdefs.h"
#include <stdbool.h>
#include <stdint.h>

typedef uint32_t XaEffectId;
typedef uint32_t XaErrorTypeId;
typedef uint32_t XaErrorVariantId;
typedef uint32_t XaEffectEdgeId;
typedef struct XrType XrType;

#define XA_EFFECT_NONE ((XaEffectId) 0u)
#define XA_EFFECT_ID_NONE XA_EFFECT_NONE
#define XA_EFFECT_EDGE_NONE ((XaEffectEdgeId) 0u)
#define XA_ERROR_TYPE_NONE ((XaErrorTypeId) 0u)
#define XA_ERROR_VARIANT_INVALID ((XaErrorVariantId) UINT32_MAX)

typedef struct XaBitSet {
    uint64_t *words;
    uint32_t word_count;
} XaBitSet;

typedef struct XaErrorTypeSet {
    XaErrorTypeId type_id;
    uint64_t stable_type_key;
    XaBitSet variants;
    bool all_variants;
} XaErrorTypeSet;

typedef struct XaErrorSet {
    XaErrorTypeSet *types;
    uint32_t count;
    uint32_t capacity;
} XaErrorSet;

/* Source-semantic effects are a product, not a mutually exclusive state.
 * Backend residue such as a release-AOT heap allocation is intentionally not
 * represented here; it belongs to the target/profile shape plan.
 *
 * Suspension is two independent dimensions, not one.  `yield expr` and
 * `Coro.yield()` both stop a body mid-flight, but they stop it in ways that
 * differ in every property a caller can act on, so a single bit cannot answer
 * either question:
 *
 *   SCHED_SUSPEND -- control reaches the scheduler.  The coroutine parks, may
 *     resume on a different OS thread, and observes cancellation here.  Sources:
 *     `await`, `select`, scope join, `Coro.yield()`, the blocking
 *     concurrency-handle methods, `time.sleep`, and an audited native contract
 *     that declares it.  Caller-visible: it composes transitively across calls.
 *
 *   GEN_SUSPEND -- the body contains `yield expr`, so its frame must survive a
 *     symmetric transfer back to the iterator driving it.  The scheduler is
 *     never involved, no thread migration is possible, and it is not a
 *     cancellation point.  It does NOT compose across calls: driving a generator
 *     resumes the generator's frame and returns normally, so a caller's own
 *     frame is untouched.  A generator that also reaches the scheduler
 *     publishes SCHED_SUSPEND independently, and that bit does propagate. */
typedef enum XaSemanticEffect {
    XA_SEM_EFFECT_NONE = 0,
    XA_SEM_EFFECT_ALLOC = 1u << 0,
    XA_SEM_EFFECT_SCHED_SUSPEND = 1u << 1,
    XA_SEM_EFFECT_MAY_BLOCK = 1u << 2,
    XA_SEM_EFFECT_THREAD_BLOCK = 1u << 3,
    XA_SEM_EFFECT_PANIC = 1u << 4,
    XA_SEM_EFFECT_ABORT = 1u << 5,
    XA_SEM_EFFECT_IO = 1u << 6,
    XA_SEM_EFFECT_FOREIGN = 1u << 7,
    XA_SEM_EFFECT_SYNC = 1u << 8,
    XA_SEM_EFFECT_GEN_SUSPEND = 1u << 9,
} XaSemanticEffect;

/* Either kind of suspension means the frame is not a plain function activation.
 * Backends asking "can control leave this body and come back?" want this union;
 * user-facing contracts ask the two questions separately. */
#define XA_SEM_EFFECT_ANY_SUSPEND                                                                  \
    ((XaSemanticEffectSet) (XA_SEM_EFFECT_SCHED_SUSPEND | XA_SEM_EFFECT_GEN_SUSPEND))

typedef uint32_t XaSemanticEffectSet;

typedef enum XaEffectCompleteness {
    XA_EFFECT_COMPLETE = 0,
    XA_EFFECT_INCOMPLETE = 1,
} XaEffectCompleteness;

typedef enum XaUnknownReason {
    XA_UNKNOWN_NONE = 0,
    XA_UNKNOWN_UNRESOLVED_CALLEE = 1u << 0,
    XA_UNKNOWN_MISSING_IMPORTED_EFFECT = 1u << 1,
    XA_UNKNOWN_OPEN_VIRTUAL_DISPATCH = 1u << 2,
    XA_UNKNOWN_DYNAMIC_CALL_TARGET = 1u << 3,
    XA_UNKNOWN_NATIVE_CONTRACT_MISSING = 1u << 4,
    XA_UNKNOWN_ANALYSIS_LIMIT = 1u << 5,
    XA_UNKNOWN_INVALID_PROGRAM = 1u << 6,
    XA_UNKNOWN_VIEW_INVALIDATION = 1u << 7,
    /* Resource exhaustion is not a semantic unknown. Callers must turn this
     * into a compiler error and must never consume the summary as permission
     * to optimize, move, share, borrow, or call a strong boundary. */
    XA_UNKNOWN_ANALYSIS_RESOURCE_FAILURE = 1u << 8,
} XaUnknownReason;

typedef uint32_t XaUnknownReasonSet;

typedef enum XaEffectContractKind {
    XA_EFFECT_CONTRACT_MISSING = 0,
    XA_EFFECT_CONTRACT_NOTHROW,
    XA_EFFECT_CONTRACT_ERRORS,
} XaEffectContractKind;

typedef struct XaEffectContract {
    XaEffectContractKind kind;
    const char **errors;
    uint32_t error_count;
} XaEffectContract;

typedef struct XaEffectSummary {
    XaSemanticEffectSet semantic_effects;
    XaSemanticEffectSet unknown_semantic_effects;
    XaErrorSet escaping;
    XaEffectCompleteness error_set_completeness;
    XaUnknownReasonSet error_unknown_reasons;
    XaEffectCompleteness completeness;
    XaUnknownReasonSet unknown_reasons;
    bool contains_unsafe_op;
    bool requires_unsafe_at_call;
    XaEffectEdgeId *roots;
    uint32_t root_count;
    uint32_t root_capacity;
    uint64_t fingerprint;
    uint32_t revision;
} XaEffectSummary;

typedef struct XaEffectDatabase XaEffectDatabase;

XR_FUNC XaEffectDatabase *xa_effect_db_new(void);
XR_FUNC void xa_effect_db_free(XaEffectDatabase *db);
XR_FUNC void xa_effect_db_clear(XaEffectDatabase *db);

XR_FUNC XaErrorTypeId xa_effect_db_register_error_type(XaEffectDatabase *db,
                                                       uint64_t stable_type_key,
                                                       XrType *type_handle);
XR_FUNC XaErrorTypeId xa_effect_db_register_error_enum(XaEffectDatabase *db, XrType *enum_type);
XR_FUNC XaErrorVariantId xa_effect_db_register_error_variant(XaEffectDatabase *db,
                                                             XaErrorTypeId type_id,
                                                             uint64_t stable_variant_key);
XR_FUNC uint64_t xa_effect_db_error_type_key(const XaEffectDatabase *db, XaErrorTypeId type_id);
XR_FUNC XrType *xa_effect_db_error_type_handle(const XaEffectDatabase *db, XaErrorTypeId type_id);
XR_FUNC uint64_t xa_effect_db_error_variant_key(const XaEffectDatabase *db, XaErrorTypeId type_id,
                                                XaErrorVariantId variant_id);
/* Canonical human-readable names for deterministic query/manifest output
 * (task 205 §7.2 / §11.1).  Names are display metadata; the stable key remains
 * the cross-process identity.  First assignment wins and the DB owns the copy. */
XR_FUNC void xa_effect_db_set_error_type_name(XaEffectDatabase *db, XaErrorTypeId type_id,
                                              const char *name);
XR_FUNC void xa_effect_db_set_error_variant_name(XaEffectDatabase *db, XaErrorTypeId type_id,
                                                 XaErrorVariantId variant_id, const char *name);
XR_FUNC const char *xa_effect_db_error_type_name(const XaEffectDatabase *db, XaErrorTypeId type_id);
XR_FUNC const char *xa_effect_db_error_variant_name(const XaEffectDatabase *db,
                                                    XaErrorTypeId type_id,
                                                    XaErrorVariantId variant_id);

XR_FUNC void xa_effect_summary_init(XaEffectSummary *summary);
XR_FUNC void xa_effect_summary_clear(XaEffectSummary *summary);
XR_FUNC bool xa_effect_summary_add_root(XaEffectSummary *summary, XaEffectEdgeId root_id);
XR_FUNC bool xa_effect_summary_add_variant(XaEffectDatabase *db, XaEffectSummary *summary,
                                           XaErrorTypeId type_id, XaErrorVariantId variant_id);
XR_FUNC bool xa_effect_summary_add_all_variants(XaEffectDatabase *db, XaEffectSummary *summary,
                                                XaErrorTypeId type_id);
XR_FUNC bool xa_effect_summary_add_summary(XaEffectDatabase *db, XaEffectSummary *summary,
                                           const XaEffectSummary *src);
XR_FUNC void xa_effect_summary_add_semantic_effects(XaEffectSummary *summary,
                                                    XaSemanticEffectSet effects);
XR_FUNC bool xa_effect_summary_has_semantic_effect(const XaEffectSummary *summary,
                                                   XaSemanticEffect effect);
XR_FUNC void xa_effect_summary_mark_contains_unsafe(XaEffectSummary *summary);
XR_FUNC void xa_effect_summary_mark_requires_unsafe(XaEffectSummary *summary);
XR_FUNC bool xa_effect_summary_add_type_from_summary(XaEffectDatabase *db, XaEffectSummary *summary,
                                                     const XaEffectSummary *src,
                                                     XaErrorTypeId type_id);
XR_FUNC bool xa_effect_summary_add_variant_from_summary(XaEffectDatabase *db,
                                                        XaEffectSummary *summary,
                                                        const XaEffectSummary *src,
                                                        XaErrorTypeId type_id,
                                                        XaErrorVariantId variant_id);
XR_FUNC void xa_effect_summary_clear_escaping(XaEffectSummary *summary);
XR_FUNC bool xa_effect_summary_subtract_type(XaEffectSummary *summary, XaErrorTypeId type_id);
XR_FUNC bool xa_effect_summary_subtract_variant(XaEffectDatabase *db, XaEffectSummary *summary,
                                                XaErrorTypeId type_id, XaErrorVariantId variant_id);
XR_FUNC void xa_effect_summary_mark_incomplete(XaEffectSummary *summary, XaUnknownReason reason);
XR_FUNC void xa_effect_summary_mark_semantic_incomplete(XaEffectSummary *summary,
                                                        XaSemanticEffect effect,
                                                        XaUnknownReason reason);
XR_FUNC bool xa_effect_summary_is_complete(const XaEffectSummary *summary);
XR_FUNC bool xa_effect_summary_has_resource_failure(const XaEffectSummary *summary);
XR_FUNC bool xa_effect_summary_is_nothrow(const XaEffectSummary *summary);
XR_FUNC uint64_t xa_effect_summary_fingerprint(const XaEffectDatabase *db,
                                               const XaEffectSummary *summary);

/* Public effect API-diff. Compares two summaries of the same
 * exported entity by stable error keys and completeness.  SOURCE_BREAKING
 * dominates BREAKING dominates IMPROVEMENT dominates COMPATIBLE so release gates
 * can reject regressions.
 *
 * SOURCE_BREAKING is reserved for a newly escaping typed error because a
 * catch-all binding projects the callee's reachable error set. Widening that
 * set makes a downstream exhaustive `match (e)`
 * stop compiling (E0371) even though the source signature is unchanged.  A typed
 * `catch (e: T)` is immune, but the gate classifies by worst case.  Behavioral
 * breaks that a catch-all cannot observe -- became-incomplete, a new semantic
 * effect, a new unsafe requirement -- stay BREAKING. */
typedef enum XaEffectDiffKind {
    XA_EFFECT_DIFF_COMPATIBLE = 0,
    XA_EFFECT_DIFF_IMPROVEMENT = 1,
    XA_EFFECT_DIFF_BREAKING = 2,
    XA_EFFECT_DIFF_SOURCE_BREAKING = 3,
} XaEffectDiffKind;

typedef struct XaEffectDiff {
    XaEffectDiffKind kind;
    bool added_escaping;   /* after escapes an error the before summary did not */
    bool removed_escaping; /* before escaped an error the after summary does not */
    bool became_incomplete;
    bool became_complete;
    bool widened_unknown;  /* after has unknown reasons absent from before */
    bool narrowed_unknown; /* before has unknown reasons absent from after */
    XaSemanticEffectSet added_semantic_effects;
    XaSemanticEffectSet removed_semantic_effects;
    bool added_unsafe_operation;
    bool removed_unsafe_operation;
    bool added_unsafe_call_requirement;
    bool removed_unsafe_call_requirement;
} XaEffectDiff;

XR_FUNC XaEffectDiffKind xa_effect_summary_diff(const XaEffectDatabase *db,
                                                const XaEffectSummary *before,
                                                const XaEffectSummary *after,
                                                XaEffectDiff *out_diff);

/* Deterministic canonical JSON projection of a summary (task 205 §7.2 / §11.1):
 * schema-tagged, escaping errors sorted by stable key, fixed-order unknown
 * reasons, and the stable fingerprint.  `symbol_qualified_name` is optional.
 * Returns a heap string the caller frees, or NULL on allocation failure. */
XR_FUNC char *xa_effect_summary_to_json(const XaEffectDatabase *db, const XaEffectSummary *summary,
                                        const char *symbol_qualified_name);

XR_FUNC XaEffectId xa_effect_db_intern(XaEffectDatabase *db, const XaEffectSummary *summary);
/* Import a summary from another analyzer-owned database by stable error type
 * and variant keys. Analyzer-local witness edge ids are deliberately not
 * copied across the ownership boundary. */
XR_FUNC XaEffectId xa_effect_db_import(XaEffectDatabase *dst, const XaEffectDatabase *src,
                                       XaEffectId src_id);
XR_FUNC const XaEffectSummary *xa_effect_db_get(const XaEffectDatabase *db, XaEffectId id);
XR_FUNC uint32_t xa_effect_db_summary_count(const XaEffectDatabase *db);

XR_FUNC bool xa_bitset_test(const XaBitSet *set, uint32_t bit);
XR_FUNC uint32_t xa_bitset_word_count(const XaBitSet *set);

#endif /* XA_EFFECT_DB_H */
