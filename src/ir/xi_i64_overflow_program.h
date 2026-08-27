/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */

#ifndef XI_I64_OVERFLOW_PROGRAM_H
#define XI_I64_OVERFLOW_PROGRAM_H

#include "xi_module.h"

struct XrTargetProfile;

XR_FUNC bool xi_i64_overflow_program_verify(const XiModule *module,
                                             const struct XrTargetProfile *target_profile,
                                             char *error, size_t error_size);

#endif  // XI_I64_OVERFLOW_PROGRAM_H
