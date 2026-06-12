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

typedef enum XaotVerifyMode {
    XAOT_VERIFY_REP_READY = 0,
    XAOT_VERIFY_LAYOUT_READY,
    XAOT_VERIFY_ABI_READY,
    XAOT_VERIFY_BOUNDARY_READY,
    XAOT_VERIFY_AOT_READY,
} XaotVerifyMode;

XR_FUNC bool xaot_verify_bundle(const XaotBundle *bundle, XaotVerifyMode mode, char *errbuf,
                                size_t errbuf_len);

#endif  // XAOT_VERIFY_H
