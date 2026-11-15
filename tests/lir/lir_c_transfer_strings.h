/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_c_transfer_strings.h - Host fixture for real typed String ownership leaves
 */
#ifndef LIR_C_TRANSFER_STRINGS_H
#define LIR_C_TRANSFER_STRINGS_H
#include "xray_native_managed_root.h"
XRAY_API XrtManagedStatus lir_c_transfer_strings_init(XrtBillingOwner *);
XRAY_API void lir_c_transfer_strings_drop(void);
XRAY_API XrtManagedStatus lir_c_transfer_string_make(XrtManagedFrame *, size_t, int, XrtManagedHeader **);
XRAY_API bool lir_c_transfer_string_matches(const XrtManagedHeader *, int);
#endif // LIR_C_TRANSFER_STRINGS_H
