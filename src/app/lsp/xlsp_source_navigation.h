/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef XLSP_SOURCE_NAVIGATION_H
#define XLSP_SOURCE_NAVIGATION_H
#include "xlsp_source_query.h"
#include "../../base/xjson.h"
/* Publish a complete protocol-owned result or preserve output. Input snapshot
 * and URI are synchronous borrows. No semantic fallback, stale AST or lexer is
 * consulted. UNRESOLVED is a typed failure; callers must not hide it as empty
 * success. An actual unused declaration may have a successful empty refs array.
 * Returned JSON owns its strings independently of the query snapshot. */
XR_FUNC XrXirStatus xlsp_source_definition_json(const XlspSourceSnapshot *snapshot,
    const char *uri,size_t uri_length,XrLspPosition position,XrJsonValue **output);
XR_FUNC XrXirStatus xlsp_source_references_json(const XlspSourceSnapshot *snapshot,
    const char *uri,size_t uri_length,XrLspPosition position,bool include_declaration,
    XrJsonValue **output);
/* Union actual root closures, deduplicating URI/range under the same ledger.
 * Every closure containing the selected document must resolve the same target.
 * mode: 0 definition, 1 references, 2 same-document read/write highlights. */
XR_FUNC XrXirStatus xlsp_source_navigation_many_json(XrCompileResources *,
    XlspSourceSnapshot *const *,size_t,const char *,size_t,XrLspPosition,unsigned,bool,XrJsonValue **);
XR_FUNC XrXirStatus xlsp_source_hover_many_json(XrCompileResources *,
    XlspSourceSnapshot *const *,size_t,const char *,size_t,XrLspPosition,XrJsonValue **);
XR_FUNC XrXirStatus xlsp_source_tokens_json(XrCompileResources *,const XlspSourceTokens *,
    const XlspSourceTokens *,uint32_t,bool,const XrLspRange *,XrJsonValue **);
#endif
