/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */
#ifndef XMODULE_GRAPH_RUNTIME_H
#define XMODULE_GRAPH_RUNTIME_H
#include "xmodule_graph.h"
/* Runtime preload owns the returned table, whose elements remain borrowed. */
XR_FUNC bool xr_module_graph_preload(struct XrVMRuntime *runtime, const XrModuleGraph *graph,
    struct XrModule ***output);
#endif
