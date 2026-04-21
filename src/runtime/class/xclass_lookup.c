/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xclass_lookup.c - Name -> XrClass lookup
 *
 * KEY CONCEPT:
 *   Single O(1) path through the type registry hashmap. Every class
 *   that reaches xr_class_builder_finalize is eagerly registered
 *   there, so the registry is authoritative for builtin as well as
 *   user-defined classes. The previous implementation also carried a
 *   hard-coded 11-entry strcmp chain for the core classes and a
 *   linear globals scan; both were redundant after the eager
 *   registration in P7 and have been removed.
 */

#include "xclass_lookup.h"
#include "../../base/xchecks.h"
#include "../xisolate_api.h"
#include "xclass.h"
#include "xreflect_registry.h"

XrClass* xr_class_lookup_by_name(XrayIsolate *X, const char *class_name) {
    if (!X || !class_name) return NULL;

    XrTypeMetadata *meta = xr_registry_find_type(X, class_name);
    return (meta && meta->klass) ? meta->klass : NULL;
}
