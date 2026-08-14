/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_coro_exception_verify.h - Independent coroutine continuation verifier
 */

#ifndef XI_CORO_EXCEPTION_VERIFY_H
#define XI_CORO_EXCEPTION_VERIFY_H

#include "xi_coro_analyze.h"

XR_FUNC bool xi_coro_exception_verify(const XiFunc *func, const XiCoroPlan *plan, char *error,
                                      size_t error_size);

#endif  // XI_CORO_EXCEPTION_VERIFY_H
