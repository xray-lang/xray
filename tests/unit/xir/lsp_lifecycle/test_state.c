/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_lifecycle_state.h"
#include "app/lsp/xlsp_server.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d:%s\n", __LINE__, #c); exit(1); } } while (0)
#include "lsp_open_json_owner.h"
#include "app/lsp/xlsp_lifecycle_state.c"

static unsigned matrix(void) {
    const char *methods[] = {"initialize", "initialized", "shutdown", "exit", "$/cancelRequest",
        "window/workDoneProgress/cancel", "textDocument/didOpen", "textDocument/hover", "unknown"};
    const int requests[4][9] = {
        {0, -32002, -32002, -32002, -32002, -32002, -32002, -32002, -32002},
        {-32600, -32600, 0, -32600, 0, 0, 0, 0, 0},
        {-32600, -32600, -32600, -32600, -32600, -32600, -32600, -32600, -32600},
        {1, 1, 1, 1, 1, 1, 1, 1, 1}
    };
    const int notifications[4][9] = {
        {1, 1, 1, 0, 1, 1, 1, 1, 1},
        {1, 0, 1, 0, 0, 0, 0, 0, 0},
        {1, 1, 1, 0, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1, 1, 1, 1}
    };
    unsigned count = 0;
    for (unsigned bits = 0; bits < 8; ++bits) {
        XrLspServer server = {0}; server.initialized = (bits & 1) != 0;
        server.shutdown_received = (bits & 2) != 0; server.exit_received = (bits & 4) != 0;
        unsigned phase = server.exit_received ? 3 : server.shutdown_received ? 2 : server.initialized ? 1 : 0;
        XrLspServer before = server; size_t attempts = lsp_json_calls;
        for (unsigned i = 0; i < 9; ++i) {
            CHECK((int)xlsp_lifecycle_admit(&server, methods[i], true) == requests[phase][i]);
            CHECK((int)xlsp_lifecycle_admit(&server, methods[i], false) == notifications[phase][i]);
            count += 2;
        }
        CHECK(!memcmp(&before, &server, sizeof(server)) && lsp_json_calls == attempts);
    }
    CHECK(xlsp_lifecycle_admit(NULL, "exit", true) == XLSP_LIFECYCLE_INVALID_REQUEST);
    CHECK(xlsp_lifecycle_admit(NULL, "exit", false) == XLSP_LIFECYCLE_DROP);
    XrLspServer server = {0};
    CHECK(xlsp_lifecycle_admit(&server, NULL, true) == XLSP_LIFECYCLE_INVALID_REQUEST);
    CHECK(xlsp_lifecycle_admit(&server, "", false) == XLSP_LIFECYCLE_DROP);
    return count + 4;
}

static XrLspServer initial(void) {
    XrLspServer server = {0}; server.initialized = true; server.next_request_id = 37;
    server.source_revision = 9; server.source_failure.stage = 8; server.source_failure.status = 9;
    return server;
}

static void unchanged_except_failure(const XrLspServer *before, const XrLspServer *after,
                                     unsigned stage, XrXirStatus status) {
    XrLspServer expected = *before; expected.source_failure.stage = stage;
    expected.source_failure.status = (unsigned)status;
    CHECK(!memcmp(&expected, after, sizeof(expected)));
}

static unsigned shutdown_cases(bool inject) {
    XrJsonValue *params[] = {NULL, xjson_new_null(), xjson_new_object(), xjson_new_array(),
        xjson_new_bool(true), xjson_new_number(1), xjson_new_string("bad")};
    for (unsigned i = 1; i < 7; ++i) CHECK(params[i]);
    unsigned cases = 0;
    for (unsigned bits = 0; bits < 8; ++bits) {
        for (unsigned p = 0; p < 7; ++p) {
            XrLspServer server = initial(); server.initialized = (bits & 1) != 0;
            server.shutdown_received = (bits & 2) != 0; server.exit_received = (bits & 4) != 0;
            XrLspServer before = server; size_t calls = lsp_json_calls;
            XrJsonValue *result = xlsp_handle_lc_shutdown(&server, params[p]);
            if (bits == 1 && p == 0) {
                CHECK(result && result->type == XR_JSON_NULL && lsp_json_calls == calls + 1);
                XrLspServer expected = before; expected.shutdown_received = true;
                expected.source_failure.stage = expected.source_failure.status = 0;
                CHECK(!memcmp(&expected, &server, sizeof(server)));
            } else {
                CHECK(!result && lsp_json_calls == calls);
                unchanged_except_failure(&before, &server, 18, bits == 1 ? XR_XIR_BAD_STRUCTURE : XR_XIR_BAD_STAGE);
            }
            xjson_free(result); ++cases;
        }
    }
    CHECK(!xlsp_handle_lc_shutdown(NULL, NULL));
    if (inject) {
        XrLspServer server = initial(), before = server; size_t live = lsp_json_live, bytes = lsp_json_bytes;
        lsp_json_injected = false; lsp_json_fail = lsp_json_calls;
        CHECK(!xlsp_handle_lc_shutdown(&server, NULL) && lsp_json_injected);
        unchanged_except_failure(&before, &server, 18, XR_XIR_OUT_OF_MEMORY);
        CHECK(lsp_json_live == live && lsp_json_bytes == bytes);
        lsp_json_fail = SIZE_MAX;
        XrJsonValue *retry = xlsp_handle_lc_shutdown(&server, NULL);
        CHECK(retry && retry->type == XR_JSON_NULL && server.shutdown_received && !server.exit_received);
        CHECK(!server.source_failure.stage && !server.source_failure.status); xjson_free(retry); cases += 2;
    }
    for (unsigned p = 1; p < 7; ++p) xjson_free(params[p]);
    return cases + 1;
}

static unsigned exit_cases(void) {
    XrJsonValue *params[] = {NULL, xjson_new_null(), xjson_new_object(), xjson_new_array(),
        xjson_new_bool(false), xjson_new_number(0), xjson_new_string("bad")};
    for (unsigned i = 1; i < 7; ++i) CHECK(params[i]);
    unsigned cases = 0;
    for (unsigned bits = 0; bits < 8; ++bits) {
        for (unsigned p = 0; p < 7; ++p) {
            XrLspServer server = initial(); server.initialized = (bits & 1) != 0;
            server.shutdown_received = (bits & 2) != 0; server.exit_received = (bits & 4) != 0;
            XrLspServer before = server, expected = before; size_t calls = lsp_json_calls;
            xlsp_handle_lc_exit(&server, params[p]); CHECK(lsp_json_calls == calls);
            if (p == 0) {
                expected.exit_received = true; expected.source_failure.stage = expected.source_failure.status = 0;
                CHECK(!memcmp(&expected, &server, sizeof(server)));
            } else unchanged_except_failure(&before, &server, 19, XR_XIR_BAD_STRUCTURE);
            ++cases;
        }
    }
    xlsp_handle_lc_exit(NULL, NULL);
    for (unsigned p = 1; p < 7; ++p) xjson_free(params[p]);
    return cases + 1;
}

int main(void) {
    bool full = getenv("XR_LSP_LIFECYCLE_NORMAL_ONLY") == NULL;
    unsigned admission = matrix(), shutdown = shutdown_cases(full), exiting = exit_cases();
    CHECK(!lsp_json_live && !lsp_json_bytes);
    printf("{\"admission\":%u,\"shutdown\":%u,\"exit\":%u,\"full\":%s,\"shutdown_physical_points\":1,\"live\":0,\"bytes\":0}\n",
           admission, shutdown, exiting, full ? "true" : "false");
    return 0;
}
