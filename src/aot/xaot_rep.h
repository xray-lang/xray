/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_rep.h - AOT value representation plan
 */

#ifndef XAOT_REP_H
#define XAOT_REP_H

#include "xaot_rep_gen.h"
#include "../runtime/value/xtype.h"
#include "../base/xdefs.h"
#include <stdint.h>

struct XiValue;

typedef enum XaotValueKind {
    XAOT_VALUE_VOID = 0,
    XAOT_VALUE_SCALAR,
    XAOT_VALUE_TAGGED,
    XAOT_VALUE_PTR,
    XAOT_VALUE_AGGREGATE,
    XAOT_VALUE_VIEW,
} XaotValueKind;

typedef struct XaotValueRep {
    XaotValueKind kind;
    XaotRep rep;
    const XrType *type;
    const char *c_type;
    uint32_t flags;
} XaotValueRep;

XR_FUNC XaotValueRep xaot_value_rep_for_type(const XrType *type);
XR_FUNC XaotValueRep xaot_value_rep_for_value(const struct XiValue *value);
XR_FUNC XrRep xaot_value_storage_rep(XaotValueRep rep);
XR_FUNC const char *xaot_value_kind_name(XaotValueKind kind);

#endif  // XAOT_REP_H
