/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_ownership_replay.h - Independent ordered ownership/liveness replay
 */

#ifndef XR_OWNERSHIP_REPLAY_H
#define XR_OWNERSHIP_REPLAY_H

#include "../../base/xdefs.h"
#include <stdbool.h>
#include <stddef.h>

struct XrSemanticGraph;
struct XrSemanticPlan;

XR_FUNC bool xr_ownership_replay_check(const struct XrSemanticPlan *plan,
                                       const struct XrSemanticGraph *graph, char *error,
                                       size_t error_size);

#endif  // XR_OWNERSHIP_REPLAY_H
