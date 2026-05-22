/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_knowledge_generated.h - Public types for the auto-generated
 * MCP knowledge tables produced by scripts/gen_mcp_knowledge.py.
 *
 * The arrays themselves live in xmcp_knowledge_generated.c. xmcp_knowledge.c
 * is the only consumer; it copies pointers into XmcpKnowledge at load time
 * and runs search/ranking on top of these immutable tables.
 *
 * Lifetime: all pointers in this file refer to string literals that live
 * for the duration of the process and are safe to expose without copying.
 */

#ifndef XMCP_KNOWLEDGE_GENERATED_H
#define XMCP_KNOWLEDGE_GENERATED_H

#include "../../base/xdefs.h"

/* A single syntax topic (channel, coroutine, class, ...). The lookup key
 * is `id`; `aliases_csv` is a comma-separated list of additional case-
 * insensitive substring keys consumed by xmcp_knowledge_lookup_topic. */
typedef struct XmcpGeneratedTopic {
    const char *id;
    const char *title;
    const char *aliases_csv;
    const char *body; /* Markdown returned verbatim to the caller. */
} XmcpGeneratedTopic;

/* A single stdlib symbol. Sourced from `xray builtin-dump`, not from the
 * Markdown docs, so signatures stay in lock-step with the analyzer's
 * builtin registry. `summary` is optional and may be empty. */
typedef struct XmcpGeneratedStdlibSymbol {
    const char *name;
    const char *signature;
    const char *summary;
} XmcpGeneratedStdlibSymbol;

/* A single stdlib module. `summary` is the one-line description used by
 * ranked search; `body` is the long-form Markdown shown by xray_definition
 * and the `xray://stdlib/<module>` resource. The `symbols` array is empty
 * when no builtin-dump JSON was supplied to the generator. */
typedef struct XmcpGeneratedStdlibEntry {
    const char *module;
    const char *summary;
    const char *body;
    const XmcpGeneratedStdlibSymbol *symbols;
    int symbol_count;
} XmcpGeneratedStdlibEntry;

/* Tables. Sentinel zero entries may exist at the tail when a category is
 * empty; consumers MUST honour the *_count companions, never iterate to
 * a NULL terminator. */
XR_DATA const XmcpGeneratedTopic xmcp_generated_topics[];
XR_DATA const int xmcp_generated_topic_count;

XR_DATA const XmcpGeneratedStdlibEntry xmcp_generated_stdlib[];
XR_DATA const int xmcp_generated_stdlib_count;

/* Long-form resource bodies served by xray://spec/cheatsheet,
 * xray://spec/topic/concurrency, and xray://stdlib/modules. Always
 * non-NULL — the generator emits an empty literal when the source file
 * is missing so callers never need a NULL check. */
XR_DATA const char xmcp_generated_cheatsheet[];
XR_DATA const char xmcp_generated_concurrency[];
XR_DATA const char xmcp_generated_stdlib_list[];

#endif /* XMCP_KNOWLEDGE_GENERATED_H */
