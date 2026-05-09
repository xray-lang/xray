/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_selection.h - Member/method/index selection facts
 *
 * KEY CONCEPT:
 *   When the analyzer resolves a member access (obj.field), method call
 *   (obj.method()), index operation (arr[i]), or module export reference,
 *   it records an XaSelection fact keyed by the AST node_id of the access
 *   expression.  This fact captures:
 *     - selection kind (field, method, index, static, module export)
 *     - receiver type (type of the object/container)
 *     - target symbol (resolved member XaSymbol*)
 *     - field index (for direct-slot access without name lookup)
 *     - result type (type of the accessed value)
 *     - indirection flag (pointer dereference needed)
 *
 *   The lowerer and backends read these facts instead of re-discovering
 *   member resolution at each stage, eliminating duplicated logic and
 *   ensuring consistency across VM/JIT/AOT.
 *
 * OWNERSHIP:
 *   One selection table per XaAnalyzer, freed with the analyzer.
 *   Entries reference types/symbols owned by the analyzer's pools.
 */

#ifndef XA_SELECTION_H
#define XA_SELECTION_H

#include "../../base/xdefs.h"
#include <stdint.h>
#include <stdbool.h>

struct XrType;
struct XaSymbol;
struct AstNode;

/* What kind of member/access is this selection? */
typedef enum XaSelectionKind {
    XA_SEL_FIELD,         /* instance field access: obj.field */
    XA_SEL_METHOD,        /* method access: obj.method (before call) */
    XA_SEL_INDEX,         /* subscript/index access: arr[i] / map[k] */
    XA_SEL_STATIC_MEMBER, /* static member: Class.staticField */
    XA_SEL_MODULE_EXPORT, /* module namespace access: mod.exportedName */
    XA_SEL_ENUM_MEMBER,   /* enum member: Color.Red */
} XaSelectionKind;

/* A resolved selection fact for one AST node. */
typedef struct XaSelection {
    XaSelectionKind kind;
    struct XrType *receiver_type;   /* type of the object being accessed */
    struct XaSymbol *target_symbol; /* resolved member symbol (NULL if unresolved) */
    int32_t field_index;            /* direct slot index (-1 = needs name lookup) */
    struct XrType *result_type;     /* type of the selection result */
    bool is_indirect;               /* true if access goes through a pointer/cell */
    bool is_optional;               /* true if optional chain (obj?.field) */
} XaSelection;

/* Opaque selection table (node_id -> XaSelection). */
typedef struct XaSelectionTable XaSelectionTable;

/* Create / destroy a selection table. */
XR_FUNC XaSelectionTable *xa_selection_table_new(void);
XR_FUNC void xa_selection_table_free(XaSelectionTable *t);

/* Record a selection fact for the given AST node. Overwrites any
 * existing entry for the same node_id. */
XR_FUNC void xa_selection_table_set(XaSelectionTable *t, struct AstNode *node,
                                    const XaSelection *sel);

/* Retrieve the selection fact for a node, or NULL if none recorded. */
XR_FUNC const XaSelection *xa_selection_table_get(const XaSelectionTable *t,
                                                  const struct AstNode *node);

/* Drop all entries (keeps bucket array allocated). */
XR_FUNC void xa_selection_table_clear(XaSelectionTable *t);

/* Number of live entries (for invariant checks). */
XR_FUNC int xa_selection_table_size(const XaSelectionTable *t);

#endif  // XA_SELECTION_H
