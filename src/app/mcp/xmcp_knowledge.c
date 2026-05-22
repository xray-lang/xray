/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_knowledge.c - Generated MCP knowledge lookup and ranking
 *
 * KEY CONCEPT:
 *   The MCP knowledge payload is generated into immutable tables. This file
 *   only copies table pointers into a small runtime index and implements the
 *   lookup/ranking behavior used by tools and resources.
 */

#include "xmcp_knowledge.h"
#include "../../base/xchecks.h"
#include "../../base/xmalloc.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#ifdef XR_OS_WINDOWS
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif

#define XMCP_QUERY_MAX_TOKENS 8
#define XMCP_QUERY_TOKEN_MAX 64
#define XMCP_QUERY_MODULE_MAX 64
#define XMCP_QUERY_SYMBOL_MAX 128

static bool icontains(const char *haystack, const char *needle) {
    if (!haystack || !needle)
        return false;
    if (needle[0] == '\0')
        return true;
    size_t nlen = strlen(needle);
    size_t hlen = strlen(haystack);
    if (nlen > hlen)
        return false;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        if (strncasecmp(haystack + i, needle, nlen) == 0)
            return true;
    }
    return false;
}

static bool csv_has_exact(const char *csv, const char *query) {
    if (!csv || !query || query[0] == '\0')
        return false;

    const char *p = csv;
    while (*p) {
        while (*p == ',' || isspace((unsigned char) *p))
            p++;
        const char *start = p;
        while (*p && *p != ',')
            p++;
        const char *end = p;
        while (end > start && isspace((unsigned char) end[-1]))
            end--;
        size_t len = (size_t) (end - start);
        if (strlen(query) == len && strncasecmp(start, query, len) == 0)
            return true;
    }
    return false;
}

static int split_query_tokens(const char *query,
                              char tokens[XMCP_QUERY_MAX_TOKENS][XMCP_QUERY_TOKEN_MAX]) {
    int count = 0;
    const unsigned char *p = (const unsigned char *) query;
    while (p && *p && count < XMCP_QUERY_MAX_TOKENS) {
        while (*p && !isalnum(*p) && *p != '_')
            p++;
        if (!*p)
            break;
        size_t len = 0;
        while (p[len] && (isalnum(p[len]) || p[len] == '_'))
            len++;
        if (len > 0) {
            size_t copy_len = len < XMCP_QUERY_TOKEN_MAX ? len : XMCP_QUERY_TOKEN_MAX - 1;
            memcpy(tokens[count], p, copy_len);
            tokens[count][copy_len] = '\0';
            count++;
        }
        p += len;
    }
    return count;
}

static bool split_module_symbol(const char *query, char *module, size_t module_cap, char *symbol,
                                size_t symbol_cap) {
    if (!query || !module || !symbol)
        return false;
    const char *dot = strchr(query, '.');
    if (!dot || dot == query || dot[1] == '\0')
        return false;
    size_t module_len = (size_t) (dot - query);
    size_t symbol_len = strlen(dot + 1);
    if (module_len >= module_cap || symbol_len >= symbol_cap)
        return false;
    memcpy(module, query, module_len);
    module[module_len] = '\0';
    memcpy(symbol, dot + 1, symbol_len + 1);
    return true;
}

static int score_text_terms(const char *text, const char *query,
                            char tokens[XMCP_QUERY_MAX_TOKENS][XMCP_QUERY_TOKEN_MAX],
                            int token_count, int exact_score, int contains_score, int token_score) {
    if (!text || !query || query[0] == '\0')
        return 0;
    int score = 0;
    if (strcasecmp(text, query) == 0)
        score = exact_score;
    if (icontains(text, query))
        score = score > contains_score ? score : contains_score;
    int token_hits = 0;
    for (int i = 0; i < token_count; i++) {
        if (tokens[i][0] != '\0' && icontains(text, tokens[i]))
            token_hits++;
    }
    if (token_hits > 0) {
        int token_total = token_hits * token_score;
        if (token_count > 1 && token_hits == token_count)
            token_total += token_score;
        if (token_total > score)
            score = token_total;
    }
    return score;
}

static void insert_match(XmcpStdlibSearchResult *out, const XmcpModule *module,
                         const XmcpGeneratedStdlibSymbol *symbol, int score) {
    XR_DCHECK(out != NULL, "insert_match: NULL out");
    XR_DCHECK(module != NULL, "insert_match: NULL module");
    if (score <= 0)
        return;

    int pos = out->match_count;
    if (pos >= XMCP_STDLIB_MAX_MATCHES) {
        if (score <= out->matches[XMCP_STDLIB_MAX_MATCHES - 1].score)
            return;
        pos = XMCP_STDLIB_MAX_MATCHES - 1;
    } else {
        out->match_count++;
    }

    while (pos > 0 && out->matches[pos - 1].score < score) {
        out->matches[pos] = out->matches[pos - 1];
        pos--;
    }
    out->matches[pos].module = module;
    out->matches[pos].symbol = symbol;
    out->matches[pos].score = score;
}

static int score_symbol(const XmcpGeneratedStdlibSymbol *symbol, const char *query,
                        char tokens[XMCP_QUERY_MAX_TOKENS][XMCP_QUERY_TOKEN_MAX], int token_count) {
    if (!symbol)
        return 0;
    int score = score_text_terms(symbol->name, query, tokens, token_count, 140, 100, 55);
    int sig_score = score_text_terms(symbol->signature, query, tokens, token_count, 70, 45, 25);
    int doc_score = score_text_terms(symbol->summary, query, tokens, token_count, 50, 30, 18);
    if (sig_score > score)
        score = sig_score;
    if (doc_score > score)
        score = doc_score;
    return score;
}

static int score_module(const XmcpModule *module, const char *query,
                        char tokens[XMCP_QUERY_MAX_TOKENS][XMCP_QUERY_TOKEN_MAX], int token_count) {
    if (!module)
        return 0;
    int score = score_text_terms(module->name, query, tokens, token_count, 120, 90, 55);
    int summary_score = score_text_terms(module->summary, query, tokens, token_count, 70, 45, 25);
    int body_score = score_text_terms(module->body, query, tokens, token_count, 35, 20, 10);
    if (summary_score > score)
        score = summary_score;
    if (body_score > score)
        score = body_score;
    return score;
}

static bool module_filter_allows(const XmcpModule *module, const char *module_filter) {
    XR_DCHECK(module != NULL, "module_filter_allows: NULL module");
    if (!module_filter || module_filter[0] == '\0')
        return true;
    return strcasecmp(module->name, module_filter) == 0;
}

static bool knowledge_has_module(XmcpKnowledge *kb, const char *name) {
    if (!kb || !name || name[0] == '\0')
        return false;
    for (int i = 0; i < kb->module_count; i++) {
        if (strcasecmp(kb->modules[i].name, name) == 0)
            return true;
    }
    return false;
}

static void append_available_modules(char *buf, size_t cap, size_t *len, XmcpKnowledge *kb) {
    XR_DCHECK(buf != NULL, "append_available_modules: NULL buf");
    XR_DCHECK(len != NULL, "append_available_modules: NULL len");
    XR_DCHECK(kb != NULL, "append_available_modules: NULL kb");
    for (int i = 0; i < kb->module_count; i++) {
        if (i > 0)
            *len += (size_t) snprintf(buf + *len, cap - *len, ", ");
        *len += (size_t) snprintf(buf + *len, cap - *len, "%s", kb->modules[i].name);
    }
}

XmcpKnowledge *xmcp_knowledge_new(void) {
    XmcpKnowledge *kb = xr_calloc(1, sizeof(XmcpKnowledge));
    if (!kb)
        return NULL;
    return kb;
}

void xmcp_knowledge_load(XmcpKnowledge *kb) {
    if (!kb)
        return;
    kb->topic_count = 0;
    kb->module_count = 0;

    for (int i = 0; i < xmcp_generated_topic_count; i++) {
        if (kb->topic_count >= XMCP_MAX_TOPICS)
            break;
        const XmcpGeneratedTopic *src = &xmcp_generated_topics[i];
        XmcpTopic *dst = &kb->topics[kb->topic_count++];
        dst->name = src->id;
        dst->title = src->title;
        dst->aliases = src->aliases_csv;
        dst->content = src->body;
    }

    for (int i = 0; i < xmcp_generated_stdlib_count; i++) {
        if (kb->module_count >= XMCP_MAX_MODULES)
            break;
        const XmcpGeneratedStdlibEntry *src = &xmcp_generated_stdlib[i];
        XmcpModule *dst = &kb->modules[kb->module_count++];
        dst->name = src->module;
        dst->summary = src->summary;
        dst->body = src->body;
        dst->symbols = src->symbols;
        dst->symbol_count = src->symbol_count;
    }
}

void xmcp_knowledge_free(XmcpKnowledge *kb) {
    xr_free(kb);
}

const char *xmcp_knowledge_lookup_topic(XmcpKnowledge *kb, const char *query) {
    if (!kb || !query || query[0] == '\0')
        return NULL;

    for (int i = 0; i < kb->topic_count; i++) {
        if (strcasecmp(kb->topics[i].name, query) == 0)
            return kb->topics[i].content;
    }
    for (int i = 0; i < kb->topic_count; i++) {
        if (csv_has_exact(kb->topics[i].aliases, query))
            return kb->topics[i].content;
    }
    for (int i = 0; i < kb->topic_count; i++) {
        if (icontains(kb->topics[i].name, query))
            return kb->topics[i].content;
    }
    return NULL;
}

char *xmcp_knowledge_list_topics(XmcpKnowledge *kb) {
    if (!kb)
        return NULL;
    size_t cap = 1;
    for (int i = 0; i < kb->topic_count; i++)
        cap += strlen(kb->topics[i].name) + 2;
    char *buf = xr_malloc(cap);
    if (!buf)
        return NULL;
    size_t len = 0;
    buf[0] = '\0';
    for (int i = 0; i < kb->topic_count; i++) {
        if (i > 0)
            len += (size_t) snprintf(buf + len, cap - len, ", ");
        len += (size_t) snprintf(buf + len, cap - len, "%s", kb->topics[i].name);
    }
    return buf;
}

void xmcp_knowledge_search_stdlib_matches(XmcpKnowledge *kb, const char *query,
                                          const char *module_filter, XmcpStdlibSearchResult *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!kb || !query || strlen(query) < 2)
        return;

    char direct_module[XMCP_QUERY_MODULE_MAX] = {0};
    char direct_symbol[XMCP_QUERY_SYMBOL_MAX] = {0};
    bool direct = split_module_symbol(query, direct_module, sizeof(direct_module), direct_symbol,
                                      sizeof(direct_symbol));
    bool direct_module_known = direct && knowledge_has_module(kb, direct_module);
    const char *effective_query = query;
    const char *effective_filter = module_filter;
    if (direct_module_known && (!module_filter || module_filter[0] == '\0' ||
                                strcasecmp(module_filter, direct_module) == 0)) {
        effective_query = direct_symbol;
        effective_filter = direct_module;
    } else if (direct_module_known) {
        return;
    }

    char tokens[XMCP_QUERY_MAX_TOKENS][XMCP_QUERY_TOKEN_MAX] = {{0}};
    int token_count = split_query_tokens(effective_query, tokens);

    for (int i = 0; i < kb->module_count; i++) {
        const XmcpModule *module = &kb->modules[i];
        if (!module_filter_allows(module, effective_filter))
            continue;

        int module_score =
            direct_module_known ? 0 : score_module(module, effective_query, tokens, token_count);
        insert_match(out, module, NULL, module_score);
        int module_context = score_module(module, query, tokens, token_count);
        for (int j = 0; j < module->symbol_count; j++) {
            int symbol_score =
                score_symbol(&module->symbols[j], effective_query, tokens, token_count);
            if (symbol_score > 0) {
                if (!direct_module_known && module_context > 0)
                    symbol_score += module_context / 2;
                if (direct_module_known)
                    symbol_score += 160;
                insert_match(out, module, &module->symbols[j], symbol_score + 20);
            }
        }
    }
}

char *xmcp_knowledge_search_stdlib(XmcpKnowledge *kb, const char *query, const char *module_filter,
                                   int *match_count) {
    if (!kb || !query)
        return NULL;
    if (match_count)
        *match_count = 0;

    XmcpStdlibSearchResult result;
    xmcp_knowledge_search_stdlib_matches(kb, query, module_filter, &result);
    if (match_count)
        *match_count = result.match_count;

    size_t cap = 8192;
    char *text = xr_malloc(cap);
    if (!text)
        return NULL;
    size_t len = 0;
    len += (size_t) snprintf(text + len, cap - len, "# Standard Library Search: \"%s\"\n\n", query);

    if (result.match_count == 0) {
        len += (size_t) snprintf(text + len, cap - len, "No modules found matching \"%s\".\n\n",
                                 query);
        len += (size_t) snprintf(text + len, cap - len, "Available modules: ");
        append_available_modules(text, cap, &len, kb);
        len += (size_t) snprintf(text + len, cap - len, "\n");
        return text;
    }

    for (int i = 0; i < result.match_count; i++) {
        const XmcpStdlibMatch *m = &result.matches[i];
        if (m->symbol) {
            len += (size_t) snprintf(text + len, cap - len, "## %s.%s\n\n", m->module->name,
                                     m->symbol->name);
            len += (size_t) snprintf(text + len, cap - len, "Signature: `%s%s`\n\n",
                                     m->symbol->name, m->symbol->signature);
            if (m->symbol->summary && m->symbol->summary[0] != '\0')
                len += (size_t) snprintf(text + len, cap - len, "%s\n\n", m->symbol->summary);
        } else {
            len += (size_t) snprintf(text + len, cap - len, "## Module: %s\n\n", m->module->name);
            len += (size_t) snprintf(text + len, cap - len, "Import: `import %s`\n\n",
                                     m->module->name);
            len += (size_t) snprintf(text + len, cap - len, "%s\n\n", m->module->summary);
        }
        if (len > cap - 512)
            break;
    }
    return text;
}

const char *xmcp_knowledge_get_cheatsheet(void) {
    return xmcp_generated_cheatsheet;
}

const char *xmcp_knowledge_get_concurrency(void) {
    return xmcp_generated_concurrency;
}

const char *xmcp_knowledge_get_stdlib_list(void) {
    return xmcp_generated_stdlib_list;
}
