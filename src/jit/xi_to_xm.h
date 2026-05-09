/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_to_xm.h - Xi IR to Xm lowering for JIT compilation
 *
 * Translates Xi SSA (XiFunc) directly to JIT SSA (XmFunc) without
 * going through bytecode reconstruction. Uses Xi IR's precise types,
 * existing SSA form, and the XiSlotMap for deopt snapshot generation.
 */

#ifndef XI_TO_XM_H
#define XI_TO_XM_H

#include "../ir/xi.h"

struct XmFunc;
struct XrProto;
struct XrayIsolate;
struct XrICFieldTable;
struct XrICMethodTable;

/* IC snapshot bundle for speculative optimization during lowering.
 * All pointers are read-only snapshots; NULL = no IC data available. */
typedef struct {
    struct XrICFieldTable *ic_fields;
    struct XrICMethodTable *ic_methods;
} XmICSnapshot;

/* Lower an Xi IR function to Xm for JIT compilation.
 * Returns a new XmFunc ready for the optimization pipeline.
 * Returns NULL on failure (unsupported ops, allocation failure).
 *
 * proto: associated bytecode proto (for IC feedback, deopt PCs)
 * slot_map: value_id → bytecode slot mapping (for deopt generation)
 * ic: IC snapshots for speculative guard insertion (may be NULL)
 * isolate: runtime context (may be NULL for unit tests) */
XR_FUNC struct XmFunc *xi_to_xm_lower(XiFunc *xi_func, struct XrProto *proto, XiSlotMap *slot_map,
                                      const XmICSnapshot *ic, struct XrayIsolate *isolate);

#endif  // XI_TO_XM_H
