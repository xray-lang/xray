/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_container.h - AOT typed container provenance plan
 */

#ifndef XAOT_CONTAINER_H
#define XAOT_CONTAINER_H

#include "xaot_rep.h"
#include "../base/xdefs.h"
#include "../runtime/value/xtype.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum XaotContainerKind {
    XAOT_CONTAINER_ARRAY = 0,
    XAOT_CONTAINER_MAP,
    XAOT_CONTAINER_SET,
} XaotContainerKind;

enum {
    XAOT_CONTAINER_TYPED_STORAGE = 1u << 0,
    XAOT_CONTAINER_DIRECT_HELPERS = 1u << 1,
    XAOT_CONTAINER_RAW_DATA = 1u << 2,
};

typedef struct XaotContainerElemPlan {
    const XrType *type;
    XaotRep rep;
    XrRep storage_rep;
    const char *elem_name;
    const char *c_type;
} XaotContainerElemPlan;

typedef enum XaotTypeKeyKind {
    XAOT_TYPE_KEY_NONE = 0,
    XAOT_TYPE_KEY_CONTAINER = 1,
} XaotTypeKeyKind;

typedef struct XaotTypeKey {
    XaotTypeKeyKind kind;
    XaotContainerKind container_kind;
    XaotRep elem_rep;
    XaotRep key_rep;
    XaotRep value_rep;
    XrRep elem_storage_rep;
    XrRep key_storage_rep;
    XrRep value_storage_rep;
    uint64_t fingerprint;
} XaotTypeKey;

typedef struct XaotContainerPlan {
    const XrType *type;
    XaotTypeKey type_key;
    XaotContainerKind kind;
    uint32_t flags;
    XaotContainerElemPlan elem;
    XaotContainerElemPlan key;
    XaotContainerElemPlan value;
} XaotContainerPlan;

XR_FUNC bool xaot_container_elem_plan_for_type(const XrType *type, XaotContainerElemPlan *out);
XR_FUNC bool xaot_container_plan_for_type(const XrType *type, XaotContainerPlan *out);
XR_FUNC bool xaot_container_plan_matches_type(const XaotContainerPlan *plan, const XrType *type);
XR_FUNC bool xaot_type_key_equal(const XaotTypeKey *a, const XaotTypeKey *b);
XR_FUNC const char *xaot_container_kind_name(XaotContainerKind kind);

#endif  // XAOT_CONTAINER_H
