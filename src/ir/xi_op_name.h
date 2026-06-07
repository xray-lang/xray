/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_op_name.h - Human-readable Xi op names for diagnostics
 *
 * Pure lookup with no side effects. Usable from dump, verify, and
 * any diagnostic context that needs to print an op name.
 */

#ifndef XI_OP_NAME_H
#define XI_OP_NAME_H

#include "xi_ops_gen.h"

static inline const char *xi_op_name(uint16_t op) {
    return xi_generated_op_name(op);
}

#endif  // XI_OP_NAME_H
