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
/* The Source compiler has proved these exact stored-type roots using its
 * actual receiving map and both sealed Programs. Shape checks below only
 * validate that correspondence; matching unrelated shapes grants nothing.
 * The target must be private and unused. Failure changes neither arena. */
XR_FUNC XrXirValueStatus xir_compile_type_arena_receive(const XrXirCompileContext *context,
    XrXirTypeArena *target,XrXirTypeArena *source,const XirTypeReceivingRoot *roots,uint32_t count);
/* Identity equality grants no Domain, publication, borrow or call permission. */
XR_FUNC bool xir_type_arena_same_type(const XrXirTypeArena *left,XrXirType left_type,
    const XrXirTypeArena *right,XrXirType right_type);
/* Callers charge this complete bounded pool walk on their own work ledger.
 * Failure preserves output and does not infer identity from descriptor shape. */
XR_FUNC uint64_t xir_type_arena_lookup_work(const XrXirTypeArena *target);
XR_FUNC bool xir_type_arena_local_type(const XrXirTypeArena *target,
    const XrXirTypeArena *source,XrXirType type,XrXirType *output);
#endif // XXIR_TYPE_ARENA_INTERNAL_H
