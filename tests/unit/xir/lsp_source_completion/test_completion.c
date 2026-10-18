/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_source_completion.h"
#include "app/lsp/xlsp_pending_requests.h"
#include "app/lsp/xlsp_protocol_output.h"
#include "app/lsp/xlsp_index_json.h"
#include "base/xmalloc.h"
#include "os/os_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d:%s\n", __LINE__, #c); exit(1); } } while (0)
static void native_free(void *p) { xr_free(p); }
#include "lsp_open_json_owner.h"
#pragma push_macro("xr_free")
#undef xr_free
#define xr_free(p) lsp_json_free(p)
#include "app/lsp/xlsp_protocol_output.c"
#pragma pop_macro("xr_free")

static const char *status_name(XrXirStatus status) {
    return status == XR_XIR_OK ? "OK" : status == XR_XIR_OUT_OF_MEMORY ? "OOM" : status == XR_XIR_IO ? "IO" :
        status == XR_XIR_BAD_STAGE ? "BAD_STAGE" : status == XR_XIR_BAD_STRUCTURE ? "BAD_STRUCTURE" : "UNEXPECTED";
}
static XrJsonValue *payload(unsigned mode) {
    XrJsonValue *object = xjson_new_object(); if (!object) return NULL;
    const char text[] = "owned\0λ\\\n\"";
    char grown[4096];
    for (size_t i = 0; i < sizeof(grown); ++i)
        grown[i] = i % 5 == 0 ? 0 : i % 5 == 1 ? '"' : i % 5 == 2 ? '\\' : i % 5 == 3 ? '\t' : 'x';
    if (!index_json_member(object, "origin", xjson_new_string("captured")) ||
        !index_json_member(object, "value", xjson_new_string_n(mode == 33 ? grown : text,
            mode == 33 ? sizeof(grown) : sizeof(text) - 1))) {
        xjson_free(object); return NULL;
    }
    return object;
}
static XlspRequestId identity(unsigned mode) {
    const char *text = mode == 13 || mode == 32 ? "{\"id\":\"owned\\u0000λ\"}" : mode == 14 ? "{\"id\":-1}" :
        mode == 15 ? "{\"id\":2147483647}" : "{\"id\":0}";
    XrJsonValue *json = xjson_parse(text, strlen(text)); CHECK(json); XlspRequestId id = {0};
    CHECK(xlsp_request_id_parse(json, &id) == XLSP_PROTOCOL_OK); xjson_free(json); return id;
}
static void acknowledge(XrLspTransport *transport) {
    fprintf(stderr, "READY\n"); fflush(stderr);
    for (unsigned waits = 0;; ++waits) {
        size_t length = 0; bool blocked = false;
        char *body = xlsp_transport_try_read(transport, &length, &blocked);
        if (body) { CHECK(length == 2 && !memcmp(body, "{}", 2)); native_free(body); return; }
        CHECK(transport->connected && blocked && waits < 1000); xr_time_sleep_ms(1);
    }
}
int main(int argc, char **argv) {
    CHECK(argc >= 2); unsigned mode = (unsigned)strtoul(argv[1], NULL, 10); CHECK(mode <= 33);
    XrLspTransport *transport = xlsp_transport_stdio(); CHECK(transport);
    XrLspServer server = {0}; server.transport = transport; server.source_revision = 5; server.next_request_id = 81;
    server.source_failure.stage = 13; server.source_failure.status = 31;
    XlspRequestId id = identity(mode), slot_id = {0};
    CHECK(xlsp_request_id_clone(&id, &slot_id) == XLSP_PROTOCOL_OK);
    CHECK(xlsp_pending_request_add(&server.pending_requests, slot_id, "textDocument/hover", 99));
    XlspPendingRequest *pending = xlsp_pending_request_find(&server.pending_requests, &id); CHECK(pending);
    pending->source_task_id = 7;
    if (mode == 13 || mode == 32) CHECK(pending->id.as.string != id.as.string);
    XlspSourceCompletion completion = {.id = &id, .task_id = 7, .revision = 5,
        .worker_completed = mode != 4 && mode != 30};
    XlspSourceRequestResult result = {.status = XR_XIR_OK};
    if (mode == 1 || mode == 30) server.shutdown_received = true;
    if (mode == 2 || mode == 31) server.exit_received = true;
    if (mode == 3 || mode == 29) pending->cancelled = true;
    if (mode == 5 || mode == 31) result.cancelled = true;
    if (mode == 6 || mode == 29) ++server.source_revision;
    if (mode == 7 || mode == 29) { result.status = XR_XIR_BAD_TYPE; result.stage = 6; }
    if (mode == 8) { result.status = XR_XIR_OUT_OF_MEMORY; result.stage = 1; }
    if (mode == 10) completion.task_id = 8;
    if (mode == 11) CHECK(xlsp_pending_request_remove(&server.pending_requests, &id));
    if (mode == 12) completion.task_id = 0;
    if (mode == 18) server.transport = NULL;
    if (mode == 19) acknowledge(transport);
    if (mode == 21 || mode == 32) {
        CHECK(xlsp_pending_request_remove(&server.pending_requests, &id)); slot_id = (XlspRequestId){0};
        CHECK(xlsp_request_id_clone(&id, &slot_id) == XLSP_PROTOCOL_OK);
        CHECK(xlsp_pending_request_add(&server.pending_requests, slot_id, "textDocument/references", 100));
        pending = xlsp_pending_request_find(&server.pending_requests, &id); CHECK(pending); pending->source_task_id = 9;
    }
    XlspRequestId empty = {0}; if (mode == 23) completion.id = &empty;
    if (mode == 24) transport->connected = false;
    if (mode == 25) completion.revision = server.source_revision = UINT64_MAX;
    if (mode == 26) { server.source_revision = UINT64_MAX; completion.revision = 0; }
    XrLspServer before = server; int slot = -1;
    for (int i = 0; i < MAX_PENDING_REQUESTS; ++i)
        if (xlsp_request_id_equals(&before.pending_requests.requests[i].id, &id)) slot = i;
    lsp_json_calls = 0; lsp_json_injected = false;
    lsp_json_fail = argc > 2 ? (size_t)strtoull(argv[2], NULL, 10) : SIZE_MAX;
    bool producer_failed = false;
    if (mode != 8 && mode != 9 && mode != 22) {
        result.json = mode == 27 ? xjson_new_null() : mode == 28 ? xjson_new_array() : payload(mode);
        if (!result.json) { producer_failed = true; result.status = XR_XIR_OUT_OF_MEMORY; result.stage = 99; }
    }
    XlspSourceRequestResult result_before = result;
    bool admitted = mode != 10 && mode != 11 && mode != 12 && mode != 16 && mode != 17 &&
        mode != 21 && mode != 22 && mode != 23 && mode != 32;
    XrXirStatus status = xlsp_source_completion_publish(mode == 16 ? NULL : &server,
        mode == 17 ? NULL : &completion, mode == 22 ? NULL : &result);
    CHECK(!result.json && result.status == result_before.status && result.stage == result_before.stage &&
        result.cancelled == result_before.cancelled);
    XrLspServer expected = before;
    if (admitted) {
        CHECK(slot >= 0); memset(&expected.pending_requests.requests[slot], 0, sizeof(XlspPendingRequest));
        --expected.pending_requests.count; if (status != XR_XIR_OK) expected.exit_received = true;
    }
    CHECK(!memcmp(&expected, &server, sizeof(server)));
    if (mode == 20) {
        CHECK(xlsp_source_completion_publish(&server, &completion, &result) == XR_XIR_BAD_STAGE);
        CHECK(!memcmp(&expected, &server, sizeof(server)));
    }
    int remaining = server.pending_requests.count;
    if (mode == 32) {
        XlspRequestId survivor = {0}; CHECK(xlsp_request_id_clone(&id, &survivor) == XLSP_PROTOCOL_OK);
        xlsp_request_id_free(&id); pending = xlsp_pending_request_find(&server.pending_requests, &survivor);
        CHECK(pending && pending->source_task_id == 9 && pending->id.as.string != survivor.as.string);
        xlsp_request_id_free(&survivor);
    }
    for (int i = 0; i < MAX_PENDING_REQUESTS; ++i)
        if (server.pending_requests.requests[i].id.kind != XLSP_ID_NONE)
            CHECK(xlsp_pending_request_remove(&server.pending_requests, &server.pending_requests.requests[i].id));
    CHECK(!server.pending_requests.count); xlsp_request_id_free(&id); xlsp_transport_free(transport);
    CHECK(!lsp_json_live && !lsp_json_bytes);
    fprintf(stderr, "STATS mode=%u status=%s attempts=%zu injected=%u producer=%u admitted=%u pending=%d exit=%u live=%zu bytes=%zu\n",
        mode, status_name(status), lsp_json_calls, lsp_json_injected ? 1u : 0u, producer_failed ? 1u : 0u,
        admitted ? 1u : 0u, remaining, server.exit_received ? 1u : 0u, lsp_json_live, lsp_json_bytes);
    return 0;
}
