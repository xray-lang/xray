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

/* Lower all high-level ops in the function tree to backend-legal form
 * for the backend verifier. Requires an exact XI_STAGE_REPPED input; the
 * consuming stage transition publishes XI_STAGE_BACKEND. */
XR_FUNC void xi_backend_lower(XiFunc *f);

#endif  // XI_BACKEND_LOWER_H
