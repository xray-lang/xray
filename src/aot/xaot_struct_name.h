/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_struct_name.h - shared AOT native struct C type naming
 */

#ifndef XAOT_STRUCT_NAME_H
#define XAOT_STRUCT_NAME_H

#include "../runtime/value/xstruct_layout.h"
#include "../base/xdefs.h"
#include <stdint.h>
#include <stddef.h>

XR_FUNC uint64_t xaot_struct_layout_hash(const XrStructLayout *sl);
XR_FUNC void xaot_struct_c_type_name(char *buf, size_t buflen, const char *prefix,
                                     const XrStructLayout *sl);

#endif  // XAOT_STRUCT_NAME_H
