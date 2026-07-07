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

typedef struct XiEnumData XiEnumData;
typedef struct XrType XrType;

XR_FUNC uint64_t xaot_struct_layout_hash(const XrAggregateLayout *sl);
XR_FUNC void xaot_struct_c_type_name(char *buf, size_t buflen, const char *prefix,
                                     const XrAggregateLayout *sl);
XR_FUNC uint64_t xaot_enum_data_hash(const XiEnumData *ed);
XR_FUNC void xaot_enum_c_type_name(char *buf, size_t buflen, const char *prefix,
                                   const XiEnumData *ed);
XR_FUNC uint64_t xaot_type_fingerprint(const XrType *type);
XR_FUNC void xaot_enum_c_type_name_for_type(char *buf, size_t buflen, const char *prefix,
                                            const XiEnumData *ed, const XrType *type);

#endif  // XAOT_STRUCT_NAME_H
