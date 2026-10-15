/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_method_table.h"
#include "app/lsp/xlsp_lifecycle_state.h"
#include "base/xmalloc.h"
#include "base/xhash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d:%s\n", __LINE__, #c); exit(1); } } while (0)
typedef struct { void *pointer; size_t bytes; } Block;
static Block blocks[512];
static size_t calls, fail_at = SIZE_MAX, live, bytes;
static bool injected;
static void *table_alloc(size_t size) {
    if (calls++ == fail_at) { injected = true; return NULL; }
    void *pointer = xr_malloc(size); if (!pointer) return NULL;
    size_t i = 0; while (i < 512 && blocks[i].pointer) ++i; CHECK(i < 512);
    blocks[i] = (Block){pointer, size}; ++live; bytes += size; return pointer;
}
static void table_free(void *pointer) {
    if (!pointer) return;
    size_t i = 0; while (i < 512 && blocks[i].pointer != pointer) ++i;
    CHECK(i < 512 && live && bytes >= blocks[i].bytes);
    --live; bytes -= blocks[i].bytes; blocks[i] = (Block){0}; xr_free(pointer);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(n) table_alloc(n)
#define xr_free(p) table_free(p)
#include "app/lsp/xlsp_method_table.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")
#include "registry_names.h"
#define EXTRA_METHODS 65
#define METHOD_COUNT (sizeof(registry_names) / sizeof(registry_names[0]) + EXTRA_METHODS)

static XrLspServer initial(void) {
    XrLspServer server = {0}; server.next_request_id = 71; server.source_revision = 9;
    server.source_failure.stage = 8; server.source_failure.status = 6; return server;
}
static void all_found(const XrLspServer *server, const LspMethodEntry *entries) {
    size_t before = calls;
    for (size_t i = 0; i < METHOD_COUNT; ++i)
        CHECK(xlsp_method_table_find(server, entries[i].method) == &entries[i]);
    CHECK(!xlsp_method_table_find(server, "not/registered"));
    CHECK(!xlsp_method_table_find(server, "Shutdown"));
    CHECK(!xlsp_method_table_find(server, NULL)); CHECK(!xlsp_method_table_find(server, ""));
    CHECK(calls == before);
}
static void only_table_changed(const XrLspServer *before, const XrLspServer *after) {
    XrLspServer expected = *before; expected.method_table_initialized = true;
    memcpy(expected.method_hash_table, after->method_hash_table, sizeof(expected.method_hash_table));
    CHECK(!memcmp(&expected, after, sizeof(expected)));
}
static void invalid_cases(LspMethodEntry *entries) {
    XrLspServer server = initial(), before = server; size_t allocated = calls;
    CHECK(xlsp_method_table_initialize(NULL, entries, METHOD_COUNT) == XR_XIR_BAD_STRUCTURE);
    CHECK(xlsp_method_table_initialize(&server, NULL, METHOD_COUNT) == XR_XIR_BAD_STRUCTURE);
    CHECK(xlsp_method_table_initialize(&server, entries, 0) == XR_XIR_BAD_STRUCTURE);
    LspMethodEntry saved = entries[METHOD_COUNT - 1];
    const LspMethodEntry malformed[] = {{NULL, xlsp_handle_lc_shutdown, NULL},
        {"", xlsp_handle_lc_shutdown, NULL}, {"\xff", xlsp_handle_lc_shutdown, NULL},
        {"two", xlsp_handle_lc_shutdown, xlsp_handle_lc_exit}, {"neither", NULL, NULL},
        {"shutdown", xlsp_handle_lc_shutdown, NULL}};
    for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); ++i) {
        entries[METHOD_COUNT - 1] = malformed[i];
        CHECK(xlsp_method_table_initialize(&server, entries, METHOD_COUNT) == XR_XIR_BAD_STRUCTURE);
        CHECK(!memcmp(&before, &server, sizeof(server)) && calls == allocated && !live && !bytes);
    }
    entries[METHOD_COUNT - 1] = saved;
    CHECK(!xlsp_method_table_find(NULL, "shutdown"));
    CHECK(!xlsp_method_table_find(&server, "shutdown")); xlsp_method_table_free(NULL);
}
static void ordinary(LspMethodEntry *entries) {
    XrLspServer left = initial(), right = initial(), before = left;
    size_t allocated = calls;
    CHECK(xlsp_method_table_initialize(&left, entries, METHOD_COUNT) == XR_XIR_OK);
    CHECK(calls == allocated + METHOD_COUNT && live == METHOD_COUNT); only_table_changed(&before, &left);
    all_found(&left, entries); XrLspServer published = left;
    CHECK(xlsp_method_table_initialize(&left, entries, METHOD_COUNT) == XR_XIR_BAD_STAGE);
    CHECK(!memcmp(&published, &left, sizeof(left)) && calls == allocated + METHOD_COUNT);
    right.method_hash_table[0] = left.method_hash_table[0];
    if (!right.method_hash_table[0]) {
        for (size_t i = 1; i < METHOD_HASH_SIZE; ++i)
            if (left.method_hash_table[i]) { right.method_hash_table[0] = left.method_hash_table[i]; break; }
    }
    XrLspServer dirty = right;
    CHECK(xlsp_method_table_initialize(&right, entries, METHOD_COUNT) == XR_XIR_BAD_STAGE);
    CHECK(!memcmp(&dirty, &right, sizeof(right))); right = initial();
    CHECK(xlsp_method_table_initialize(&right, entries, METHOD_COUNT) == XR_XIR_OK);
    CHECK(live == 2 * METHOD_COUNT); all_found(&right, entries);
    size_t collisions = 0;
    for (size_t i = 0; i < METHOD_HASH_SIZE; ++i)
        if (left.method_hash_table[i] && left.method_hash_table[i]->next) ++collisions;
    CHECK(collisions > 0);
    xlsp_method_table_free(&left); CHECK(!memcmp(&left, &before, sizeof(left)));
    all_found(&right, entries); CHECK(live == METHOD_COUNT);
    xlsp_method_table_free(&right); CHECK(!memcmp(&right, &before, sizeof(right)) && !live && !bytes);
    allocated = calls; xlsp_method_table_free(&right); CHECK(calls == allocated);
}
static void callbacks(LspMethodEntry *entries) {
    XrLspServer server = initial(); server.initialized = true;
    CHECK(xlsp_method_table_initialize(&server, entries, METHOD_COUNT) == XR_XIR_OK);
    const LspMethodEntry *shutdown = xlsp_method_table_find(&server, "shutdown");
    const LspMethodEntry *exiting = xlsp_method_table_find(&server, "exit");
    CHECK(shutdown && shutdown->request_handler == xlsp_handle_lc_shutdown);
    CHECK(exiting && exiting->notification_handler == xlsp_handle_lc_exit);
    XrJsonValue *result = shutdown->request_handler(&server, NULL);
    CHECK(result && result->type == XR_JSON_NULL && server.shutdown_received && !server.exit_received);
    xjson_free(result); exiting->notification_handler(&server, NULL); CHECK(server.exit_received);
    xlsp_method_table_free(&server); CHECK(!live && !bytes);
}
static size_t failure_cases(LspMethodEntry *entries, bool full) {
    XrLspServer owner = initial(); CHECK(xlsp_method_table_initialize(&owner, entries, METHOD_COUNT) == XR_XIR_OK);
    XrLspServer owner_before = owner;
    size_t selected = full ? METHOD_COUNT : 3;
    for (size_t sample = 0; sample < selected; ++sample) {
        size_t point = full ? sample : sample == 0 ? 0 : sample == 1 ? METHOD_COUNT / 2 : METHOD_COUNT - 1;
        XrLspServer receiver = initial(), before = receiver; size_t old_live = live, old_bytes = bytes;
        fail_at = calls + point; injected = false;
        CHECK(xlsp_method_table_initialize(&receiver, entries, METHOD_COUNT) == XR_XIR_OUT_OF_MEMORY);
        CHECK(injected && !memcmp(&before, &receiver, sizeof(receiver)));
        CHECK(live == old_live && bytes == old_bytes && !memcmp(&owner_before, &owner, sizeof(owner)));
        all_found(&owner, entries); CHECK(!xlsp_method_table_find(&receiver, "shutdown"));
        fail_at = SIZE_MAX;
        CHECK(xlsp_method_table_initialize(&receiver, entries, METHOD_COUNT) == XR_XIR_OK);
        only_table_changed(&before, &receiver); all_found(&receiver, entries); all_found(&owner, entries);
        CHECK(live == old_live + METHOD_COUNT); xlsp_method_table_free(&receiver);
        CHECK(!memcmp(&before, &receiver, sizeof(receiver)) && live == old_live && bytes == old_bytes);
    }
    xlsp_method_table_free(&owner); CHECK(!live && !bytes); return selected;
}
int main(void) {
    LspMethodEntry entries[METHOD_COUNT]; char extra_names[EXTRA_METHODS][32];
    size_t canonical = sizeof(registry_names) / sizeof(registry_names[0]);
    for (size_t i = 0; i < canonical; ++i)
        entries[i] = registry_names[i].request ? (LspMethodEntry){registry_names[i].name, xlsp_handle_lc_shutdown, NULL} :
                                               (LspMethodEntry){registry_names[i].name, NULL, xlsp_handle_lc_exit};
    for (size_t i = 0; i < EXTRA_METHODS; ++i) {
        snprintf(extra_names[i], sizeof(extra_names[i]), "fixture/λ/%zu", i);
        entries[canonical + i] = (LspMethodEntry){extra_names[i], xlsp_handle_lc_shutdown, NULL};
    }
    invalid_cases(entries); ordinary(entries); callbacks(entries);
    bool full = getenv("XR_LSP_METHOD_TABLE_NORMAL_ONLY") == NULL;
    size_t points = failure_cases(entries, full);
    CHECK(!live && !bytes);
    printf("{\"canonical_names\":%zu,\"entries\":%zu,\"invalid_cases\":9,\"full\":%s,\"physical_failure_points\":%zu,\"live\":%zu,\"bytes\":%zu}\n",
        canonical, (size_t)METHOD_COUNT, full ? "true" : "false", points, live, bytes);
    return 0;
}
