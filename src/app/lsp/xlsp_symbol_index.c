/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_symbol_index.c - Shallow, workspace-wide symbol index implementation
 *
 * Storage: a hash map of file URI -> file node, each file node owning a chain
 * of shallow entries. Per-file grouping makes re-indexing a single file O(k)
 * (drop the file's chain, rebuild it) and keeps one owned URI string shared by
 * all of that file's entries. Lookups that need every symbol (workspace symbol,
 * definition fallback, completion) iterate the map directly.
 */

#include "xlsp_symbol_index.h"
#include "xlsp_index_pool.h"  // XrLspIndexSymbol
#include "../../base/xhashmap.h"
#include "../../base/xmalloc.h"
#include <string.h>

// A file's owned URI plus the chain of shallow entries declared in it.
typedef struct XlspIndexFile {
    char *uri;                // Owned; also serves as the by_file map key
    XlspIndexEntry *entries;  // Head of the file_next chain
} XlspIndexFile;

struct XlspSymbolIndex {
    XrHashMap *by_file;  // uri -> XlspIndexFile*
    size_t entry_count;
};

XlspSymbolIndex *xlsp_symbol_index_new(void) {
    XlspSymbolIndex *idx = xr_calloc(1, sizeof(XlspSymbolIndex));
    if (!idx)
        return NULL;
    idx->by_file = xr_hashmap_new();
    if (!idx->by_file) {
        xr_free(idx);
        return NULL;
    }
    return idx;
}

static void free_entries(XlspSymbolIndex *idx, XlspIndexFile *file) {
    XlspIndexEntry *e = file->entries;
    while (e) {
        XlspIndexEntry *next = e->file_next;
        xr_free(e->name);
        xr_free(e);
        if (idx->entry_count > 0)
            idx->entry_count--;
        e = next;
    }
    file->entries = NULL;
}

static void free_file_cb(const char *key, void *value, void *userdata) {
    (void) key;
    XlspSymbolIndex *idx = (XlspSymbolIndex *) userdata;
    XlspIndexFile *file = (XlspIndexFile *) value;
    if (!file)
        return;
    free_entries(idx, file);
    xr_free(file->uri);
    xr_free(file);
}

void xlsp_symbol_index_free(XlspSymbolIndex *idx) {
    if (!idx)
        return;
    if (idx->by_file) {
        xr_hashmap_foreach(idx->by_file, free_file_cb, idx);
        xr_hashmap_free(idx->by_file);
    }
    xr_free(idx);
}

void xlsp_symbol_index_replace_file(XlspSymbolIndex *idx, const char *uri,
                                    struct XrLspIndexSymbol *symbols) {
    if (!idx || !uri || !idx->by_file)
        return;

    XlspIndexFile *file = (XlspIndexFile *) xr_hashmap_get(idx->by_file, uri);
    if (!file) {
        file = xr_calloc(1, sizeof(XlspIndexFile));
        if (!file)
            return;
        file->uri = xr_strdup(uri);
        if (!file->uri) {
            xr_free(file);
            return;
        }
        // Key by the file node's own URI so its lifetime matches the entry.
        if (!xr_hashmap_set(idx->by_file, file->uri, file)) {
            xr_free(file->uri);
            xr_free(file);
            return;
        }
    } else {
        free_entries(idx, file);
    }

    for (struct XrLspIndexSymbol *s = symbols; s; s = s->next) {
        if (!s->name)
            continue;
        XlspIndexEntry *e = xr_calloc(1, sizeof(XlspIndexEntry));
        if (!e)
            continue;
        e->name = xr_strdup(s->name);
        if (!e->name) {
            xr_free(e);
            continue;
        }
        e->uri = file->uri;  // Borrow the file node's owned URI.
        e->line = s->line;
        e->column = s->column;
        e->kind = s->kind;
        e->is_exported = s->is_exported;
        e->file_next = file->entries;
        file->entries = e;
        idx->entry_count++;
    }
}

void xlsp_symbol_index_remove_file(XlspSymbolIndex *idx, const char *uri) {
    if (!idx || !uri || !idx->by_file)
        return;
    XlspIndexFile *file = (XlspIndexFile *) xr_hashmap_get(idx->by_file, uri);
    if (!file)
        return;
    // Delete the map entry before freeing file->uri (the entry's key). The map
    // does not own keys, so this leaves file->uri valid to free below.
    xr_hashmap_delete(idx->by_file, uri);
    free_entries(idx, file);
    xr_free(file->uri);
    xr_free(file);
}

void xlsp_symbol_index_remove_prefix(XlspSymbolIndex *idx, const char *path_prefix) {
    if (!idx || !path_prefix || !idx->by_file || path_prefix[0] == '\0')
        return;
    size_t prefix_len = strlen(path_prefix);

    // Collect matching file nodes first; deleting during map iteration is unsafe.
    XrHashMap *m = idx->by_file;
    XlspIndexFile **doomed = xr_malloc(sizeof(XlspIndexFile *) * (m->count ? m->count : 1));
    if (!doomed)
        return;
    int n = 0;
    for (uint32_t i = 0; i < m->capacity; i++) {
        XrHashMapEntry *slot = &m->entries[i];
        if (!slot->key || slot->value == XR_HASHMAP_TOMBSTONE)
            continue;
        XlspIndexFile *file = (XlspIndexFile *) slot->value;
        const char *p = file->uri;
        if (strncmp(p, "file://", 7) == 0)
            p += 7;
        if (strncmp(p, path_prefix, prefix_len) == 0)
            doomed[n++] = file;
    }
    for (int i = 0; i < n; i++) {
        XlspIndexFile *file = doomed[i];
        xr_hashmap_delete(idx->by_file, file->uri);
        free_entries(idx, file);
        xr_free(file->uri);
        xr_free(file);
    }
    xr_free(doomed);
}

const XlspIndexEntry *xlsp_symbol_index_find(XlspSymbolIndex *idx, const char *name) {
    if (!idx || !name || !idx->by_file)
        return NULL;
    const XlspIndexEntry *fallback = NULL;
    XrHashMap *m = idx->by_file;
    for (uint32_t i = 0; i < m->capacity; i++) {
        XrHashMapEntry *slot = &m->entries[i];
        if (!slot->key || slot->value == XR_HASHMAP_TOMBSTONE)
            continue;
        XlspIndexFile *file = (XlspIndexFile *) slot->value;
        for (XlspIndexEntry *e = file->entries; e; e = e->file_next) {
            if (strcmp(e->name, name) == 0) {
                if (e->is_exported)
                    return e;  // Strongest: an exported definition.
                if (!fallback)
                    fallback = e;
            }
        }
    }
    return fallback;
}

void xlsp_symbol_index_foreach(XlspSymbolIndex *idx, XlspSymbolIndexIter cb, void *ctx) {
    if (!idx || !cb || !idx->by_file)
        return;
    XrHashMap *m = idx->by_file;
    for (uint32_t i = 0; i < m->capacity; i++) {
        XrHashMapEntry *slot = &m->entries[i];
        if (!slot->key || slot->value == XR_HASHMAP_TOMBSTONE)
            continue;
        XlspIndexFile *file = (XlspIndexFile *) slot->value;
        for (XlspIndexEntry *e = file->entries; e; e = e->file_next) {
            cb(e, ctx);
        }
    }
}

size_t xlsp_symbol_index_entry_count(const XlspSymbolIndex *idx) {
    return idx ? idx->entry_count : 0;
}
