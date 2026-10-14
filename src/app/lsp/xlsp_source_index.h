/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef XLSP_SOURCE_INDEX_H
#define XLSP_SOURCE_INDEX_H
#include "xlsp_source_syntax.h"
typedef struct XlspSourceIndex XlspSourceIndex;
typedef struct XlspSourceIndexSymbol {
    const char *name;
    XrLspSymbolKind kind;
    XrLspRange selection;
    bool exported;
} XlspSourceIndexSymbol;
/* A display index copies only actual parser declarations. It carries no type,
 * call or execution authority. A recoverable parse can publish its real names;
 * allocator/budget failure preserves output. All names/URI/version survive
 * caller text, Session, syntax owner and external resource-lease destruction. */
XR_FUNC XrXirStatus xlsp_source_index_build(XrCompileResources *,const XlspSourceDocument *,XlspSourceIndex **);
XR_FUNC void xlsp_source_index_free(XlspSourceIndex *);
XR_FUNC const XlspSourceIndexSymbol *xlsp_source_index_symbols(const XlspSourceIndex *,size_t *);
XR_FUNC const char *xlsp_source_index_uri(const XlspSourceIndex *);
XR_FUNC int64_t xlsp_source_index_version(const XlspSourceIndex *);
XR_FUNC XrParseStatus xlsp_source_index_parse_status(const XlspSourceIndex *);
#endif // XLSP_SOURCE_INDEX_H
