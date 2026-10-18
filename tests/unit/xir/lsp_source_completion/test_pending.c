/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_pending_requests.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d:%s\n", __LINE__, #c); exit(1); } } while (0)
static XlspRequestId identity(unsigned n, bool string) {
    if (!string) return (XlspRequestId){.kind = XLSP_ID_NUMBER, .as.number = (int32_t)n};
    char input[96]; int length = snprintf(input, sizeof(input), "{\"id\":\"%u\\u0000λ\"}", n);
    CHECK(length > 0 && (size_t)length < sizeof(input));
    XrJsonValue *json = xjson_parse(input, (size_t)length); CHECK(json); XlspRequestId id = {0};
    CHECK(xlsp_request_id_parse(json, &id) == XLSP_PROTOCOL_OK); xjson_free(json); return id;
}
static void add(XlspPendingRequests *pending, unsigned n, bool string) {
    XlspRequestId producer = identity(n, string), owned = {0};
    CHECK(xlsp_request_id_clone(&producer, &owned) == XLSP_PROTOCOL_OK);
    CHECK(xlsp_pending_request_add(pending, owned, "fixture/request", 99));
    xlsp_request_id_free(&producer);
    XlspRequestId key = identity(n, string); XlspPendingRequest *entry = xlsp_pending_request_find(pending, &key);
    CHECK(entry && entry->start_time == 99 && !entry->cancelled && !entry->source_task_id);
    xlsp_request_id_free(&key);
}
static void remove_entry(XlspPendingRequests *pending, unsigned n, bool string) {
    XlspRequestId key = identity(n, string); XlspPendingRequest *entry = xlsp_pending_request_find(pending, &key);
    CHECK(entry); entry->cancelled = true; entry->source_task_id = 45;
    CHECK(xlsp_pending_request_remove(pending, &key)); XlspPendingRequest empty = {0};
    CHECK(!memcmp(entry, &empty, sizeof(empty)) && !xlsp_pending_request_remove(pending, &key));
    xlsp_request_id_free(&key);
}
static void bounds(void) {
    XlspPendingRequests pending = {0}; XlspRequestId id = identity(0, true);
    const int heads[] = {-1, MAX_PENDING_REQUESTS, 0, 0}; const int counts[] = {0, 0, -1, MAX_PENDING_REQUESTS + 1};
    for (size_t i = 0; i < 4; ++i) {
        pending.head = heads[i]; pending.count = counts[i]; XlspPendingRequests before = pending;
        CHECK(!xlsp_pending_request_add(&pending, id, "fixture", 0));
        CHECK(!xlsp_pending_request_find(&pending, &id)); CHECK(!xlsp_pending_request_remove(&pending, &id));
        CHECK(!memcmp(&before, &pending, sizeof(pending)));
    }
    pending = (XlspPendingRequests){0}; XlspPendingRequests before = pending; XlspRequestId empty = {0};
    CHECK(!xlsp_pending_request_add(&pending, id, NULL, 0)); CHECK(!xlsp_pending_request_add(&pending, id, "", 0));
    CHECK(!xlsp_pending_request_add(&pending, empty, "fixture", 0));
    CHECK(!xlsp_pending_request_add(NULL, id, "fixture", 0)); CHECK(!xlsp_pending_request_find(NULL, &id));
    CHECK(!xlsp_pending_request_remove(NULL, &id)); CHECK(!memcmp(&before, &pending, sizeof(pending)));
    pending.count = 1; before = pending; CHECK(!xlsp_pending_request_add(&pending, id, "fixture", 0));
    CHECK(!memcmp(&before, &pending, sizeof(pending))); xlsp_request_id_free(&id);
}
int main(void) {
    bounds(); XlspPendingRequests pending = {0};
    for (unsigned i = 0; i < MAX_PENDING_REQUESTS; ++i) add(&pending, i / 2, (i & 1) != 0);
    CHECK(pending.count == MAX_PENDING_REQUESTS && pending.head == 0);
    XlspRequestId extra = identity(9999, true); XlspPendingRequests before = pending;
    CHECK(!xlsp_pending_request_add(&pending, extra, "fixture", 0)); CHECK(!memcmp(&before, &pending, sizeof(pending)));
    pending.count = MAX_PENDING_REQUESTS - 1; before = pending;
    CHECK(!xlsp_pending_request_add(&pending, extra, "inconsistent", 0));
    CHECK(!memcmp(&before, &pending, sizeof(pending))); pending.count = MAX_PENDING_REQUESTS;
    xlsp_request_id_free(&extra);
    for (unsigned i = 0; i < MAX_PENDING_REQUESTS; i += 2) remove_entry(&pending, i / 2, false);
    extra = identity(0, true); before = pending;
    CHECK(!xlsp_pending_request_add(&pending, extra, "duplicate", 0)); CHECK(!memcmp(&before, &pending, sizeof(pending)));
    xlsp_request_id_free(&extra);
    for (unsigned i = 0; i < MAX_PENDING_REQUESTS / 2; ++i) add(&pending, 1000 + i, true);
    CHECK(pending.count == MAX_PENDING_REQUESTS);
    for (unsigned i = 0; i < MAX_PENDING_REQUESTS / 2; ++i) {
        remove_entry(&pending, i, true); remove_entry(&pending, 1000 + i, true);
    }
    CHECK(!pending.count);
    for (unsigned i = 0; i < 4096; ++i) { add(&pending, i, (i & 1) != 0); remove_entry(&pending, i, (i & 1) != 0); }
    XlspRequestId min = {.kind = XLSP_ID_NUMBER, .as.number = INT32_MIN};
    XlspRequestId max = {.kind = XLSP_ID_NUMBER, .as.number = INT32_MAX};
    CHECK(xlsp_pending_request_add(&pending, min, "min", 0)); CHECK(xlsp_pending_request_add(&pending, max, "max", UINT64_MAX));
    CHECK(xlsp_pending_request_remove(&pending, &min)); CHECK(xlsp_pending_request_remove(&pending, &max));
    CHECK(!pending.count);
    for (int i = 0; i < MAX_PENDING_REQUESTS; ++i) { XlspPendingRequest empty = {0}; CHECK(!memcmp(&empty, &pending.requests[i], sizeof(empty))); }
    puts("{\"capacity\":64,\"ring_cycles\":4096,\"owned_string_nul\":true,\"numeric_zero_min_max\":true,\"all_slots_cleared\":true}");
    return 0;
}
