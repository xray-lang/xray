/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_backend_lower.h - Lower Xi IR to backend-legal form
 */

#ifndef XI_BACKEND_LOWER_H
#define XI_BACKEND_LOWER_H

#include "xi.h"

/* Lower all high-level ops in the function tree to backend-legal
 * form (XI_CALL_BUILTIN) and advance stage to XI_STAGE_BACKEND.
 * Requires: f->stage >= XI_STAGE_REPPED.
 * Idempotent: no-op if already at STAGE_BACKEND. */
XR_FUNC void xi_backend_lower(XiFunc *f);

#endif  // XI_BACKEND_LOWER_H
