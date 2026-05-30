/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_block_layout.h - Profile-guided block reordering
 *
 * KEY CONCEPT:
 *   Reorders basic blocks so that hot (frequently executed) blocks
 *   appear earlier / contiguously.  This reduces icache misses and
 *   improves branch prediction by keeping fall-through paths hot.
 *
 *   When block frequency data is available (from VM profiling),
 *   blocks are sorted by frequency in the chain-formation style.
 *   When no profile is available, a static heuristic (RPO order
 *   with loop bodies contiguous) is used as a best-effort layout.
 */

#ifndef XI_BLOCK_LAYOUT_H
#define XI_BLOCK_LAYOUT_H

#include "xi_pass.h"

XR_FUNC XiPassChange xi_opt_block_layout(XiFunc *f);

#endif /* XI_BLOCK_LAYOUT_H */
