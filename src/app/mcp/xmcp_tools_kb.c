/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_tools_kb.c - Knowledge-base MCP tools
 *
 * KEY CONCEPT:
 *   Three read-only tools backed by xmcp_knowledge:
 *     - xray_syntax_lookup   exact / fuzzy lookup of one syntax topic
 *     - xray_stdlib_search   ranked full-text search across stdlib modules
 *     - xray_definition      tries syntax topic, then stdlib search, then
 *                            "module.symbol" disambiguation
 *   The structured result helpers live here because nothing outside this
 *   trio needs them.
 */

#include "xmcp_tools_internal.h"
#include "xmcp_server.h"
#include "xmcp_knowledge.h"
#include "../../base/xjson.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include <stdio.h>
#include <string.h>

/* ---- Structured-result helpers ----------------------------------------- */

static XrJsonValue *make_syntax_result_content(const char *topic, bool found, const char *content) {
    XR_DCHECK(topic != NULL, "make_syntax_result_content: NULL topic");
    XR_DCHECK(content != NULL, "make_syntax_result_content: NULL content");
    XrJsonValue *structured = xjson_new_object();
    XJSON_SET_STRING(structured, "topic", topic);
    XJSON_SET_BOOL(structured, "found", found);
    XJSON_SET_STRING(structured, "content", content);
    return structured;
}

static XrJsonValue *make_stdlib_match_json(const XmcpStdlibMatch *match) {
    XR_DCHECK(match != NULL, "make_stdlib_match_json: NULL match");
    XR_DCHECK(match->module != NULL, "make_stdlib_match_json: NULL module");
    XrJsonValue *item = xjson_new_object();
    XJSON_SET_STRING(item, "module", match->module->name);
    XJSON_SET_STRING(item, "summary", match->module->summary);
    XJSON_SET_INT(item, "score", match->score);
    if (match->symbol) {
        XJSON_SET_STRING(item, "symbol", match->symbol->name);
        XJSON_SET_STRING(item, "signature", match->symbol->signature);
        XJSON_SET_STRING(item, "symbolSummary", match->symbol->summary);
    }
    return item;
}

static XrJsonValue *make_stdlib_result_content(const char *query, const char *module,
                                               const XmcpStdlibSearchResult *search,
                                               const char *content) {
    XR_DCHECK(query != NULL, "make_stdlib_result_content: NULL query");
    XR_DCHECK(search != NULL, "make_stdlib_result_content: NULL search");
    XR_DCHECK(content != NULL, "make_stdlib_result_content: NULL content");
    XrJsonValue *structured = xjson_new_object();
    XJSON_SET_STRING(structured, "query", query);
    XJSON_SET_STRING(structured, "module", module ? module : "");
    XJSON_SET_INT(structured, "matchCount", search->match_count);
    XJSON_SET_BOOL(structured, "found", search->match_count > 0);
    XJSON_SET_STRING(structured, "content", content);
    XrJsonValue *matches = xjson_new_array();
    for (int i = 0; i < search->match_count; i++)
        xjson_array_push(matches, make_stdlib_match_json(&search->matches[i]));
    xjson_object_set(structured, "matches", matches);
    return structured;
}

static XrJsonValue *make_definition_result_content(const char *symbol, const char *kind, bool found,
                                                   const char *content) {
    XR_DCHECK(symbol != NULL, "make_definition_result_content: NULL symbol");
    XR_DCHECK(kind != NULL, "make_definition_result_content: NULL kind");
    XR_DCHECK(content != NULL, "make_definition_result_content: NULL content");
    XrJsonValue *structured = xjson_new_object();
    XJSON_SET_STRING(structured, "symbol", symbol);
    XJSON_SET_STRING(structured, "kind", kind);
    XJSON_SET_BOOL(structured, "found", found);
    XJSON_SET_STRING(structured, "content", content);
    return structured;
}

/* ---- Tool: xray_syntax_lookup ------------------------------------------ */

XR_FUNC XrJsonValue *xmcp_tool_xray_syntax_lookup(XmcpServer *server, const XmcpCallContext *ctx,
                                                  XrJsonValue *arguments) {
    XR_DCHECK(server != NULL, "xmcp_tool_xray_syntax_lookup: NULL server");
    XR_DCHECK(ctx != NULL, "xmcp_tool_xray_syntax_lookup: NULL ctx");
    XR_DCHECK(arguments != NULL, "xmcp_tool_xray_syntax_lookup: NULL arguments");
    (void) ctx;

    const char *topic = xjson_get_string(arguments, "topic");
    if (!topic || topic[0] == '\0')
        return xmcp_make_error_result("Error: 'topic' must not be empty");
    if (!server->knowledge)
        return xmcp_make_error_result("Error: knowledge base is not available");

    const char *content = xmcp_knowledge_lookup_topic(server->knowledge, topic);
    if (content) {
        return xmcp_make_text_structured_result(
            content, make_syntax_result_content(topic, true, content), false);
    }

    char *available = xmcp_knowledge_list_topics(server->knowledge);
    char msg[1024];
    snprintf(msg, sizeof(msg),
             "No syntax documentation found for topic \"%s\".\n\n"
             "Available topics: %s.",
             topic, available ? available : "");
    if (available)
        xr_free(available);
    return xmcp_make_text_structured_result(msg, make_syntax_result_content(topic, false, msg),
                                            false);
}

/* ---- Tool: xray_stdlib_search ------------------------------------------ */

XR_FUNC XrJsonValue *xmcp_tool_xray_stdlib_search(XmcpServer *server, const XmcpCallContext *ctx,
                                                  XrJsonValue *arguments) {
    XR_DCHECK(server != NULL, "xmcp_tool_xray_stdlib_search: NULL server");
    XR_DCHECK(ctx != NULL, "xmcp_tool_xray_stdlib_search: NULL ctx");
    XR_DCHECK(arguments != NULL, "xmcp_tool_xray_stdlib_search: NULL arguments");
    (void) ctx;

    const char *query = xjson_get_string(arguments, "query");
    const char *module = xjson_get_string(arguments, "module");
    if (!query || query[0] == '\0')
        return xmcp_make_error_result("Error: 'query' must not be empty");
    if (!server->knowledge)
        return xmcp_make_error_result("Error: knowledge base is not available");

    XmcpStdlibSearchResult search;
    xmcp_knowledge_search_stdlib_matches(server->knowledge, query, module, &search);
    char *text = xmcp_knowledge_search_stdlib(server->knowledge, query, module, NULL);
    if (!text)
        return xmcp_make_error_result("Error: stdlib search failed");
    XrJsonValue *result = xmcp_make_text_structured_result(
        text, make_stdlib_result_content(query, module, &search, text), false);
    xr_free(text);
    return result;
}

/* ---- Tool: xray_definition --------------------------------------------- */

/* Run a single stdlib lookup pass for xmcp_tool_xray_definition.
 *
 *   *search_failed = true   the underlying search allocator failed; abort.
 *   return non-NULL          a hit; caller owns the result and returns it.
 *   return NULL, !*failed    no match; caller should try the next strategy.
 *
 * Centralizes the (text != NULL ? match_count > 0 ? consume : free : abort)
 * three-way decision so each caller below stays linear. */
static XrJsonValue *try_definition_stdlib(XmcpKnowledge *kb, const char *symbol, const char *query,
                                          const char *module_filter, bool *search_failed) {
    XR_DCHECK(kb != NULL && symbol != NULL && query != NULL && search_failed != NULL,
              "try_definition_stdlib: NULL parameter");
    int match_count = 0;
    char *text = xmcp_knowledge_search_stdlib(kb, query, module_filter, &match_count);
    if (!text) {
        *search_failed = true;
        return NULL;
    }
    if (match_count == 0) {
        xr_free(text);
        return NULL;
    }
    XrJsonValue *result = xmcp_make_text_structured_result(
        text, make_definition_result_content(symbol, "stdlib", true, text), false);
    xr_free(text);
    return result;
}

XR_FUNC XrJsonValue *xmcp_tool_xray_definition(XmcpServer *server, const XmcpCallContext *ctx,
                                               XrJsonValue *arguments) {
    XR_DCHECK(server != NULL, "xmcp_tool_xray_definition: NULL server");
    XR_DCHECK(ctx != NULL, "xmcp_tool_xray_definition: NULL ctx");
    XR_DCHECK(arguments != NULL, "xmcp_tool_xray_definition: NULL arguments");
    (void) ctx;

    const char *symbol = xjson_get_string(arguments, "symbol");
    if (!symbol || symbol[0] == '\0')
        return xmcp_make_error_result("Error: 'symbol' must not be empty");
    if (!server->knowledge)
        return xmcp_make_error_result("Error: knowledge base is not available");

    /* Strategy 1: language syntax topic ("chan", "class", "enum"). */
    const char *topic_content = xmcp_knowledge_lookup_topic(server->knowledge, symbol);
    if (topic_content) {
        return xmcp_make_text_structured_result(
            topic_content, make_definition_result_content(symbol, "syntax", true, topic_content),
            false);
    }

    /* Strategy 2: full-text stdlib search ("http.Server", "json.parse"). */
    bool failed = false;
    XrJsonValue *hit = try_definition_stdlib(server->knowledge, symbol, symbol, NULL, &failed);
    if (hit)
        return hit;
    if (failed)
        return xmcp_make_error_result("Error: stdlib search failed");

    /* Strategy 3: split "module.symbol" and search within that module. */
    const char *dot = strchr(symbol, '.');
    if (dot && dot > symbol) {
        size_t mod_len = (size_t) (dot - symbol);
        char mod[128];
        if (mod_len < sizeof(mod)) {
            memcpy(mod, symbol, mod_len);
            mod[mod_len] = '\0';
            hit = try_definition_stdlib(server->knowledge, symbol, dot + 1, mod, &failed);
            if (hit)
                return hit;
            if (failed)
                return xmcp_make_error_result("Error: stdlib search failed");
        }
    }

    /* Nothing matched: render a hint instead of returning an error so the
     * MCP client can show the user actionable next steps. */
    char msg[512];
    snprintf(msg, sizeof(msg),
             "No definition found for \"%s\".\n\n"
             "Try: language keywords (class, chan, enum), stdlib modules "
             "(http, json), or module.symbol format (http.Server).",
             symbol);
    return xmcp_make_text_structured_result(
        msg, make_definition_result_content(symbol, "none", false, msg), false);
}
