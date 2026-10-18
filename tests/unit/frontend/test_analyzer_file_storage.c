/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_analyzer_file_storage.c - Real file index, cursor and origin lifetimes
 */
#include "frontend/analyzer/xanalyzer_file_storage.h"
#include "frontend/analyzer/xanalyzer_storage.h"
#include "toolchain/xcompiler_session.h"
#include "base/xhashmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define xr_compile_resources_work xr_test_actual_resources_work
#include "../base/parser_resources_observer.h"
#undef xr_compile_resources_work
static bool recording;
static uint64_t boundaries[65536];
static size_t boundary_count, work_calls;
XR_FUNC XrCompileResourceStatus xr_compile_resources_work(XrCompileResources *resources, uint64_t units) {
    ++work_calls;
    XrCompileResourceStatus status = xr_test_actual_resources_work(resources, units);
    if (recording && status == XR_COMPILE_RESOURCE_OK && units) {
        XrCompileResourceStats current = {0};
        CHECK(xr_compile_resources_stats(resources, &current) == XR_COMPILE_RESOURCE_OK);
        CHECK(boundary_count < sizeof(boundaries) / sizeof(boundaries[0]));
        boundaries[boundary_count++] = current.work;
    }
    return status;
}
static const XrCompileResourceLimits ample = {1048576, 1048576, 1048576};
static const char *names[] = {"alpha.xr", "beta.xr", "./alpha.xr", "child/zero.xr", "four.xr",
    "five.xr", "six.xr", "seven.xr", "eight.xr", "nine.xr", "ten.xr", "eleven.xr",
    "twelve.xr", "thirteen.xr", "unicode_\xE4\xB8\xAD.xr"};
typedef struct FileFixture {
    XrCompileResources *resources;
    XrCompilerSession *session;
    XaAnalyzer analyzer;
    XaScope *external;
    XaSymbol *kept;
    XaFileEntry *entries[15];
    XaAnalyzerFileScope outer, inner;
    const char **dirty;
    int dirty_count;
    bool root_closed;
} FileFixture;
static XrCompileResourceStats stats(XrCompileResources *resources) {
    XrCompileResourceStats result = {0};
    CHECK(xr_compile_resources_stats(resources, &result) == XR_COMPILE_RESOURCE_OK); return result;
}
static XrCompileResourceStatus current(FileFixture *fixture) {
    return xr_compile_state_status(xa_symbol_registry_state(fixture->analyzer.symbol_registry));
}
#define TRY(c) do { XrCompileResourceStatus result_ = (c); if (result_ != XR_COMPILE_RESOURCE_OK) return result_; } while (0)
static XrCompileResourceStatus open_fixture(FileFixture *fixture, bool dead,
    const XrCompileResourceLimits *limits) {
    TRY(xr_compile_resources_new(limits, &fixture->resources));
    XrCompilerSessionStatus opened = xr_compile_session_new(fixture->resources, &fixture->session);
    CHECK(opened == XR_COMPILER_SESSION_OK || opened == XR_COMPILER_SESSION_BUDGET || opened == XR_COMPILER_SESSION_OUT_OF_MEMORY);
    if (opened != XR_COMPILER_SESSION_OK) return opened == XR_COMPILER_SESSION_BUDGET
        ? XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_OUT_OF_MEMORY;
    fixture->analyzer.compiler_session = fixture->session;
    TRY(xa_analyzer_storage_open(xr_compile_session_compile_state(fixture->session), &fixture->analyzer));
    fixture->external = xa_scope_new(fixture->analyzer.symbol_registry, XA_SCOPE_GLOBAL, NULL);
    if (!fixture->external) { CHECK(current(fixture) != XR_COMPILE_RESOURCE_OK); return current(fixture); }
    if (dead) {
        xr_compile_session_free(fixture->session); fixture->session = NULL;
        xr_compile_resources_release(fixture->resources); fixture->root_closed = true;
    }
    TRY(xa_analyzer_files_open(&fixture->analyzer));
    return XR_COMPILE_RESOURCE_OK;
}
static XrCompileResourceStatus create_record(FileFixture *fixture, const char *name, XaFileEntry **output) {
    XaAnalyzer *analyzer = &fixture->analyzer; XaScope *root = analyzer->global_scope;
    XaScope **children = root->children; int capacity = root->child_capacity, count = root->child_count;
    XaScope *prior[32]; CHECK(count <= 32);
    if (count) memcpy(prior, children, (size_t)count * sizeof(*children));
    XaFileEntry *head = analyzer->files; int files = analyzer->file_count;
    uint32_t indexed = xa_analyzer_file_index_size(analyzer);
    XaFileEntry marker = {0}; XaFileEntry *entry = &marker;
    char mutable_path[64]; snprintf(mutable_path, sizeof(mutable_path), "%s", name);
    XrCompileResourceStatus status = xa_analyzer_file_get_or_create(analyzer, mutable_path, root, &entry);
    memset(mutable_path, '!', strlen(mutable_path));
    if (status != XR_COMPILE_RESOURCE_OK) {
        CHECK(entry == &marker && analyzer->files == head && analyzer->file_count == files);
        CHECK(xa_analyzer_file_index_size(analyzer) == indexed && root->child_count == count);
        CHECK(root->children == children && root->child_capacity == capacity);
        if (count) CHECK(!memcmp(prior, root->children, (size_t)count * sizeof(*children)));
        return status;
    }
    CHECK(entry != &marker && entry->path != mutable_path && !strcmp(entry->path, name));
    CHECK(entry->file_scope && entry->file_scope->parent == root && entry->file_scope->registry == analyzer->symbol_registry);
    CHECK(analyzer->file_count == files + 1 && xa_analyzer_file_index_size(analyzer) == indexed + 1);
    *output = entry;
    return status;
}
static XrCompileResourceStatus produce_records(FileFixture *fixture) {
    for (unsigned i = 0; i < 15; ++i) TRY(create_record(fixture, names[i], &fixture->entries[i]));
    CHECK(fixture->analyzer.file_count == 15 && fixture->analyzer.global_scope->child_count == 15);
    for (unsigned i = 0; i < 15; ++i) {
        XaFileEntry *found = NULL; TRY(xa_analyzer_file_find(&fixture->analyzer, names[i], &found));
        CHECK(found == fixture->entries[i]);
    }
    size_t attempts = attempt_count; XaFileEntry *same = NULL;
    TRY(xa_analyzer_file_get_or_create(&fixture->analyzer, names[0], fixture->analyzer.global_scope, &same));
    CHECK(same == fixture->entries[0] && attempts == attempt_count && fixture->analyzer.file_count == 15);
    return XR_COMPILE_RESOURCE_OK;
}
static XrCompileResourceStatus produce_alias_and_dirty(FileFixture *fixture) {
    XaAnalyzer *analyzer = &fixture->analyzer;
    fixture->kept = xa_symbol_new(analyzer->symbol_registry, "persistent", XA_SYM_VARIABLE);
    if (!fixture->kept) { CHECK(current(fixture) != XR_COMPILE_RESOURCE_OK); return current(fixture); }
    XaSymbol *symbol = fixture->kept;
    symbol->location.file = fixture->entries[0]->path; symbol->location.line = 7;
    symbol->links.type = xr_pool_alloc_type(analyzer->type_pool, XR_KIND_INT);
    if (!symbol->links.type) { xa_symbol_free(symbol); fixture->kept = NULL; return current(fixture); }
    CHECK(symbol->id == 1 && symbol->links.type->id == 1);
    XrCompileResourceStatus status = xa_scope_add_symbol(fixture->entries[0]->file_scope, symbol);
    if (status != XR_COMPILE_RESOURCE_OK) { xa_symbol_free(symbol); fixture->kept = NULL; return status; }
    if (!xa_scope_set_alias(fixture->external, "kept", symbol)) {
        CHECK(current(fixture) != XR_COMPILE_RESOURCE_OK); return current(fixture);
    }
    for (unsigned i = 0; i < 15; ++i) fixture->entries[i]->dirty = false;
    xa_analyzer_mark_file_dirty(analyzer, names[0]); TRY(current(fixture));
    xa_analyzer_mark_file_dirty(analyzer, names[1]); TRY(current(fixture));
    xa_analyzer_mark_file_dirty(analyzer, "missing.xr"); TRY(current(fixture));
    xa_analyzer_invalidate_range(analyzer, names[3], 1, 7); TRY(current(fixture));
    xa_analyzer_invalidate_range(analyzer, "new-before-save.xr", 2, 9); TRY(current(fixture));
    CHECK(analyzer->file_count == 16);
    fixture->dirty_count = -17;
    status = xa_analyzer_get_dirty_files(analyzer, &fixture->dirty, &fixture->dirty_count);
    if (status != XR_COMPILE_RESOURCE_OK) { CHECK(!fixture->dirty && fixture->dirty_count == -17); return status; }
    CHECK(fixture->dirty && fixture->dirty_count == 4);
    unsigned seen = 0;
    for (int i = 0; i < fixture->dirty_count; ++i) {
        const char *path = fixture->dirty[i]; unsigned bit = !strcmp(path, names[0]) ? 1u :
            !strcmp(path, names[1]) ? 2u : !strcmp(path, names[3]) ? 4u : !strcmp(path, "new-before-save.xr") ? 8u : 0u;
        CHECK(bit && !(seen & bit)); seen |= bit;
    }
    CHECK(seen == 15);
    return XR_COMPILE_RESOURCE_OK;
}
static XrCompileResourceStatus retire_and_restore(FileFixture *fixture) {
    XaAnalyzer *analyzer = &fixture->analyzer;
    if (!xa_analyzer_push_file_scope(analyzer, names[0], &fixture->outer)) {
        CHECK(current(fixture) != XR_COMPILE_RESOURCE_OK); return current(fixture);
    }
    CHECK(analyzer->current_scope == fixture->entries[0]->file_scope && analyzer->current_file == fixture->entries[0]->path);
    if (!xa_analyzer_push_file_scope(analyzer, names[1], &fixture->inner)) {
        CHECK(current(fixture) != XR_COMPILE_RESOURCE_OK); return current(fixture);
    }
    const char *old_path = fixture->entries[0]->path; XaScope *old_scope = fixture->entries[0]->file_scope;
    TRY(xa_scope_remove_owned_symbol(old_scope, fixture->kept));
    xa_symbol_free(fixture->kept);
    bool removed = false; TRY(xa_analyzer_file_remove_record(analyzer, names[0], &removed)); CHECK(removed);
    fixture->entries[0] = NULL;
    CHECK(analyzer->file_count == 15 && !strcmp(fixture->kept->location.file, names[0]));
    TRY(create_record(fixture, names[0], &fixture->entries[0]));
    CHECK(fixture->entries[0]->path != old_path && fixture->entries[0]->file_scope != old_scope);
    size_t attempts = attempt_count, calls = work_calls;
    xa_analyzer_pop_file_scope(analyzer, &fixture->inner);
    CHECK(!fixture->inner.active && analyzer->current_scope == analyzer->global_scope && !analyzer->current_file);
    xa_analyzer_pop_file_scope(analyzer, &fixture->outer);
    CHECK(!fixture->outer.active && !fixture->outer.owner && !fixture->outer.parent);
    CHECK(calls == work_calls && attempts == attempt_count);
    XaFileEntry *missing = (void *)(uintptr_t)1;
    TRY(xa_analyzer_file_find(analyzer, "absent.xr", &missing)); CHECK(!missing);
    bool erased = true; TRY(xa_analyzer_file_remove_record(analyzer, "absent.xr", &erased)); CHECK(!erased);
    return XR_COMPILE_RESOURCE_OK;
}
static void sticky(FileFixture *fixture, XrCompileResourceStatus status) {
    XaAnalyzer *analyzer = &fixture->analyzer;
    XrCompileResourceStats before = stats(fixture->resources); size_t attempts = attempt_count, calls = work_calls;
    XaFileEntry *entry = (void *)(uintptr_t)3; const char **dirty = (void *)(uintptr_t)5; int count = 71; bool removed = true;
    CHECK(xa_analyzer_files_open(analyzer) == status);
    CHECK(xa_analyzer_file_find(analyzer, (const char *)(uintptr_t)1, &entry) == status && entry == (void *)(uintptr_t)3);
    CHECK(xa_analyzer_file_get_or_create(analyzer, (const char *)(uintptr_t)1, (XaScope *)(uintptr_t)1, &entry) == status);
    CHECK(xa_analyzer_file_remove_record(analyzer, (const char *)(uintptr_t)1, &removed) == status && removed);
    CHECK(xa_analyzer_get_dirty_files(analyzer, &dirty, &count) == status && dirty == (void *)(uintptr_t)5 && count == 71);
    CHECK(!xa_analyzer_push_file_scope(analyzer, (const char *)(uintptr_t)1, (XaAnalyzerFileScope *)(uintptr_t)1));
    xa_analyzer_mark_file_dirty(analyzer, (const char *)(uintptr_t)1);
    xa_analyzer_invalidate_range(analyzer, (const char *)(uintptr_t)1, 0, 1);
    const char *origin = NULL;
    CHECK(xa_symbol_registry_copy_origin(analyzer->symbol_registry, (const char *)(uintptr_t)1, &origin) == status && !origin);
    CHECK(!xa_scope_new_indexed(analyzer->symbol_registry, XA_SCOPE_GLOBAL, (XaScope *)(uintptr_t)1,
        (struct XrHashMap *)(uintptr_t)1, (const char *)(uintptr_t)1, (void *)(uintptr_t)1));
    XrCompileResourceStats after = stats(fixture->resources);
    CHECK(calls == work_calls && attempts == attempt_count && before.work == after.work && before.live_bytes == after.live_bytes && before.allocated_bytes == after.allocated_bytes);
}
static XrCompileResourceStatus close_fixture(FileFixture *fixture, XrCompileResourceStatus status,
    XrCompileResourceStats *observed) {
    if (fixture->analyzer.symbol_registry && status != XR_COMPILE_RESOURCE_OK) sticky(fixture, status);
    size_t attempts = attempt_count, calls = work_calls;
    xa_analyzer_files_close(&fixture->analyzer);
    CHECK(!fixture->outer.active && !fixture->inner.active);
    xa_analyzer_storage_close(&fixture->analyzer);
    xr_compile_session_free(fixture->session); fixture->session = NULL;
    CHECK(attempts == attempt_count && calls == work_calls);
    if (status == XR_COMPILE_RESOURCE_OK && fixture->kept) {
        XaSymbol *kept = xa_scope_lookup_local(fixture->external, "kept");
        status = xa_scope_resource_status(fixture->external);
        if (status == XR_COMPILE_RESOURCE_OK) {
            CHECK(kept == fixture->kept && kept->id == 1 && !strcmp(kept->location.file, names[0]));
            CHECK(kept->links.type && kept->links.type->kind == XR_KIND_INT && kept->links.type->id == 1);
        }
    }
    if (fixture->resources) *observed = stats(fixture->resources);
    attempts = attempt_count; calls = work_calls;
    xr_compile_state_free((void *)fixture->dirty);
    xa_scope_free(fixture->external);
    if (!fixture->root_closed) xr_compile_resources_release(fixture->resources);
    CHECK(attempts == attempt_count && calls == work_calls && !physical_live && !allocation_count);
    return status;
}
static XrCompileResourceStatus pipeline(bool dead, bool observe,
    const XrCompileResourceLimits *limits, XrCompileResourceStats *observed) {
    FileFixture fixture = {0}; recording = observe;
    XrCompileResourceStatus status = open_fixture(&fixture, dead, limits);
    if (status == XR_COMPILE_RESOURCE_OK) status = produce_records(&fixture);
    if (status == XR_COMPILE_RESOURCE_OK) status = produce_alias_and_dirty(&fixture);
    if (status == XR_COMPILE_RESOURCE_OK) status = retire_and_restore(&fixture);
    status = close_fixture(&fixture, status, observed); recording = false; return status;
}
static void matrix(bool dead) {
    reset(); boundary_count = 0; XrCompileResourceStats baseline = {0};
    CHECK(pipeline(dead, true, &ample, &baseline) == XR_COMPILE_RESOURCE_OK);
    size_t sites = attempt_count, cuts = boundary_count; CHECK(sites && cuts);
    XrCompileResourceLimits exact = {baseline.allocated_bytes, baseline.peak_bytes, baseline.work};
    for (unsigned axis = 0; axis < 3; ++axis) for (int delta = -1; delta <= 1; ++delta) {
        XrCompileResourceLimits limits = exact; uint64_t *limit = axis == 0 ? &limits.allocated_bytes : axis == 1 ? &limits.live_bytes : &limits.work;
        *limit = (uint64_t)((int64_t)*limit + delta); reset(); XrCompileResourceStats observed = {0};
        CHECK(pipeline(dead, false, &limits, &observed) == (delta < 0 ? XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_OK));
    }
    for (size_t i = 0; i < sites; ++i) {
        reset(); fail_at = i; XrCompileResourceStats observed = {0};
        CHECK(pipeline(dead, false, &ample, &observed) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    }
    for (size_t i = 0; i < cuts; ++i) {
        reset(); XrCompileResourceLimits limits = ample; limits.work = boundaries[i] - 1; XrCompileResourceStats observed = {0};
        CHECK(pipeline(dead, false, &limits, &observed) == XR_COMPILE_RESOURCE_BUDGET);
    }
    printf("file storage dead%d: %zu actual allocation faults/%zu actual work cuts; parent/index atomic,retirement/cursors/origin/axes/sticky/physical0 PASS\n", dead, sites, cuts);
}
static void guards(void) {
    for (int mode = 0; mode < 7; ++mode) {
        reset(); FileFixture fixture = {0};
        CHECK(open_fixture(&fixture, false, &ample) == XR_COMPILE_RESOURCE_OK);
        CHECK(create_record(&fixture, names[0], &fixture.entries[0]) == XR_COMPILE_RESOURCE_OK);
        CHECK(create_record(&fixture, names[1], &fixture.entries[1]) == XR_COMPILE_RESOURCE_OK);
        XaAnalyzer *analyzer = &fixture.analyzer;
        if (mode >= 2 && mode <= 4) CHECK(xa_analyzer_push_file_scope(analyzer, names[0], &fixture.outer));
        if (mode == 3) CHECK(xa_analyzer_push_file_scope(analyzer, names[1], &fixture.inner));
        XaAnalyzerFileScope prior = fixture.outer, inner_prior = fixture.inner;
        XaScope *selected = analyzer->current_scope; const char *file = analyzer->current_file;
        void *storage = analyzer->files_storage; XaFileEntry *head = analyzer->files;
        XrCompileResourceStats before = stats(fixture.resources); size_t attempts = attempt_count, calls = work_calls;
        if (mode == 0) CHECK(xa_analyzer_files_open(analyzer) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        else if (mode == 1) {
            const char **output = (void *)(uintptr_t)1; int count = 99;
            CHECK(xa_analyzer_get_dirty_files(analyzer, &output, &count) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
            CHECK(output == (void *)(uintptr_t)1 && count == 99);
        } else if (mode == 2) CHECK(!xa_analyzer_push_file_scope(analyzer, names[1], &fixture.outer));
        else if (mode == 3) xa_analyzer_pop_file_scope(analyzer, &fixture.outer);
        else if (mode == 4) {
            XaAnalyzerFileScope copy = fixture.outer;
            xa_analyzer_pop_file_scope(analyzer, &copy);
            CHECK(!memcmp(&copy, &fixture.outer, sizeof(copy)));
        } else if (mode == 5) {
            XaFileEntry *output = (void *)(uintptr_t)1;
            CHECK(xa_analyzer_file_get_or_create(analyzer, "outside.xr", fixture.external, &output) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
            CHECK(output == (void *)(uintptr_t)1);
        } else CHECK(xa_analyzer_file_get_or_create(analyzer, (const char *)(uintptr_t)1,
            (XaScope *)(uintptr_t)1, NULL) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        CHECK(current(&fixture) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        CHECK(analyzer->current_scope == selected && analyzer->current_file == file && analyzer->files_storage == storage && analyzer->files == head);
        CHECK(!memcmp(&prior, &fixture.outer, sizeof(prior)) && !memcmp(&inner_prior, &fixture.inner, sizeof(inner_prior)));
        CHECK(attempts == attempt_count && calls == work_calls && stats(fixture.resources).work == before.work);
        xa_analyzer_pop_file_scope(analyzer, &fixture.inner);
        xa_analyzer_pop_file_scope(analyzer, &fixture.outer);
        CHECK(attempts == attempt_count && calls == work_calls);
        XrCompileResourceStats observed = {0};
        CHECK(close_fixture(&fixture, XR_COMPILE_RESOURCE_BAD_ARGUMENT, &observed) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    }
    for (int mode = 0; mode < 2; ++mode) {
        reset(); FileFixture fixture = {0}; CHECK(open_fixture(&fixture, false, &ample) == XR_COMPILE_RESOURCE_OK);
        CHECK(create_record(&fixture, names[0], &fixture.entries[0]) == XR_COMPILE_RESOURCE_OK);
        XrCompilerSession *peer_session = NULL; XaAnalyzer peer = {0};
        CHECK(xr_compile_session_new(fixture.resources, &peer_session) == XR_COMPILER_SESSION_OK);
        peer.compiler_session = peer_session;
        CHECK(xa_analyzer_storage_open(xr_compile_session_compile_state(peer_session), &peer) == XR_COMPILE_RESOURCE_OK);
        CHECK(xa_analyzer_files_open(&peer) == XR_COMPILE_RESOURCE_OK);
        if (mode == 0) {
            CHECK(xa_analyzer_push_file_scope(&fixture.analyzer, names[0], &fixture.outer));
            XaAnalyzerFileScope prior = fixture.outer; size_t calls = work_calls, attempts = attempt_count;
            xa_analyzer_pop_file_scope(&peer, &fixture.outer);
            CHECK(xa_scope_resource_status(peer.global_scope) == XR_COMPILE_RESOURCE_BAD_ARGUMENT && current(&fixture) == XR_COMPILE_RESOURCE_OK);
            CHECK(!memcmp(&prior, &fixture.outer, sizeof(prior)) && calls == work_calls && attempts == attempt_count);
            xa_analyzer_pop_file_scope(&fixture.analyzer, &fixture.outer);
        } else {
            XaFileEntry *output = (void *)(uintptr_t)1;
            CHECK(xa_analyzer_file_get_or_create(&fixture.analyzer, "foreign.xr", peer.global_scope, &output) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
            CHECK(output == (void *)(uintptr_t)1 && xa_scope_resource_status(peer.global_scope) == XR_COMPILE_RESOURCE_OK);
        }
        size_t calls = work_calls, attempts = attempt_count;
        xa_analyzer_files_close(&peer); xa_analyzer_storage_close(&peer); xr_compile_session_free(peer_session);
        CHECK(calls == work_calls && attempts == attempt_count);
        XrCompileResourceStats observed = {0};
        CHECK(close_fixture(&fixture, mode ? XR_COMPILE_RESOURCE_BAD_ARGUMENT : XR_COMPILE_RESOURCE_OK, &observed) ==
            (mode ? XR_COMPILE_RESOURCE_BAD_ARGUMENT : XR_COMPILE_RESOURCE_OK));
    }
    reset(); FileFixture fixture = {0}; CHECK(open_fixture(&fixture, false, &ample) == XR_COMPILE_RESOURCE_OK);
    XaFileEntry *entry = (void *)(uintptr_t)1;
    CHECK(xa_analyzer_file_find(&fixture.analyzer, "absent.xr", &entry) == XR_COMPILE_RESOURCE_OK && !entry);
    entry = (void *)(uintptr_t)1;
    CHECK(xa_analyzer_file_get_or_create(&fixture.analyzer, NULL, (XaScope *)(uintptr_t)1, &entry) == XR_COMPILE_RESOURCE_OK && !entry);
    int count = 99; const char **output = NULL;
    CHECK(xa_analyzer_get_dirty_files(&fixture.analyzer, &output, &count) == XR_COMPILE_RESOURCE_OK && !output && !count);
    CHECK(!xa_analyzer_push_file_scope(&fixture.analyzer, "missing.xr", &fixture.outer) && current(&fixture) == XR_COMPILE_RESOURCE_OK);
    CHECK(!fixture.outer.active && !fixture.outer.owner && !fixture.outer.parent && !xa_analyzer_file_index_size(&fixture.analyzer));
    XrCompileResourceStats observed = {0}; CHECK(close_fixture(&fixture, XR_COMPILE_RESOURCE_OK, &observed) == XR_COMPILE_RESOURCE_OK);
    puts("file storage guards: empty/occupied/output,real same-ledger foreign owner/parent,LIFO/copy rejection,cleanup afterfatal PASS");
}
static void foreign_index(void) {
    reset(); FileFixture fixture = {0};
    CHECK(open_fixture(&fixture, false, &ample) == XR_COMPILE_RESOURCE_OK);
    XrCompileResources *foreign = NULL;
    CHECK(xr_compile_resources_new(&ample, &foreign) == XR_COMPILE_RESOURCE_OK);
    XrOsIoPolicy policy = xr_compile_io_policy(foreign); XrHashMap *map = NULL;
    CHECK(xr_hashmap_owned_new(&policy, &map) == XR_OS_IO_OK);
    CHECK(xr_hashmap_owned_set(map, "retained", &fixture) == XR_OS_IO_OK);
    CHECK(xr_hashmap_owned_uses_policy(map, &policy));
    for (int field = 0; field < 4; ++field) {
        XrOsIoPolicy changed = policy;
        if (field == 0) changed.context = fixture.resources;
        else if (field == 1) changed.alloc = NULL;
        else if (field == 2) changed.free = NULL;
        else changed.work = NULL;
        CHECK(!xr_hashmap_owned_uses_policy(map, &changed));
    }
    CHECK(!xr_hashmap_owned_uses_policy(NULL, &policy) && !xr_hashmap_owned_uses_policy(map, NULL));
    XrCompileResourceStats local_before = stats(fixture.resources), foreign_before = stats(foreign);
    XaScope *root = fixture.analyzer.global_scope; XaScope **children = root->children;
    int count = root->child_count, capacity = root->child_capacity;
    XrHashMapEntry *entries = map->entries; size_t attempts = attempt_count, calls = work_calls;
    CHECK(!xa_scope_new_indexed(fixture.analyzer.symbol_registry, XA_SCOPE_GLOBAL, root, map, "new", &fixture));
    CHECK(current(&fixture) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(root->children == children && root->child_count == count && root->child_capacity == capacity);
    CHECK(map->entries == entries && map->count == 1 && attempts == attempt_count && calls == work_calls);
    XrCompileResourceStats local_after = stats(fixture.resources), foreign_after = stats(foreign);
    CHECK(local_before.work == local_after.work && local_before.allocated_bytes == local_after.allocated_bytes && local_before.live_bytes == local_after.live_bytes);
    CHECK(foreign_before.work == foreign_after.work && foreign_before.allocated_bytes == foreign_after.allocated_bytes && foreign_before.live_bytes == foreign_after.live_bytes);
    void *kept = NULL; CHECK(xr_hashmap_owned_get(map, "retained", &kept) == XR_OS_IO_OK && kept == &fixture);
    xr_hashmap_owned_free(map); xr_compile_resources_release(foreign);
    XrCompileResourceStats observed = {0};
    CHECK(close_fixture(&fixture, XR_COMPILE_RESOURCE_BAD_ARGUMENT, &observed) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    puts("file storage foreign index: exact policy identity, foreign ledger untouched, atomic rejection PASS");
}
int main(void) {
    matrix(false); matrix(true); guards(); foreign_index();
    return 0;
}
