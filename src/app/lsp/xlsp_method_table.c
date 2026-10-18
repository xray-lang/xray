/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_method_table.c - Atomic per-server method table ownership
 */
#include "xlsp_method_table.h"
#include "../../base/xhash.h"
#include "../../base/xmalloc.h"
#include "../../base/xutf8.h"
#include <string.h>

struct MethodHashEntry {
    const LspMethodEntry *entry;
    struct MethodHashEntry *next;
};

static uint32_t method_hash(const char *method) {
    return xr_hash_bytes(method, strlen(method)) % METHOD_HASH_SIZE;
}

static void free_buckets(MethodHashEntry **buckets) {
    for (size_t i = 0; i < METHOD_HASH_SIZE; ++i) {
        MethodHashEntry *entry = buckets[i];
        while (entry) {
            MethodHashEntry *next = entry->next; xr_free(entry); entry = next;
        }
        buckets[i] = NULL;
    }
}

static bool valid_entries(const LspMethodEntry *entries, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const LspMethodEntry *entry = &entries[i];
        if (!entry->method || !entry->method[0] ||
            !xr_utf8_validate(entry->method, strlen(entry->method)) ||
            (!!entry->request_handler == !!entry->notification_handler)) return false;
        for (size_t j = 0; j < i; ++j)
            if (!strcmp(entries[j].method, entry->method)) return false;
    }
    return true;
}

XR_FUNC XrXirStatus xlsp_method_table_initialize(XrLspServer *server,
                                               const LspMethodEntry *entries, size_t count) {
    if (!server || !entries || !count) return XR_XIR_BAD_STRUCTURE;
    if (server->method_table_initialized) return XR_XIR_BAD_STAGE;
    for (size_t i = 0; i < METHOD_HASH_SIZE; ++i)
        if (server->method_hash_table[i]) return XR_XIR_BAD_STAGE;
    if (!valid_entries(entries, count)) return XR_XIR_BAD_STRUCTURE;
    MethodHashEntry *candidate[METHOD_HASH_SIZE] = {0};
    for (size_t i = 0; i < count; ++i) {
        uint32_t hash = method_hash(entries[i].method);
        MethodHashEntry *entry = xr_malloc(sizeof(*entry));
        if (!entry) { free_buckets(candidate); return XR_XIR_OUT_OF_MEMORY; }
        entry->entry = &entries[i]; entry->next = candidate[hash]; candidate[hash] = entry;
    }
    memcpy(server->method_hash_table, candidate, sizeof(candidate));
    server->method_table_initialized = true;
    return XR_XIR_OK;
}

XR_FUNC const LspMethodEntry *xlsp_method_table_find(const XrLspServer *server, const char *method) {
    if (!server || !server->method_table_initialized || !method || !method[0]) return NULL;
    for (MethodHashEntry *entry = server->method_hash_table[method_hash(method)]; entry; entry = entry->next)
        if (!strcmp(entry->entry->method, method)) return entry->entry;
    return NULL;
}

XR_FUNC void xlsp_method_table_free(XrLspServer *server) {
    if (!server) return;
    free_buckets(server->method_hash_table); server->method_table_initialized = false;
}
