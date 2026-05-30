/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cha.h - Build class hierarchy snapshots from Xi IR for devirtualization
 */

#ifndef XI_CHA_H
#define XI_CHA_H

#include "../frontend/analyzer/xanalyzer_cha.h"
#include "xi.h"
#include <stdbool.h>

/* Collect class infos from f, snapshot them, and build CHA.
 * On success, *out_copies holds heap storage referenced by CHA nodes; the
 * caller must xr_free(*out_copies) after xa_cha_free(out). */
XR_FUNC bool xi_cha_build_for_func(const XiFunc *f, XaClassHierarchy *out,
                                   XrClassInfo **out_copies);

/* Find the CHA snapshot node for an original class info (match by name). */
XR_FUNC const XrClassInfo *xi_cha_snapshot_info(const XaClassHierarchy *cha,
                                                const XrClassInfo *info);

#endif /* XI_CHA_H */
