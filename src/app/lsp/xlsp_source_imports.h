/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef XLSP_SOURCE_IMPORTS_H
#define XLSP_SOURCE_IMPORTS_H
#include "xlsp_source_buffer.h"

typedef struct XlspSourceImport {
    const char *specifier;
    XrLspRange range;
    bool quoted;
} XlspSourceImport;
typedef struct XlspSourceImports XlspSourceImports;
/* Immutable display observations from a successful modern parse. Specifiers
 * come from the AST; exact spelling spans come from the canonical lexer at
 * each AST declaration. No module resolution or semantic authority is implied.
 * Missing parser positions reject the complete result. The output pins the
 * supplied cumulative ledger after Session, AST and input destruction. */
XR_FUNC XrXirStatus xlsp_source_imports_build(XrCompileResources *,const XlspSourceDocument *,XlspSourceImports **);
XR_FUNC const XlspSourceImport *xlsp_source_imports_items(const XlspSourceImports *,size_t *);
XR_FUNC void xlsp_source_imports_free(XlspSourceImports *);
#endif // XLSP_SOURCE_IMPORTS_H
