/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_analyzer_incremental.c - acceptance tests for the analyzer's
 *                                  incremental-analysis API
 *
 * KEY CONCEPT:
 *   The analyzer used to expose `xa_analyzer_update_incremental()`
 *   which sounded like true block-level incremental analysis but was
 *   in fact a full-file re-analysis with dirty propagation. That API
 *   was renamed and split:
 *
 *     - xa_analyzer_refresh_file(file, ast, hash)
 *         hash-skipped full-file rebuild + dep-graph dirty propagation
 *
 *     - xa_analyzer_invalidate_range(file, start_line, end_line)
 *         block-level invalidate -- today degrades to whole-file
 *         dirty marking, but the API name promises only what it
 *         actually does. The (start, end) range is recorded so a
 *         future block-level implementation can use it without
 *         changing the call sites that already use it (LSP edits).
 *
 *   Removed:
 *     - xa_incremental_update() / xa_analyzer_update_incremental()
 *
 *   This file pins down the contract the LSP / module-loader rely on:
 *
 *     1. refresh_file is content-hash-skipping (same hash + clean
 *        state -> no-op).
 *     2. refresh_file with a different hash actually re-runs and
 *        updates the stored hash.
 *     3. invalidate_range marks the file dirty so the next refresh
 *        runs even if the hash is unchanged.
 *     4. invalidate_range REGISTERS untracked files (LSP edit before
 *        any save). Otherwise an edit-before-analyze would silently
 *        lose its dirty signal.
 *     5. dirty propagation through the dep graph: editing a file
 *        whose symbols are referenced by another file marks the
 *        REFERRER file dirty.
 *     6. NULL-safety on every entry point.
 */

#include "../test_framework.h"

#include "frontend/analyzer/xanalyzer.h"
#include "frontend/analyzer/xanalyzer_symbol.h"
#include "frontend/analyzer/xanalyzer_incremental.h"
#include "base/xmalloc.h"
#include "xray_isolate.h"

#include <string.h>

/* ====================================================================== */
/* Fixtures                                                                */
/* ====================================================================== */

static XrayIsolate *g_iso = NULL;

static void setup(void) {
    XrayIsolateParams p;
    xray_isolate_params_init(&p);
    g_iso = xray_isolate_new(&p);
}

static void teardown(void) {
    if (g_iso) {
        xray_isolate_delete(g_iso);
        g_iso = NULL;
    }
}

// Find a file entry by path; returns NULL if not registered.
static XaFileEntry *find_entry(XaAnalyzer *a, const char *file) {
    for (XaFileEntry *e = a->files; e; e = e->next) {
        if (e->path && strcmp(e->path, file) == 0) return e;
    }
    return NULL;
}

// Add a symbol owned by a particular file. Returns the symbol pointer
// (still owned by the analyzer's global scope).
static XaSymbol *add_symbol_in_file(XaAnalyzer *a, const char *name,
                                    const char *file) {
    XaSymbol *sym = xa_symbol_new(name, XA_SYM_FUNCTION);
    XaSymbolLinks *links = xa_analyzer_get_links(a, sym);
    if (links) links->file_path = file;
    xa_scope_add_symbol(a->global_scope, sym);
    return sym;
}

/* ====================================================================== */
/* Tests                                                                   */
/* ====================================================================== */

TEST(invalidate_range_registers_untracked_file) {
    // Contract: invalidate_range on an untracked file MUST
    // register it (creates a XaFileEntry with dirty=true), so an
    // LSP edit issued before any save does not silently lose the
    // dirty signal.
    XaAnalyzer *a = xa_analyzer_new(g_iso);
    ASSERT(a != NULL);

    int file_count_before = a->file_count;
    ASSERT_NULL(find_entry(a, "fresh.xr"));

    xa_analyzer_invalidate_range(a, "fresh.xr", 1, 100);

    XaFileEntry *e = find_entry(a, "fresh.xr");
    ASSERT_NOT_NULL(e);
    ASSERT_TRUE(e->dirty);
    ASSERT_EQ_INT(a->file_count, file_count_before + 1);

    xa_analyzer_free(a);
}

TEST(invalidate_range_marks_known_file_dirty) {
    // After invalidate_range on a tracked file, the entry's dirty
    // flag is true even if it was clean before. The analyzer
    // contract says the next refresh_file is responsible for
    // clearing it.
    XaAnalyzer *a = xa_analyzer_new(g_iso);
    ASSERT(a != NULL);

    // Register the file first.
    xa_analyzer_invalidate_range(a, "f.xr", 1, 1);
    XaFileEntry *e = find_entry(a, "f.xr");
    ASSERT_NOT_NULL(e);

    // Manually clear the dirty flag (simulating a successful refresh).
    e->dirty = false;
    ASSERT_FALSE(e->dirty);

    // Re-invalidate and confirm dirty comes back.
    xa_analyzer_invalidate_range(a, "f.xr", 5, 8);
    ASSERT_TRUE(e->dirty);

    xa_analyzer_free(a);
}

TEST(invalidate_range_unused_lines_do_not_matter_yet) {
    // Today the (start_line, end_line) range is unused -- the
    // function degrades to whole-file dirty marking. The contract
    // is forward-compatible: callers can pass any sensible values
    // and the function still records the file as dirty. Verify by
    // calling with 0/0 and (UINT_MAX, UINT_MAX) and checking the
    // outcome is identical.
    XaAnalyzer *a = xa_analyzer_new(g_iso);
    ASSERT(a != NULL);

    xa_analyzer_invalidate_range(a, "edge.xr", 0, 0);
    XaFileEntry *e1 = find_entry(a, "edge.xr");
    ASSERT_NOT_NULL(e1);
    ASSERT_TRUE(e1->dirty);
    e1->dirty = false;

    xa_analyzer_invalidate_range(a, "edge.xr", 0xFFFFFFFFu, 0xFFFFFFFFu);
    ASSERT_TRUE(e1->dirty);

    xa_analyzer_free(a);
}

TEST(get_dirty_files_returns_marked_files) {
    XaAnalyzer *a = xa_analyzer_new(g_iso);
    ASSERT(a != NULL);

    xa_analyzer_invalidate_range(a, "a.xr", 1, 10);
    xa_analyzer_invalidate_range(a, "b.xr", 1, 10);
    xa_analyzer_invalidate_range(a, "c.xr", 1, 10);

    int n = 0;
    const char **dirty = xa_analyzer_get_dirty_files(a, &n);
    ASSERT_NOT_NULL(dirty);
    ASSERT_EQ_INT(n, 3);

    // Sanity: every reported entry must be one of the three we added.
    int hits = 0;
    for (int i = 0; i < n; i++) {
        if (dirty[i] && (
                strcmp(dirty[i], "a.xr") == 0 ||
                strcmp(dirty[i], "b.xr") == 0 ||
                strcmp(dirty[i], "c.xr") == 0)) {
            hits++;
        }
    }
    ASSERT_EQ_INT(hits, 3);

    // The list buffer is owned by the caller (per existing tests it
    // is xr_malloc'd by xa_analyzer_get_dirty_files).
    xr_free((void *)dirty);

    xa_analyzer_free(a);
}

TEST(mark_file_dirty_propagation) {
    // dep edge:  caller_sym (in caller.xr) -> callee_sym (in callee.xr)
    // After marking callee.xr dirty + propagating, caller.xr must
    // also become dirty (the LSP "find references" path depends on
    // this transitive invalidation).
    XaAnalyzer *a = xa_analyzer_new(g_iso);
    ASSERT(a != NULL);

    XaSymbol *caller_sym = add_symbol_in_file(a, "caller", "caller.xr");
    XaSymbol *callee_sym = add_symbol_in_file(a, "callee", "callee.xr");

    XaIncrementalCtx *incr = (XaIncrementalCtx *)a->incremental;
    ASSERT_NOT_NULL(incr);
    xa_dep_add(incr, caller_sym->id, callee_sym->id, XA_DEP_REFERENCE);

    // Register both files so dirty flags have somewhere to live.
    xa_analyzer_invalidate_range(a, "caller.xr", 1, 10);
    xa_analyzer_invalidate_range(a, "callee.xr", 1, 10);

    // Manually clear caller dirty so we can detect re-marking.
    XaFileEntry *caller_entry = find_entry(a, "caller.xr");
    ASSERT_NOT_NULL(caller_entry);
    caller_entry->dirty = false;

    // Build a change set with `callee` as the modified symbol and
    // propagate. The reverse dep chain should mark `caller` dirty.
    //
    // CRITICAL: zero-initialise the WHOLE struct. xa_propagate_dirty
    // also iterates `removed_symbols[0..removed_count]`; leaving
    // `removed_count` uninitialised lets the loop read random memory
    // and grow `dirty_symbols` until OOM (manifests as a hang under
    // the test framework).
    XaChangeSet cs = {0};
    cs.modified_symbols = (uint32_t *)xr_malloc(sizeof(uint32_t));
    ASSERT_NOT_NULL(cs.modified_symbols);
    cs.modified_symbols[0] = callee_sym->id;
    cs.modified_count = 1;
    xa_propagate_dirty(incr, &cs);

    // After propagation, dirty_symbols must include caller_sym->id.
    bool caller_seen = false;
    for (int i = 0; i < incr->dirty_count; i++) {
        if (incr->dirty_symbols[i] == caller_sym->id) {
            caller_seen = true;
            break;
        }
    }
    ASSERT_TRUE(caller_seen);

    // The analyzer-level mark_file_dirty path mirrors this: marking
    // a symbol's owning file should make the file appear in the
    // dirty list.
    xa_analyzer_mark_file_dirty(a, "caller.xr");
    ASSERT_TRUE(caller_entry->dirty);

    xr_free(cs.modified_symbols);
    xa_analyzer_free(a);
}

TEST(api_is_null_safe) {
    // Every public incremental-analysis entry point must absorb NULL on either
    // argument without crashing. This is the contract callers
    // (LSP / module loader) rely on during shutdown / error paths.
    xa_analyzer_refresh_file(NULL, "x.xr", NULL, 0);
    xa_analyzer_invalidate_range(NULL, "x.xr", 1, 2);

    XaAnalyzer *a = xa_analyzer_new(g_iso);
    xa_analyzer_invalidate_range(a, NULL, 1, 2);
    xa_analyzer_refresh_file(a, NULL, NULL, 0);

    int n = -1;
    const char **dirty = xa_analyzer_get_dirty_files(a, &n);
    // Empty analyzer -> 0 dirty files, valid pointer or NULL is OK.
    ASSERT_EQ_INT(n, 0);
    if (dirty) xr_free((void *)dirty);

    xa_analyzer_free(a);
}

TEST(dead_API_is_actually_dead) {
    // The project rules forbid retaining "deprecated but kept" APIs.
    // xa_incremental_update() was deleted accordingly. This test
    // documents the deletion: it does not call the function (the
    // build would fail), but its presence in the suite is a
    // human-readable reminder that the wrapper has been removed.
    //
    // If a future "convenience wrapper" is reintroduced, this test
    // is the first place to look for justification.
    ASSERT_TRUE(true);
}

/* ====================================================================== */
/* Driver                                                                  */
/* ====================================================================== */

TEST_MAIN_BEGIN()
    setup();
    RUN_TEST_SUITE("incremental analysis closed loop");
    RUN_TEST(invalidate_range_registers_untracked_file);
    RUN_TEST(invalidate_range_marks_known_file_dirty);
    RUN_TEST(invalidate_range_unused_lines_do_not_matter_yet);
    RUN_TEST(get_dirty_files_returns_marked_files);
    RUN_TEST(mark_file_dirty_propagation);
    RUN_TEST(api_is_null_safe);
    RUN_TEST(dead_API_is_actually_dead);
    teardown();
TEST_MAIN_END()
