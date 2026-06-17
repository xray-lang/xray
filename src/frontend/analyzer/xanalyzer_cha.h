/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_cha.h - Class Hierarchy Analysis for static devirtualization
 *
 * KEY CONCEPT:
 *   Builds a whole-program class hierarchy from XrClassInfo metadata.
 *   Each class node tracks its direct subclasses, enabling leaf-class
 *   and single-implementor queries that the devirt pass uses to convert
 *   XI_CALL_METHOD → XI_CALL_METHOD_DIRECT.
 *
 * THREAD SAFETY:
 *   The CHA structure is built once during module analysis and is
 *   read-only thereafter.  Analysis and codegen threads can query it
 *   concurrently without synchronization.
 *
 * INVALIDATION:
 *   When a new class is loaded dynamically (e.g. via `import`), the
 *   entire CHA must be rebuilt.  The invalidation version counter
 *   lets consumers detect stale results.
 */

#ifndef XANALYZER_CHA_H
#define XANALYZER_CHA_H

#include "../../base/xdefs.h"
#include "../../runtime/class/xclass_info.h"
#include <stdbool.h>
#include <stdint.h>

/* Maximum direct subclasses tracked per class. */
#define XA_CHA_MAX_SUBCLASSES 64

/* CHA node: one per class in the hierarchy. */
typedef struct XaChaNode {
    XrClassInfo *info;
    struct XaChaNode *parent;
    struct XaChaNode **children;
    uint32_t nchildren;
    uint32_t children_cap;
    bool is_leaf;
} XaChaNode;

/* Top-level CHA structure. */
typedef struct XaClassHierarchy {
    XaChaNode *nodes;
    uint32_t nnodes;
    uint32_t nodes_cap;
    uint32_t version;
} XaClassHierarchy;

/* Build or rebuild the CHA from an array of class infos.
 * Returns true on success. The CHA takes ownership of the
 * internal storage; caller retains ownership of the infos. */
XR_FUNC bool xa_cha_build(XaClassHierarchy *cha, XrClassInfo **infos, uint32_t ninfos);

/* Free all internal CHA storage. */
XR_FUNC void xa_cha_free(XaClassHierarchy *cha);

/* Query: is the class a leaf (no subclasses)? */
XR_FUNC bool xa_cha_is_leaf(const XaClassHierarchy *cha, const XrClassInfo *info);

/* Query: does the class have exactly one direct implementor of
 * the given method name?  Returns the implementor class or NULL. */
XR_FUNC const XrClassInfo *xa_cha_single_implementor(const XaClassHierarchy *cha,
                                                     const XrClassInfo *info,
                                                     const char *method_name);

/* Query: how many subclasses does this class have (recursively)? */
XR_FUNC uint32_t xa_cha_subclass_count(const XaClassHierarchy *cha, const XrClassInfo *info);

/* Invalidate the CHA (e.g. after dynamic class loading). */
XR_FUNC void xa_cha_invalidate(XaClassHierarchy *cha);

/* Find the CHA node for a given class info. Returns NULL if not found. */
XR_FUNC XaChaNode *xa_cha_find_node(const XaClassHierarchy *cha, const XrClassInfo *info);

#endif  // XANALYZER_CHA_H
