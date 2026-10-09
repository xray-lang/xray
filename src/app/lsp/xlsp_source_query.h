/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef XLSP_SOURCE_QUERY_H
#define XLSP_SOURCE_QUERY_H
#include "xlsp_types.h"
#include "../../xir/xxir_source.h"

/* Lengths exclude NUL; source text and URI need not have a terminator. The
 * synchronous caller excludes edits until build/matches returns. */
typedef struct XlspSourceDocument {
    const char *uri, *text;
    size_t uri_length, length;
    int64_t version;
} XlspSourceDocument;
typedef struct XlspSourceSnapshot XlspSourceSnapshot;
typedef struct XlspSourceLocation {
    const char *uri; /* Borrowed from the immutable snapshot. */
    XrLspRange range;
    int64_t version;
    XrXirSourceAccess access;
} XlspSourceLocation;
typedef struct XlspSourceLocations {
    XlspSourceLocation *items;
    size_t count;
} XlspSourceLocations;

/* Rooted file URIs only. The request's entry_path is derived from documents[entry]
 * under request->authority. All documents belong to that same authority.
 * A complete successful Checked query is required before publication. The
 * snapshot owns URI/text/version and the semantic snapshot after the caller,
 * compiler Session, Checked artifact and overlay producers have all died.
 * It represents this entry's import closure, not a workspace symbol index.
 * Failure preserves output and forwards the real Source diagnostic/path. */
XR_FUNC XrXirStatus xlsp_source_snapshot_build(const XrXirSourceRequest *request,
    const XlspSourceDocument *documents, size_t count, size_t entry,
    XlspSourceSnapshot **output, XrXirSourceDiagnostic *diagnostic, char **failure_path);
XR_FUNC void xlsp_source_snapshot_free(XlspSourceSnapshot *snapshot);
/* Exact input comparison, including ordering, entry index and every version.
 * Same version with changed bytes is stale too. Failure preserves matches. */
XR_FUNC XrXirStatus xlsp_source_snapshot_matches(const XlspSourceSnapshot *snapshot,
    const XlspSourceDocument *documents, size_t count, size_t entry, bool *matches);
/* Strict UTF-16 positions; no byte fallback, clamping or half-surrogate match.
 * Missing source text or symbol is UNRESOLVED. Locations borrow snapshot URI.
 * All calls charge the snapshot's original live compiler ledger. */
XR_FUNC XrXirStatus xlsp_source_definition(const XlspSourceSnapshot *snapshot,
    const char *uri, size_t uri_length, XrLspPosition position, XlspSourceLocation *output);
XR_FUNC XrXirStatus xlsp_source_references(const XlspSourceSnapshot *snapshot,
    const char *uri, size_t uri_length, XrLspPosition position, bool include_declaration,
    XlspSourceLocations *output);
XR_FUNC void xlsp_source_locations_free(XlspSourceLocations *locations);
/* Common bounded URI decoder; output is compiler-owned, independently pinned. */
XR_FUNC XrXirStatus xlsp_source_uri_path(XrCompileResources *,const char *,size_t,char **);
/* Each document has its real independently discovered authority. The Source
 * resolver performs all cross-authority and Catalog admission. */
XR_FUNC XrXirStatus xlsp_source_snapshot_build_authorities(const XrXirSourceRequest *,
    const XlspSourceDocument *,const XrModuleIdentityAuthority *,size_t,size_t,
    XlspSourceSnapshot **,XrXirSourceDiagnostic *,char **);
XR_FUNC XrXirStatus xlsp_source_snapshot_contains(const XlspSourceSnapshot *,
    const char *,size_t,bool *);
/* Shared strict Source-byte to LSP UTF-16 conversion; text is bounded. */
XR_FUNC XrXirStatus xlsp_source_position(XrCompileResources *,const char *,size_t,int,int,XrLspPosition *);
/* Complete owned plain-text rendering of available Checked declaration facts.
 * The returned allocation pins the original ledger and survives snapshot death.
 * No invented generic names/docs/enum payload expressions; missing facts return
 * UNSUPPORTED or UNRESOLVED with output untouched. */
typedef struct XlspSourceHover {char *text;size_t length;} XlspSourceHover;
XR_FUNC XrXirStatus xlsp_source_hover(const XlspSourceSnapshot *,const char *,size_t,
    XrLspPosition,XlspSourceHover *);
XR_FUNC void xlsp_source_hover_free(XlspSourceHover *);
/* Token numbers use the unchanged semanticTokens legend; ranges are exact
 * UTF16 units. Opaque owner pins its ledger after every Source producer dies.
 * Missing admitted display facts reject the complete response, never silently
 * dropping that declaration. This is not a fallback lexical symbol index. */
typedef struct XlspSourceToken {uint32_t line,column,length,type,modifiers;} XlspSourceToken;
typedef struct XlspSourceTokens XlspSourceTokens;
XR_FUNC XrXirStatus xlsp_source_semantic_tokens(const XlspSourceSnapshot *,const char *,size_t,XlspSourceTokens **);
XR_FUNC XrXirStatus xlsp_source_semantic_range(const XlspSourceSnapshot *,const char *,size_t,XrLspRange);
XR_FUNC const XlspSourceToken *xlsp_source_tokens_items(const XlspSourceTokens *,size_t *);
XR_FUNC void xlsp_source_tokens_free(XlspSourceTokens *);
#endif
