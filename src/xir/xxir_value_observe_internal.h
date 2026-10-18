/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_value_observe_internal.h - Owned data projections on an observer ledger
 */
#ifndef XXIR_VALUE_OBSERVE_INTERNAL_H
#define XXIR_VALUE_OBSERVE_INTERNAL_H
#include "xxir_value_place.h"
#include "xxir_fault.h"
/* Read one Array/Struct child or immutable wrapper payload from an owned value. The
 * original family validates its complete storage before materialization.
 * Class handles remain identities in their original Domain; no Domain, place,
 * invocation binding or write permission is returned. All observer work and
 * materialization spend the supplied existing observer ledger. Failure keeps
 * output empty; only BOUNDS publishes a fault detail. */
XR_FUNC XrXirValueStatus xir_value_observe_child(const XrXirValue *,const XrXirValuePathStep *,
    XrXirValueAdmission *,XrXirValue *,XrXirFaultDetail *);
#endif // XXIR_VALUE_OBSERVE_INTERNAL_H
