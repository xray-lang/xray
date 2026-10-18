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
/* Borrow the original retained first-failure state for parser-owned syntax
 * formatting. The snapshot must stay live; callers never release or reset it. */
XR_FUNC XrCompileState *xlsp_source_syntax_state(const XlspSyntaxSnapshot *);

XR_FUNC const XlspSyntaxDiagnostic *xlsp_source_syntax_diagnostics(const XlspSyntaxSnapshot *);
XR_FUNC XrParseStatus xlsp_source_syntax_status(const XlspSyntaxSnapshot *);
XR_FUNC XrXirStatus xlsp_source_syntax_folding_json(const XlspSyntaxSnapshot *,XrJsonValue **);
XR_FUNC XrXirStatus xlsp_source_syntax_diagnostics_json(const XlspSyntaxSnapshot *,XrJsonValue **);
/* Exact parser-owned argument interval, excluding delimiters. The closing
 * delimiter position is selectable; the position after it is outside the call.
 * The innermost reachable completed call wins. A trailing comma can select an
 * argument beyond argument_count. These facts confer no semantic type authority.
 * All positions are strict UTF-16; failure preserves the complete output. */
typedef struct XlspSourceCallSyntax {
    XrLspRange arguments,callee;
    size_t active_argument,argument_count;
    bool has_callee;
} XlspSourceCallSyntax;
XR_FUNC XrXirStatus xlsp_source_syntax_call(const XlspSyntaxSnapshot *,XrLspPosition,XlspSourceCallSyntax *);
typedef struct XlspSourceArgumentSyntax {
    XrLspRange range,variable;
    bool has_variable;
} XlspSourceArgumentSyntax;
typedef struct XlspSourceArgumentCall {
    XlspSourceCallSyntax call;
    const XlspSourceArgumentSyntax *arguments;
} XlspSourceArgumentCall;
typedef struct XlspSourceCallArguments XlspSourceCallArguments;
typedef struct XlspSourceCallArgumentsView {
    XrCompileResources *resources;
    const char *uri;
    size_t uri_length,source_length;
    int64_t version;
    XrFingerprint fingerprint;
    const XlspSourceArgumentCall *calls;
    size_t count;
} XlspSourceCallArgumentsView;
/* Independently owned numeric facts for reachable completed calls. Complete
 * argument token ranges include explicit markers and parentheses. A variable
 * selection identifies an actual variable AST node, never guessed source text.
 * Canonical content identity binds facts to the exact document and version.
 * The owner pins the original ledger after syntax/Session/input death.
 * Failure preserves output and does not reset the original resource ledger. */
XR_FUNC XrXirStatus xlsp_source_syntax_arguments(const XlspSyntaxSnapshot *,XlspSourceCallArguments **);
XR_FUNC const XlspSourceCallArgumentsView *xlsp_source_call_arguments_view(const XlspSourceCallArguments *);
XR_FUNC void xlsp_source_call_arguments_free(XlspSourceCallArguments *);
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
