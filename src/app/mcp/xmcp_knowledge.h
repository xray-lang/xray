/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_knowledge.h - Knowledge base for MCP tools
 *
 * KEY CONCEPT:
 *   Indexes the language specification (by topic) and standard library
 *   (by module) so that xray_syntax_lookup and xray_stdlib_search
 *   can return relevant information to AI assistants.
 */

#ifndef XMCP_KNOWLEDGE_H
#define XMCP_KNOWLEDGE_H

#include "../../base/xdefs.h"

#define XMCP_MAX_TOPICS  64
#define XMCP_MAX_MODULES 32

/* A single topic entry (e.g., "channel" -> section text) */
typedef struct XmcpTopic {
    const char *name;       /* Topic key (e.g., "channel") */
    const char *aliases;    /* Comma-separated aliases (e.g., "chan,Channel") */
    const char *content;    /* Markdown content for this topic */
} XmcpTopic;

/* A standard library module entry */
typedef struct XmcpModule {
    const char *name;       /* Module name (e.g., "http") */
    const char *description;/* Brief description */
} XmcpModule;

/* Knowledge base */
typedef struct XmcpKnowledge {
    XmcpTopic  topics[XMCP_MAX_TOPICS];
    int        topic_count;

    XmcpModule modules[XMCP_MAX_MODULES];
    int        module_count;
} XmcpKnowledge;

/* Create an empty knowledge base. */
XR_FUNC XmcpKnowledge *xmcp_knowledge_new(void);

/* Load built-in knowledge (syntax spec + stdlib modules). */
XR_FUNC void xmcp_knowledge_load(XmcpKnowledge *kb);

/* Free the knowledge base. */
XR_FUNC void xmcp_knowledge_free(XmcpKnowledge *kb);

/* Look up a syntax topic. Returns the content string or NULL. */
XR_FUNC const char *xmcp_knowledge_lookup_topic(XmcpKnowledge *kb, const char *query);

/* Search stdlib modules. Returns a formatted string (caller must xr_free). */
XR_FUNC char *xmcp_knowledge_search_stdlib(XmcpKnowledge *kb, const char *query,
                                            const char *module_filter);

/* Get the cheatsheet resource content. */
XR_FUNC const char *xmcp_knowledge_get_cheatsheet(void);

/* Get the concurrency model resource content. */
XR_FUNC const char *xmcp_knowledge_get_concurrency(void);

/* Get the stdlib modules list resource content. */
XR_FUNC const char *xmcp_knowledge_get_stdlib_list(void);

#endif /* XMCP_KNOWLEDGE_H */
