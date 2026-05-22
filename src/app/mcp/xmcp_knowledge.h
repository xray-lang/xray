/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_knowledge.h - Search and lookup facade for MCP knowledge data
 *
 * KEY CONCEPT:
 *   The authored language and standard-library knowledge is generated into
 *   immutable C tables. This layer owns lookup, ranking, formatted fallback
 *   text, and the small in-memory index used by MCP tools/resources.
 */

#ifndef XMCP_KNOWLEDGE_H
#define XMCP_KNOWLEDGE_H

#include "../../base/xdefs.h"
#include "xmcp_knowledge_generated.h"

#define XMCP_MAX_TOPICS 128
#define XMCP_MAX_MODULES 64
#define XMCP_STDLIB_MAX_MATCHES 32

typedef struct XmcpTopic {
    const char *name;
    const char *title;
    const char *aliases;
    const char *content;
} XmcpTopic;

typedef struct XmcpModule {
    const char *name;
    const char *summary;
    const char *body;
    const XmcpGeneratedStdlibSymbol *symbols;
    int symbol_count;
} XmcpModule;

typedef struct XmcpStdlibMatch {
    const XmcpModule *module;
    const XmcpGeneratedStdlibSymbol *symbol;
    int score;
} XmcpStdlibMatch;

typedef struct XmcpStdlibSearchResult {
    XmcpStdlibMatch matches[XMCP_STDLIB_MAX_MATCHES];
    int match_count;
} XmcpStdlibSearchResult;

typedef struct XmcpKnowledge {
    XmcpTopic topics[XMCP_MAX_TOPICS];
    int topic_count;

    XmcpModule modules[XMCP_MAX_MODULES];
    int module_count;
} XmcpKnowledge;

XR_FUNC XmcpKnowledge *xmcp_knowledge_new(void);
XR_FUNC void xmcp_knowledge_load(XmcpKnowledge *kb);
XR_FUNC void xmcp_knowledge_free(XmcpKnowledge *kb);

XR_FUNC const char *xmcp_knowledge_lookup_topic(XmcpKnowledge *kb, const char *query);
XR_FUNC char *xmcp_knowledge_list_topics(XmcpKnowledge *kb);

XR_FUNC void xmcp_knowledge_search_stdlib_matches(XmcpKnowledge *kb, const char *query,
                                                  const char *module_filter,
                                                  XmcpStdlibSearchResult *out);
XR_FUNC char *xmcp_knowledge_search_stdlib(XmcpKnowledge *kb, const char *query,
                                           const char *module_filter, int *match_count);

XR_FUNC const char *xmcp_knowledge_get_cheatsheet(void);
XR_FUNC const char *xmcp_knowledge_get_concurrency(void);
XR_FUNC const char *xmcp_knowledge_get_stdlib_list(void);

#endif /* XMCP_KNOWLEDGE_H */
