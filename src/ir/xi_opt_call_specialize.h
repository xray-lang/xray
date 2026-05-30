/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_call_specialize.h - Constant-argument call specialization
 */

#ifndef XI_OPT_CALL_SPECIALIZE_H
#define XI_OPT_CALL_SPECIALIZE_H

#include "xi_pass.h"

XR_FUNC XiPassChange xi_opt_call_specialize(XiFunc *f);

#endif /* XI_OPT_CALL_SPECIALIZE_H */
