/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_json.h - Bounded owned DAP JSON on the caller resource ledger
 */
#ifndef XDAP_JSON_H
#define XDAP_JSON_H
#include "../../base/xcompile_resources.h"
#include "../../xir/xxir.h"
typedef enum XdapJsonKind { XDAP_JSON_NULL, XDAP_JSON_BOOL, XDAP_JSON_NUMBER,
    XDAP_JSON_STRING, XDAP_JSON_ARRAY, XDAP_JSON_OBJECT } XdapJsonKind;
typedef struct XdapJsonNode {
    XdapJsonKind kind;
    uint32_t first, next, count;
    const char *key, *text;
    size_t key_length, length;
    bool boolean, integer;
} XdapJsonNode;
typedef struct XdapJsonDocument {
    XrCompileResources *resources;
    char *bytes;
    XdapJsonNode *nodes;
    uint32_t count, capacity, root;
} XdapJsonDocument;
typedef struct XdapJsonWriter {
    XrCompileResources *resources;
    char *bytes;
    size_t length, capacity, limit;
    XrXirStatus status;
} XdapJsonWriter;
/* Successful input owns its decoded text and nodes on the original ledger.
 * Input and output require caller exclusion. Failure preserves empty output.
 * Request strings reject embedded NUL; output quoting preserves exact NUL bytes. */
XR_FUNC XrXirStatus xdap_json_parse(XrCompileResources *, const char *, size_t,
    uint32_t node_limit, uint32_t depth_limit, XdapJsonDocument *);
XR_FUNC void xdap_json_document_free(XdapJsonDocument *);
XR_FUNC XrXirStatus xdap_json_member(const XdapJsonDocument *, uint32_t object,
    const char *key, uint32_t *output);
XR_FUNC XrXirStatus xdap_json_u64(const XdapJsonDocument *, uint32_t node, uint64_t *);
XR_FUNC XrXirStatus xdap_json_i64(const XdapJsonDocument *, uint32_t node, int64_t *);
XR_FUNC XrXirStatus xdap_json_equal(const XdapJsonDocument *, uint32_t node,
    const char *expected, bool *);
XR_FUNC XdapJsonWriter xdap_json_writer(XrCompileResources *, size_t byte_limit);
XR_FUNC bool xdap_json_text(XdapJsonWriter *, const char *, size_t);
XR_FUNC bool xdap_json_quote(XdapJsonWriter *, const char *, size_t);
XR_FUNC bool xdap_json_integer(XdapJsonWriter *, int64_t);
XR_FUNC bool xdap_json_unsigned(XdapJsonWriter *, uint64_t);
XR_FUNC void xdap_json_writer_free(XdapJsonWriter *);
#endif // XDAP_JSON_H
