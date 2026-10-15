/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_type_facts.h - Source declaration identities in verified type tables
 *
 * KEY CONCEPT:
 *   Local table ordinals and real syntax bounds are immutable observations.
 */
#ifndef XXIR_SOURCE_TYPE_FACTS_H
#define XXIR_SOURCE_TYPE_FACTS_H
#include "xxir_source_query.h"
typedef enum XrXirSourceTypeKind {
    XR_XIR_SOURCE_TYPE_NONE, XR_XIR_SOURCE_TYPE_STRUCT, XR_XIR_SOURCE_TYPE_ENUM,
    XR_XIR_SOURCE_TYPE_CLASS, XR_XIR_SOURCE_TYPE_INTERFACE
} XrXirSourceTypeKind;
typedef struct XrXirSourceTypeFact {
    XrXirSourceTypeKind kind;
    uint32_t table_index;
    XrXirSourceRange range;
    bool range_known;
} XrXirSourceTypeFact;
typedef struct XrXirSourceTypeFacts {
    /* Indexed by SourceDeclaration.id - 1. NONE has no table identity.
     * INTERFACE indexes SourceView.types->interfaces; other non-NONE kinds
     * index SourceView.types->nominals. Ordinals are local to this snapshot.
     * Missing real parser bounds remain unknown with a zero range. */
    const XrXirSourceTypeFact *declarations;
    uint32_t declaration_count;
} XrXirSourceTypeFacts;
/* NULL means the snapshot did not receive this observation. Records belong to
 * the snapshot after all producer lifetimes end. They grant no construction,
 * conformance, dispatch or execution authority and carry no borrowed AST. */
XR_FUNC const XrXirSourceTypeFacts *xr_xir_compile_source_snapshot_type_facts(const XrXirSourceSnapshot *);
#endif // XXIR_SOURCE_TYPE_FACTS_H
