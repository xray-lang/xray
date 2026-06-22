/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xobj_ops.c - Object type capability helpers.
 */

#include "xobj_ops.h"

XR_FUNC bool xr_obj_type_may_need_finalize(uint8_t type) {
    return type < XR_OBJ_TYPE_MAX && type > XR_TATOMIC;
}
