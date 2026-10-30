/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_internal.h - Complete immutable bytecode admission without execution
 */
#ifndef XRBC_INTERNAL_H
#define XRBC_INTERNAL_H

#include "xrbc.h"

/* Both compiler publication and preparation use this complete receiver.
 * It does not allocate an execution or retain the borrowed module/locations. */
XR_FUNC XrRbcStatus xr_rbc_module_admit(const XrRbcModule *, const XrtLocationOwner *);

#endif // XRBC_INTERNAL_H
