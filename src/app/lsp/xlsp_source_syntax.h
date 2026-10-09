/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef XLSP_SOURCE_SYNTAX_H
#define XLSP_SOURCE_SYNTAX_H
#include "xlsp_source_query.h"
#include "../../frontend/parser/xparse.h"
#include "../../base/xjson.h"
typedef struct XlspSyntaxSnapshot XlspSyntaxSnapshot;
typedef struct XlspSyntaxDiagnostic {
    int line,column,end_line,end_column; /* Exact parser 1-based byte spans. */
    const char *message;
    const struct XlspSyntaxDiagnostic *next;
} XlspSyntaxDiagnostic;
/* Owns bounded source, URI, version, partial syntax arena and diagnostics.
 * OK or RECOVERED publishes; allocator/budget failure never publishes.
 * The resulting owner outlives its original Session and input buffers. */
XR_FUNC XrParseStatus xlsp_source_syntax_build(XrCompilerSession *,const XlspSourceDocument *,XlspSyntaxSnapshot **);
XR_FUNC void xlsp_source_syntax_free(XlspSyntaxSnapshot *);
XR_FUNC const AstNode *xlsp_source_syntax_ast(const XlspSyntaxSnapshot *);
XR_FUNC const XlspSyntaxDiagnostic *xlsp_source_syntax_diagnostics(const XlspSyntaxSnapshot *);
XR_FUNC XrParseStatus xlsp_source_syntax_status(const XlspSyntaxSnapshot *);
XR_FUNC XrXirStatus xlsp_source_syntax_folding_json(const XlspSyntaxSnapshot *,XrJsonValue **);
/* Owned outline is a syntactic projection, independent of semantic success.
 * Name/span facts come exclusively from this modern parser owner. Output and
 * every name survive syntax/Session/input death. Parent UINT32_MAX means root. */
typedef struct XlspSourceSymbol {
    const char *name;
    uint32_t kind,parent;
    XrLspRange range,selection;
} XlspSourceSymbol;
typedef struct XlspSourceOutline XlspSourceOutline;
XR_FUNC XrXirStatus xlsp_source_syntax_outline(const XlspSyntaxSnapshot *,XlspSourceOutline **);
XR_FUNC const XlspSourceSymbol *xlsp_source_outline_symbols(const XlspSourceOutline *,size_t *);
XR_FUNC void xlsp_source_outline_free(XlspSourceOutline *);
XR_FUNC XrXirStatus xlsp_source_outline_json(const XlspSourceOutline *,XrJsonValue **);
#endif
