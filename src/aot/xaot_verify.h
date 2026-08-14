/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_verify.h - AOT plan verifier
 */

#ifndef XAOT_VERIFY_H
#define XAOT_VERIFY_H

#include "xaot_bundle.h"
#include "../base/xdefs.h"
#include <stdbool.h>
#include <stddef.h>

XR_FUNC bool xaot_verify_bundle(const XaotBundle *bundle, char *errbuf, size_t errbuf_len);

/* Verify only the immutable AOT projection derived from XgGlobalEvidence. */
XR_FUNC bool xaot_verify_global_evidence_plan(const XaotBundle *bundle, char *errbuf,
                                              size_t errbuf_len);

/* Verify one enum plan without asserting that its bundle is executable-ready. */
XR_FUNC bool xaot_verify_enum_plan(const XaotBundle *bundle, const XaotEnumPlan *plan,
                                   char *errbuf, size_t errbuf_len);

#endif  // XAOT_VERIFY_H
