/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_type_arena_internal.h - Compiler-authenticated receiving identities
 */
#ifndef XXIR_TYPE_ARENA_INTERNAL_H
#define XXIR_TYPE_ARENA_INTERNAL_H
#include "xxir_type_arena.h"
typedef struct XirTypeReceivingRoot { XrXirType source, target; } XirTypeReceivingRoot;
typedef struct XirTypeIdentity { const XrXirTypeArena *owner; XrXirType type; } XirTypeIdentity;
/* The Source compiler has proved these exact stored-type roots using its
 * actual receiving map and both sealed Programs. Shape checks below only
 * validate that correspondence; matching unrelated shapes grants nothing.
 * The target must be private and unused. Failure changes neither arena. */
XR_FUNC XrXirValueStatus xir_compile_type_arena_receive(const XrXirCompileContext *context,
    XrXirTypeArena *target,XrXirTypeArena *source,const XirTypeReceivingRoot *roots,uint32_t count);
/* A live input owns this borrowed identity and its complete receiving lineage.
 * The projection grants no value, publication, Domain or loan permission. */
XR_FUNC bool xir_type_arena_identity(const XrXirTypeArena *, XrXirType, XirTypeIdentity *);
/* The native builtin producer has admitted an actual Array<i64> word. Its
 * private six-type catalog receives only that exact Array identity; the other
 * types remain new. Target allocations own their ledger and retain the source. */
XR_FUNC XrXirValueStatus xir_native_type_arena_receive_i64_array(XrXirTypeArena *target,
    const XrXirValue *array);
/* The native Map producer owns this exact single-row source catalog. The
 * private receiving catalog binds only its Map leaf before publication. */
XR_FUNC XrXirValueStatus xir_native_type_arena_receive_string_i64_map(XrXirTypeArena *target,
    XrXirType type, const XrXirTypeArena *source);
/* Borrowed through the receiving catalog's existing owning source edge. */
XR_FUNC const XrXirTypeArena *xir_native_type_arena_string_i64_map_source(const XrXirTypeArena *, XrXirType);
/* Derived directories charge the base catalog's existing finite ledger. A
 * failed receive releases the unpublished directory and preserves output. */
XR_FUNC XrXirValueStatus xir_native_type_arena_derive_i64_views(const XrXirTypeArena *catalog,
    const XrXirValue *array, XrXirTypeArena **empty);
/* Only the native receiving producer publishes this immutable directory role.
 * Reusing its owned arena keeps repeated materialization from adding lineage. */
XR_FUNC bool xir_native_type_arena_i64_views(const XrXirTypeArena *);
/* Identity equality grants no Domain, publication, borrow or call permission. */
XR_FUNC bool xir_type_arena_same_type(const XrXirTypeArena *left,XrXirType left_type,
    const XrXirTypeArena *right,XrXirType right_type);
/* Callers charge this complete bounded pool walk on their own work ledger.
 * Failure preserves output and does not infer identity from descriptor shape. */
XR_FUNC uint64_t xir_type_arena_lookup_work(const XrXirTypeArena *target);
XR_FUNC bool xir_type_arena_local_type(const XrXirTypeArena *target,
    const XrXirTypeArena *source,XrXirType type,XrXirType *output);
#endif // XXIR_TYPE_ARENA_INTERNAL_H
