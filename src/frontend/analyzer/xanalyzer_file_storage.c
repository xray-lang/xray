/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_file_storage.c - Atomic file records and owned analysis cursors
 */
#include "xanalyzer_file_storage.h"
#include "../../base/xhashmap.h"
#include "../../base/xchecks.h"
#include <limits.h>
#include <string.h>

typedef struct XaFileStorage {
    XaAnalyzer *owner;
    XaSymbolRegistry *registry;
    XrHashMap *by_path;
    XaAnalyzerFileScope *top;
} XaFileStorage;

static XrCompileState *file_state(const XaAnalyzer *analyzer) {
    return xa_symbol_registry_state(analyzer ? analyzer->symbol_registry : NULL);
}
static XrCompileResourceStatus invalid(XaAnalyzer *analyzer) {
    return xr_compile_state_fail(file_state(analyzer), XR_COMPILE_RESOURCE_BAD_ARGUMENT);
}
static XrCompileResourceStatus accept_io(XaAnalyzer *analyzer, XrOsIoStatus status) {
    return xr_compile_state_fail(file_state(analyzer), status == XR_OS_IO_OK ? XR_COMPILE_RESOURCE_OK :
        status == XR_OS_IO_OVERFLOW ? XR_COMPILE_RESOURCE_BUDGET :
        status == XR_OS_IO_OUT_OF_MEMORY ? XR_COMPILE_RESOURCE_OUT_OF_MEMORY : XR_COMPILE_RESOURCE_BAD_ARGUMENT);
}
static bool ready(XaAnalyzer *analyzer) {
    if (xr_compile_state_status(file_state(analyzer)) != XR_COMPILE_RESOURCE_OK) return false;
    XaFileStorage *storage = analyzer->files_storage;
    if (!storage || storage->owner != analyzer || storage->registry != analyzer->symbol_registry ||
        analyzer->file_count < 0 || xr_hashmap_count(storage->by_path) != (uint32_t)analyzer->file_count) {
        (void) invalid(analyzer); return false;
    }
    return true;
}

XR_FUNC XrCompileResourceStatus xa_analyzer_files_open(XaAnalyzer *analyzer) {
    XrCompileState *state = file_state(analyzer);
    XrCompileResourceStatus status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    if (analyzer->files_storage || analyzer->files || analyzer->file_count ||
        !analyzer->global_scope || analyzer->global_scope->registry != analyzer->symbol_registry)
        return invalid(analyzer);
    void *memory = NULL;
    status = xr_compile_state_calloc(state, 1, sizeof(XaFileStorage), &memory);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    XaFileStorage *storage = memory;
    XrOsIoPolicy policy = xr_compile_io_policy(xr_compile_state_resources(state));
    status = accept_io(analyzer, xr_hashmap_owned_new(&policy, &storage->by_path));
    if (status == XR_COMPILE_RESOURCE_OK) status = xa_symbol_registry_retain(analyzer->symbol_registry);
    if (status != XR_COMPILE_RESOURCE_OK) {
        xr_hashmap_owned_free(storage->by_path); xr_compile_state_free(storage); return status;
    }
    storage->owner = analyzer; storage->registry = analyzer->symbol_registry;
    analyzer->files_storage = storage;
    return XR_COMPILE_RESOURCE_OK;
}

XR_FUNC void xa_analyzer_files_close(XaAnalyzer *analyzer) {
    if (!analyzer) return;
    XaFileStorage *storage = analyzer->files_storage;
    if (!storage) return;
    XR_CHECK(storage->owner == analyzer, "File storage cleanup requires its actual owner");
    while (storage->top) {
        XaAnalyzerFileScope *scope = storage->top;
        storage->top = scope->parent;
        memset(scope, 0, sizeof(*scope));
    }
    analyzer->current_file = NULL;
    analyzer->current_scope = analyzer->global_scope;
    analyzer->files_storage = NULL;
    xr_hashmap_owned_free(storage->by_path);
    XaFileEntry *entry = analyzer->files;
    while (entry) {
        XaFileEntry *next = entry->next;
        xr_compile_state_free(entry); entry = next;
    }
    analyzer->files = NULL; analyzer->file_count = 0;
    XaSymbolRegistry *registry = storage->registry;
    xr_compile_state_free(storage); xa_symbol_registry_release(registry);
}
XR_FUNC uint32_t xa_analyzer_file_index_size(const XaAnalyzer *analyzer) {
    const XaFileStorage *storage = analyzer ? analyzer->files_storage : NULL;
    return storage && storage->owner == analyzer ? xr_hashmap_count(storage->by_path) : 0;
}

XR_FUNC XrCompileResourceStatus xa_analyzer_file_find(XaAnalyzer *analyzer,
    const char *path, XaFileEntry **output) {
    XrCompileState *state = file_state(analyzer);
    XrCompileResourceStatus status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    if (!output) return invalid(analyzer);
    if (!path) { *output = NULL; return XR_COMPILE_RESOURCE_OK; }
    if (!analyzer->files_storage) {
        if (analyzer->files || analyzer->file_count) return invalid(analyzer);
        *output = NULL; return XR_COMPILE_RESOURCE_OK;
    }
    if (!ready(analyzer)) return xr_compile_state_status(state);
    XaFileStorage *storage = analyzer->files_storage;
    const char *key = NULL; void *value = NULL;
    status = accept_io(analyzer, xr_hashmap_owned_get_pair(storage->by_path, path, &key, &value));
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    if (key) {
        status = xr_compile_state_work(state, 1);
        if (status != XR_COMPILE_RESOURCE_OK) return status;
        XaFileEntry *entry = value;
        if (!entry || entry->path != key || !entry->file_scope ||
            entry->file_scope->registry != storage->registry) return invalid(analyzer);
    }
    *output = value;
    return XR_COMPILE_RESOURCE_OK;
}

static bool valid_parent(XaAnalyzer *analyzer, XaScope *parent) {
    if (!parent || parent->registry != analyzer->symbol_registry) { (void)invalid(analyzer); return false; }
    XaScope *root = parent;
    while (root->parent) {
        if (xr_compile_state_work(file_state(analyzer), 1) != XR_COMPILE_RESOURCE_OK) return false;
        if (root->parent->registry != analyzer->symbol_registry) { (void)invalid(analyzer); return false; }
        root = root->parent;
    }
    if (root != analyzer->global_scope) { (void)invalid(analyzer); return false; }
    return true;
}

XR_FUNC XrCompileResourceStatus xa_analyzer_file_get_or_create(XaAnalyzer *analyzer,
    const char *path, XaScope *parent, XaFileEntry **output) {
    XrCompileState *state = file_state(analyzer);
    XrCompileResourceStatus status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    if (!output) return invalid(analyzer);
    if (!path) { *output = NULL; return XR_COMPILE_RESOURCE_OK; }
    if (!ready(analyzer) || !valid_parent(analyzer, parent)) return xr_compile_state_status(state);
    XaFileEntry *found = NULL;
    status = xa_analyzer_file_find(analyzer, path, &found);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    if (found) {
        if (found->file_scope->parent != parent) return invalid(analyzer);
        *output = found; return XR_COMPILE_RESOURCE_OK;
    }
    if (analyzer->file_count == INT_MAX) return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BUDGET);
    void *memory = NULL;
    status = xr_compile_state_calloc(state, 1, sizeof(XaFileEntry), &memory);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    XaFileEntry *entry = memory;
    status = xa_symbol_registry_copy_origin(analyzer->symbol_registry, path, &entry->path);
    if (status == XR_COMPILE_RESOURCE_OK) {
        XaFileStorage *storage = analyzer->files_storage;
        entry->file_scope = xa_scope_new_indexed(analyzer->symbol_registry, XA_SCOPE_GLOBAL,
            parent, storage->by_path, entry->path, entry);
        status = xr_compile_state_status(state);
    }
    if (status != XR_COMPILE_RESOURCE_OK) { xr_compile_state_free(entry); return status; }
    XR_CHECK(entry->file_scope != NULL, "A successful indexed scope must be published");
    entry->dirty = true; entry->next = analyzer->files;
    analyzer->files = entry; ++analyzer->file_count;
    *output = entry;
    return XR_COMPILE_RESOURCE_OK;
}

XR_FUNC bool xa_analyzer_prepare_input_scope(XaAnalyzer *analyzer, const char *path, XaScope *parent) {
    XaFileEntry *entry = NULL;
    return xa_analyzer_file_get_or_create(analyzer, path, parent, &entry) == XR_COMPILE_RESOURCE_OK && entry;
}

XR_FUNC bool xa_analyzer_push_file_scope(XaAnalyzer *analyzer, const char *path, XaAnalyzerFileScope *scope) {
    if (xr_compile_state_status(file_state(analyzer)) != XR_COMPILE_RESOURCE_OK) return false;
    if (!scope || scope->active || scope->owner || scope->parent || scope->previous_scope || scope->previous_file) {
        (void)invalid(analyzer); return false;
    }
    XaFileEntry *entry = NULL;
    if (xa_analyzer_file_find(analyzer, path, &entry) != XR_COMPILE_RESOURCE_OK || !entry) return false;
    XaFileStorage *storage = analyzer->files_storage;
    if (analyzer->current_scope && analyzer->current_scope->registry != storage->registry) {
        (void)invalid(analyzer); return false;
    }
    if (xr_compile_state_work(file_state(analyzer), 1) != XR_COMPILE_RESOURCE_OK) return false;
    scope->previous_scope = analyzer->current_scope; scope->previous_file = analyzer->current_file;
    scope->owner = analyzer; scope->parent = storage->top; scope->active = true;
    storage->top = scope; analyzer->current_scope = entry->file_scope; analyzer->current_file = entry->path;
    return true;
}

XR_FUNC void xa_analyzer_pop_file_scope(XaAnalyzer *analyzer, XaAnalyzerFileScope *scope) {
    if (!analyzer || !scope || !scope->active) return;
    XaFileStorage *storage = analyzer->files_storage;
    if (!storage || storage->owner != analyzer || scope->owner != analyzer || storage->top != scope) {
        (void)invalid(analyzer); return;
    }
    bool live = scope->previous_file == NULL;
    for (XaFileEntry *entry = analyzer->files; !live && entry; entry = entry->next)
        live = entry->path == scope->previous_file;
    analyzer->current_scope = live ? scope->previous_scope : analyzer->global_scope;
    analyzer->current_file = live ? scope->previous_file : NULL;
    storage->top = scope->parent;
    memset(scope, 0, sizeof(*scope));
}

XR_FUNC XrCompileResourceStatus xa_analyzer_file_remove_record(XaAnalyzer *analyzer,
    const char *path, bool *removed) {
    XrCompileState *state = file_state(analyzer);
    XrCompileResourceStatus status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    if (!removed) return invalid(analyzer);
    XaFileEntry *entry = NULL;
    status = xa_analyzer_file_find(analyzer, path, &entry);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    if (!entry) { *removed = false; return XR_COMPILE_RESOURCE_OK; }
    XaFileEntry **slot = &analyzer->files;
    while (*slot != entry) {
        if (xr_compile_state_work(state, 1) != XR_COMPILE_RESOURCE_OK) return xr_compile_state_status(state);
        if (!*slot) return invalid(analyzer);
        slot = &(*slot)->next;
    }
    bool erased = false; XaFileStorage *storage = analyzer->files_storage;
    status = accept_io(analyzer, xr_hashmap_owned_delete(storage->by_path, path, &erased));
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    XR_CHECK(erased, "The prepared file entry must still be indexed");
    *slot = entry->next; --analyzer->file_count;
    if (analyzer->current_file == entry->path) {
        analyzer->current_scope = analyzer->global_scope; analyzer->current_file = NULL;
    }
    xr_compile_state_free(entry); *removed = true;
    return XR_COMPILE_RESOURCE_OK;
}

XR_FUNC void xa_analyzer_mark_file_dirty(XaAnalyzer *analyzer, const char *path) {
    XaFileEntry *entry = NULL;
    if (xa_analyzer_file_find(analyzer, path, &entry) != XR_COMPILE_RESOURCE_OK || !entry || entry->dirty) return;
    if (xr_compile_state_work(file_state(analyzer), 1) == XR_COMPILE_RESOURCE_OK) entry->dirty = true;
}

XR_FUNC XrCompileResourceStatus xa_analyzer_get_dirty_files(XaAnalyzer *analyzer,
    const char ***output, int *count) {
    XrCompileState *state = file_state(analyzer);
    XrCompileResourceStatus status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    if (!output || *output || !count) return invalid(analyzer);
    if (!analyzer->files_storage) {
        if (analyzer->files || analyzer->file_count) return invalid(analyzer);
        *output = NULL; *count = 0; return XR_COMPILE_RESOURCE_OK;
    }
    if (!ready(analyzer)) return xr_compile_state_status(state);
    int dirty = 0, visited = 0;
    for (XaFileEntry *entry = analyzer->files; entry; entry = entry->next) {
        status = xr_compile_state_work(state, 1);
        if (status != XR_COMPILE_RESOURCE_OK) return status;
        if (visited++ >= analyzer->file_count) return invalid(analyzer);
        if (entry->dirty) ++dirty;
    }
    if (visited != analyzer->file_count) return invalid(analyzer);
    void *memory = NULL;
    if (dirty) status = xr_compile_state_calloc(state, (size_t)dirty, sizeof(const char *), &memory);
    const char **paths = memory; int index = 0;
    for (XaFileEntry *entry = analyzer->files; status == XR_COMPILE_RESOURCE_OK && entry; entry = entry->next) {
        status = xr_compile_state_work(state, 1);
        if (status == XR_COMPILE_RESOURCE_OK && entry->dirty) paths[index++] = entry->path;
    }
    if (status == XR_COMPILE_RESOURCE_OK) status = xr_compile_state_work(state, 1);
    if (status != XR_COMPILE_RESOURCE_OK) { xr_compile_state_free(memory); return status; }
    XR_CHECK(index == dirty, "The serialized dirty file set must remain unchanged");
    *output = paths; *count = dirty;
    return XR_COMPILE_RESOURCE_OK;
}

XR_FUNC void xa_analyzer_invalidate_range(XaAnalyzer *analyzer, const char *path,
    uint32_t start_line, uint32_t end_line) {
    (void)start_line; (void)end_line;
    XaFileEntry *entry = NULL;
    XaScope *parent = analyzer ? analyzer->global_scope : NULL;
    if (xa_analyzer_file_get_or_create(analyzer, path, parent, &entry) != XR_COMPILE_RESOURCE_OK || !entry || entry->dirty) return;
    if (xr_compile_state_work(file_state(analyzer), 1) == XR_COMPILE_RESOURCE_OK) entry->dirty = true;
}
