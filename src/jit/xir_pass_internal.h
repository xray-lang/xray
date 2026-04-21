/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_pass_internal.h - Internal shared declarations for XIR optimization passes
 */

#ifndef XIR_PASS_INTERNAL_H
#define XIR_PASS_INTERNAL_H

#include "xir_pass.h"
#include "xir_defuse.h"
#include "xir_builder.h"
#include "xir_offsets.h"
#include "../runtime/value/xchunk.h"
#include "../base/xmalloc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../base/xdefs.h"

// xir_op_is_pure, xir_compute_idom now declared in xir.h

#endif // XIR_PASS_INTERNAL_H
