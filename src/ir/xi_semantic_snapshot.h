/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_semantic_snapshot.h - Detach escaped Xi semantic metadata from the analyzer
 */

#ifndef XI_SEMANTIC_SNAPSHOT_H
#define XI_SEMANTIC_SNAPSHOT_H

#include "../base/xdefs.h"

#include <stddef.h>

struct XiFunc;

/*
 * Materialize every type and layout reachable by an Xi function tree in the
 * root Xi arena.  After this succeeds, the analyzer/type pool and source AST
 * may be destroyed while VM-retained IR and AOT planning continue to use the
 * snapshot.  The operation is fail-closed: false means the IR must not escape.
 */
XR_FUNC bool xi_semantic_snapshot_detach(struct XiFunc *root);
XR_FUNC bool xi_semantic_snapshot_detach_ex(struct XiFunc *root, char *error,
                                             size_t error_size);

#endif  // XI_SEMANTIC_SNAPSHOT_H
