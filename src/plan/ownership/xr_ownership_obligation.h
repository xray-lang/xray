/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_ownership_obligation.h - Semantic ownership obligation construction
 */

#ifndef XR_OWNERSHIP_OBLIGATION_H
#define XR_OWNERSHIP_OBLIGATION_H

#include "xr_ownership_certificate.h"
#include <stdbool.h>
#include <stddef.h>

struct XrSemanticPlan;

XR_FUNC bool xr_ownership_certificate_build(struct XrSemanticPlan *plan,
                                            XrOwnershipCertificate **out, char *error,
                                            size_t error_size);

#endif  // XR_OWNERSHIP_OBLIGATION_H
