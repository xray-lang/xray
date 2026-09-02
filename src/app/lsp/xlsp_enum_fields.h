/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_enum_fields.h - Semantic identity for named enum payload fields
 */

#ifndef XLSP_ENUM_FIELDS_H
#define XLSP_ENUM_FIELDS_H

#include "xlsp_types.h"
#include "../../base/xdefs.h"
#include "../../base/xlocation.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct AstNode AstNode;
typedef struct XaAnalyzer XaAnalyzer;
typedef struct XrType XrType;

/* A payload field is nominally identified. Its spelling alone is never a
 * field identity because unrelated variants may reuse the same name. */
typedef struct XlspEnumFieldIdentity {
    uint32_t enum_symbol_id;
    uint16_t variant_ordinal;
    uint16_t field_slot;
} XlspEnumFieldIdentity;

typedef struct XlspEnumFieldOccurrence {
    XlspEnumFieldIdentity identity;
    const char *enum_name;
    const char *variant_name;
    const char *field_name;
    XrType *field_type;
    XrLocation location;
    bool is_declaration;
} XlspEnumFieldOccurrence;

typedef void (*XlspEnumFieldVisitFn)(const XlspEnumFieldOccurrence *occurrence, void *ctx);

XR_FUNC bool xlsp_enum_field_identity_equal(XlspEnumFieldIdentity left,
                                            XlspEnumFieldIdentity right);
XR_FUNC bool xlsp_enum_field_at(XaAnalyzer *analyzer, AstNode *root, const char *document_uri,
                                XrLspPosition position, XlspEnumFieldOccurrence *out);
XR_FUNC bool xlsp_enum_field_definition(XaAnalyzer *analyzer,
                                        XlspEnumFieldIdentity identity,
                                        XlspEnumFieldOccurrence *out);
XR_FUNC void xlsp_visit_enum_field_occurrences(XaAnalyzer *analyzer, AstNode *root,
                                               const char *document_uri,
                                               XlspEnumFieldIdentity target,
                                               XlspEnumFieldVisitFn visit, void *ctx);

#endif  // XLSP_ENUM_FIELDS_H
