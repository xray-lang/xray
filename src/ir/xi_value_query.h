/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_value_query.h - Backend-neutral IR value/type classification predicates
 *
 * KEY CONCEPT:
 *   Classify an XiValue by the runtime type it carries (channel / task /
 *   work queue / unknown).  The value-level predicates unwrap identity casts
 *   (BOX/UNBOX/COPY) and the type-level predicates recurse through union
 *   members, so a value typed `Channel | null` is still recognized as a
 *   channel.
 *
 *   The type a value carries is an IR-level fact, so these belong in the IR
 *   layer where both the AOT compiler and the VM can consume them without
 *   each backend re-implementing the recognition logic.
 */

#ifndef XI_VALUE_QUERY_H
#define XI_VALUE_QUERY_H

#include "xi.h"

/* Type-level predicates (union-recursive). 'type' may be NULL. */
XR_FUNC bool xi_type_is_channel(const struct XrType *type);
XR_FUNC bool xi_type_is_named_instance(const struct XrType *type, const char *name);
XR_FUNC bool xi_type_is_task(const struct XrType *type);
XR_FUNC bool xi_type_is_thread(const struct XrType *type);

/* Value-level predicates: unwrap BOX/UNBOX/COPY, then test the carried type. */
XR_FUNC bool xi_value_type_is_channel(const XiValue *v);
XR_FUNC bool xi_value_type_is_task(const XiValue *v);
XR_FUNC bool xi_value_type_is_thread(const XiValue *v);
XR_FUNC bool xi_value_type_is_atomic(const XiValue *v);
XR_FUNC bool xi_value_type_is_work_queue(const XiValue *v);
XR_FUNC bool xi_value_type_is_result_group(const XiValue *v);
XR_FUNC bool xi_value_type_is_countdown_latch(const XiValue *v);
XR_FUNC bool xi_value_type_is_semaphore(const XiValue *v);
XR_FUNC bool xi_value_type_is_event_count(const XiValue *v);
XR_FUNC bool xi_value_type_is_unknown(const XiValue *v);

/* Numeric facts at a use site.
 * Uses existing range annotations when available and, if needed, dominating
 * branch guards on the path to 'site'. */
XR_FUNC bool xi_value_known_positive_at(const XiFunc *f, const XiValue *value, const XiBlock *site);
XR_FUNC bool xi_value_known_nonnegative_at(const XiFunc *f, const XiValue *value,
                                           const XiBlock *site);
XR_FUNC bool xi_value_known_ge_at(const XiFunc *f, const XiValue *value, const XiBlock *site,
                                  int64_t lower_bound);

#endif  // XI_VALUE_QUERY_H
