/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_builtin_identity_shape.h - Roster of the frozen builtin classes
 *
 * KEY CONCEPT:
 *   A frozen builtin is a named class the runtime owns outright: no source
 *   declaration can produce one, so its instances carry a NULL class_ref and
 *   the SemanticPlan stores the class as a builtin type id. Which classes are
 *   frozen, and which of their instance methods can suspend the calling
 *   coroutine, are closed facts about the language rather than conclusions any
 *   one layer derives. Both are stated once here.
 *
 *   Every consumer needs the roster in a different direction. The plan producer
 *   starts from a live type and needs its id; the verifier starts from a stored
 *   id and needs the name the canonical key must spell; a suspension judgement
 *   starts from a receiver and needs to know whether the selector it names can
 *   yield. Restating the roster once per direction is what lets the directions
 *   disagree, so each one is served from the same two tables below.
 *
 *   Sharing the roster is not sharing a conclusion. Each layer still rebuilds
 *   its own judgement from its own records — the verifier reads the frozen
 *   canonical key it must not trust the builder for — and only the list of
 *   which classes and methods exist is common.
 */

#ifndef XR_SEMANTIC_BUILTIN_IDENTITY_SHAPE_H
#define XR_SEMANTIC_BUILTIN_IDENTITY_SHAPE_H

#include "../../runtime/value/xtype.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct XrSemanticFrozenBuiltin {
    const char *name;
    uint32_t builtin_type;
} XrSemanticFrozenBuiltin;

/* The class spelling is the declaration identity, and the id is how the plan
 * stores it. `Task` maps to the coroutine id because the handle predates the
 * surface name; the two spellings denote one class, and nothing else may claim
 * either. */
static const XrSemanticFrozenBuiltin xr_semantic_frozen_builtins[] = {
    {"StringBuilder", XR_TID_STRINGBUILDER},   {"Task", XR_TID_COROUTINE},
    {"WorkQueue", XR_TID_WORKQUEUE},           {"ResultGroup", XR_TID_RESULTGROUP},
    {"CountdownLatch", XR_TID_COUNTDOWNLATCH}, {"Semaphore", XR_TID_SEMAPHORE},
    {"EventCount", XR_TID_EVENTCOUNT},
};

/* Resolve a live type to its frozen builtin id, or XR_TID_NULL when the type is
 * not a frozen builtin. A user class that reuses one of these names carries its
 * own class_ref and is deliberately not a match: builtin names are ordinary
 * identifiers, so `class Semaphore { ... }` is legal source that must not
 * inherit the runtime class's storage or suspension rules. */
static inline uint32_t xr_semantic_frozen_builtin_type(const XrType *type) {
    for (size_t i = 0;
         i < sizeof(xr_semantic_frozen_builtins) / sizeof(*xr_semantic_frozen_builtins); i++)
        if (xr_type_is_builtin_named_class(type, xr_semantic_frozen_builtins[i].name))
            return xr_semantic_frozen_builtins[i].builtin_type;
    return XR_TID_NULL;
}

/* The reverse direction: the class spelling a stored builtin id must carry, or
 * NULL when the id names no frozen builtin. */
static inline const char *xr_semantic_frozen_builtin_name(uint32_t builtin_type) {
    if (builtin_type == XR_TID_NULL)
        return NULL;
    for (size_t i = 0;
         i < sizeof(xr_semantic_frozen_builtins) / sizeof(*xr_semantic_frozen_builtins); i++)
        if (xr_semantic_frozen_builtins[i].builtin_type == builtin_type)
            return xr_semantic_frozen_builtins[i].name;
    return NULL;
}

typedef struct XrSemanticBuiltinYieldableMethod {
    uint32_t builtin_type;
    const char *selector;
    uint16_t min_arguments;
    uint16_t max_arguments;
} XrSemanticBuiltinYieldableMethod;

/* The frozen builtin instance methods that can park the calling coroutine. The
 * argument counts are part of the identity: an arity outside the declared range
 * is a different method, and no overload of these names suspends. Argument
 * counts exclude the receiver. A frozen builtin absent from this table — such
 * as StringBuilder — has no suspending method at all. */
static const XrSemanticBuiltinYieldableMethod xr_semantic_builtin_yieldable_methods[] = {
    {XR_TID_COROUTINE, "awaitResult", 0, 0}, {XR_TID_COROUTINE, "awaitTimeout", 1, 1},
    {XR_TID_WORKQUEUE, "pop", 0, 1},         {XR_TID_RESULTGROUP, "recv", 0, 0},
    {XR_TID_COUNTDOWNLATCH, "wait", 0, 0},   {XR_TID_SEMAPHORE, "acquire", 0, 0},
    {XR_TID_EVENTCOUNT, "wait", 1, 2},
};

/* Whether a receiver of this frozen builtin class suspends when the named
 * selector is called with this many arguments. */
static inline bool xr_semantic_builtin_instance_yieldable(uint32_t builtin_type,
                                                          const char *selector,
                                                          uint16_t argument_count) {
    if (builtin_type == XR_TID_NULL || !selector)
        return false;
    for (size_t i = 0; i < sizeof(xr_semantic_builtin_yieldable_methods) /
                               sizeof(*xr_semantic_builtin_yieldable_methods);
         i++) {
        const XrSemanticBuiltinYieldableMethod *method = &xr_semantic_builtin_yieldable_methods[i];
        if (method->builtin_type == builtin_type && strcmp(method->selector, selector) == 0 &&
            argument_count >= method->min_arguments && argument_count <= method->max_arguments)
            return true;
    }
    return false;
}

#endif  // XR_SEMANTIC_BUILTIN_IDENTITY_SHAPE_H
