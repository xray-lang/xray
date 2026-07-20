/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_callable.h - Closed-world function-value target and invoke plans
 */

#ifndef XAOT_CALLABLE_H
#define XAOT_CALLABLE_H

#include "xaot_bundle.h"

/* Computes a monotone, whole-bundle function-value flow fixed point and
 * publishes one immutable invoke plan for every Xi function-value call. */
XR_FUNC bool xaot_callable_plans_build(XaotBundle *bundle);

/* Structural verifier for the published plan.  Semantic re-derivation is
 * intentionally kept in this module so prepare/verifier cannot drift. */
XR_FUNC bool xaot_callable_plans_verify(const XaotBundle *bundle, char *errbuf, size_t errbuf_len);

/* Generic templates have no standalone ABI.  This returns true only for
 * ordinary concrete bodies or for a template explicitly selected by a
 * verified generic-body plan. */
XR_FUNC bool xaot_callable_func_has_executable_body_plan(const XaotBundle *bundle,
                                                         const XiFunc *func);

#endif /* XAOT_CALLABLE_H */
