/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_imported_constructor_normalize.h - Unique grounded constructor producer
 */
#ifndef XI_IMPORTED_CONSTRUCTOR_NORMALIZE_H
#define XI_IMPORTED_CONSTRUCTOR_NORMALIZE_H

#include "xi_pipeline.h"

/* Internal producer-only transaction, before derived plans are frozen. */
XR_FUNC bool xi_imported_constructor_normalize(struct XiFunc *root,
                                                struct XrVMRuntime *runtime,
                                                const XiPipelineConfig *config);

#endif  // XI_IMPORTED_CONSTRUCTOR_NORMALIZE_H
