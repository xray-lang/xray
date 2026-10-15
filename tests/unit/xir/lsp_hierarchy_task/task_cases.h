/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef LSP_HIERARCHY_TASK_CASES_H
#define LSP_HIERARCHY_TASK_CASES_H
#include "app/lsp/xlsp_call_hierarchy.h"
#include "app/lsp/xlsp_hierarchy_request.h"
static const char hierarchy_text[] =
    "fn target(v:i64)->i64{return v}\n"
    "fn wrapper(v:i64)->i64{return target(v)+target(v)}\n"
    "const answer=wrapper(1)\n"
    "interface Base {}\n"
    "interface Child extends Base {}\n";
static XrJsonValue *hierarchy_prepare_params(const char *uri, unsigned line, unsigned column) {
    XrJsonValue *params = parameters(uri), *at = xjson_get_object(params, "position");
    XrJsonValue *l = xjson_get(at, "line"), *c = xjson_get(at, "character");
    CHECK(l && c); l->as.integer = line; c->as.integer = column; return params;
}
static XrJsonValue *hierarchy_task_params(XrLspServer *server, const char *uri, unsigned method) {
    unsigned line = method < 7 ? method == 6 ? 1u : 0u : method == 9 ? 3u : 4u;
    unsigned column = method < 7 ? 3u : 10u;
    XrJsonValue *params = hierarchy_prepare_params(uri, line, column);
    if (method == 4 || method == 7) return params;
    XrJsonValue *prepared = method < 7 ? xlsp_handle_prepare_call_hierarchy(server, params) :
        xlsp_handle_prepare_type_hierarchy(server, params);
    CHECK(prepared && prepared->type == XR_JSON_ARRAY && prepared->as.array.count == 1);
    xjson_free(params); params = xjson_new_object(); CHECK(params);
    CHECK(index_json_member(params, "item", xjson_clone(xjson_array_get(prepared, 0))));
    xjson_free(prepared); CHECK(!runtime_live && !runtime_bytes); return params;
}
static void hierarchy_producers_free(XrLspServer *server) {
    xlsp_document_store_free(server->doc_table); server->doc_table = NULL;
    for (int i = 0; i < server->workspace_folder_count; ++i) {
        char *path = server->workspace_folders[i].path;
        memset(path, '?', strlen(path)); sdk_fixture_free(path); server->workspace_folders[i].path = NULL;
    }
    server->workspace_folder_count = 0;
    for (int i = 0; i < server->config.ignore_pattern_count; ++i) {
        char *pattern = server->config.ignore_patterns[i].pattern;
        memset(pattern, '?', strlen(pattern)); sdk_fixture_free(pattern);
    }
    sdk_fixture_free(server->config.ignore_patterns);
    if (server->config.log_path) { server->config.log_path[0] = '?'; sdk_fixture_free(server->config.log_path); }
    memset(&server->config, 0, sizeof(server->config));
}
static void hierarchy_configure(XrLspServer *server, const char *root) {
    server->workspace_folder_count = 1; server->workspace_folders[0].path = xr_strdup(root);
    server->config.ignore_patterns = xr_calloc(2, sizeof(*server->config.ignore_patterns));
    CHECK(server->workspace_folders[0].path && server->config.ignore_patterns);
    server->config.ignore_patterns[0].pattern = xr_strdup("ignored*.xr");
    server->config.ignore_patterns[0].is_glob = true;
    server->config.ignore_patterns[1].pattern = xr_strdup("skip");
    server->config.ignore_patterns[1].is_dir_only = true;
    server->config.ignore_pattern_count = 2;
    // A borrowed configuration need not expose its producer's spare capacity.
    server->config.ignore_pattern_capacity = 0;
    server->config.log_path = xr_strdup("log-path");
    CHECK(server->config.ignore_patterns[0].pattern && server->config.ignore_patterns[1].pattern && server->config.log_path);
    server->config.format_tab_size = 4; server->config.inlay_hints_type_annotations = true;
}
static void hierarchy_capture_faults(XrLspServer *server, const XlspRequestId *id, XrJsonValue *params, unsigned method) {
    XlspHierarchyRequest *request = NULL; size_t begin = runtime_attempts;
    CHECK(xlsp_hierarchy_request_capture(server, params, method < 7 ? method - 4 : method - 7,
        method >= 7, &request) == XR_XIR_OK);
    xlsp_hierarchy_request_free(request); CHECK(!runtime_live && !runtime_bytes);
    size_t sites = runtime_attempts - begin; CHECK(sites);
    for (size_t i = 0; i < sites; ++i) {
        runtime_fail_at = runtime_attempts + i;
        rejected(server, id, params, method, XR_XIR_OUT_OF_MEMORY);
        CHECK(runtime_attempts == runtime_fail_at + 1);
        runtime_fail_at = SIZE_MAX;
    }
    for (size_t i = 0; i < 3; ++i) {
        task_fail = task_attempts + i; rejected(server, id, params, method, XR_XIR_OUT_OF_MEMORY);
        CHECK(task_attempts == task_fail + 1); task_fail = SIZE_MAX;
    }
    fprintf(stderr, "HIERARCHY_CAPTURE method=%u prefixes=%zu app_prefixes=3 physical0/0\n", method, sites);
}
static int hierarchy_run(int argc, char **argv) {
    CHECK(argc == 6); unsigned scenario = (unsigned)strtoul(argv[4], NULL, 10);
    unsigned method = (unsigned)strtoul(argv[5], NULL, 10); CHECK(scenario <= 11 && method >= 4 && method <= 9);
    XrLspServer server = {0}; server.initialized = true; server.source_revision = 41; server.next_request_id = 71;
    server.transport = xlsp_transport_stdio(); CHECK(server.transport); XrLspTransport *transport = server.transport;
    XrLspAsync *async = xlsp_async_new(NULL, NULL); CHECK(async); server.async = async;
    Gate gate = {0}; XrLspTask *barrier = xlsp_task_new(block, block_complete, &gate, XLSP_TASK_NORMAL); CHECK(barrier);
    CHECK(xlsp_async_submit(async, barrier)); uint64_t deadline = xr_time_monotonic_ms() + 5000;
    while (!atomic_load(&gate.entered)) { CHECK(xr_time_monotonic_ms() < deadline); xr_thread_sleep_ms(1); }
    server.doc_table = xlsp_document_store_new(destroy_document); CHECK(server.doc_table);
    add_document(&server, argv[1], hierarchy_text, 7); hierarchy_configure(&server, argv[3]);
    XrJsonValue *params = hierarchy_task_params(&server, argv[1], method);
    // Closed-root capture retains the same unknown-version state for round trips.
    if (scenario == 3) {
        xjson_free(params); xlsp_document_store_free(server.doc_table); server.doc_table = NULL;
        params = hierarchy_task_params(&server, argv[1], method);
    }
    if (scenario == 8) {
        XrJsonValue *item = xjson_get_object(params, "item"), *data = xjson_get_object(item, "data");
        if (data) xjson_object_set(data, "revision", xjson_new_string("0"));
        else {
            XrJsonValue *document = xjson_get_object(params, "textDocument");
            xjson_object_set_new(document, "uri", xjson_new_string(argv[1]));
        }
    }
    XlspRequestId id = identity(), owned = {0}; CHECK(xlsp_request_id_clone(&id, &owned) == XLSP_PROTOCOL_OK);
    CHECK(xlsp_pending_request_add(&server.pending_requests, owned, "hierarchy/query", 99));
    if (scenario == 10) hierarchy_capture_faults(&server, &id, params, method);
    XrXirStatus status = xlsp_source_task_submit(&server, &id, params, method);
    if (scenario == 11) {
    CHECK(status == XR_XIR_BAD_STRUCTURE);
    xjson_free(params); hierarchy_producers_free(&server); atomic_store(&gate.release, true);
    xlsp_async_free(async); CHECK(xlsp_pending_request_remove(&server.pending_requests, &id));
    xlsp_request_id_free(&id); xlsp_transport_free(transport);
    CHECK(!runtime_live && !runtime_bytes && !lsp_json_live && !lsp_json_bytes && !task_live && !task_bytes);
    fprintf(stderr, "BEFORE actual SourceTask rejected hierarchy mode=%u status=%u physical0/0 JSON0/0 app0/0\n", method, (unsigned)status);
    return 1;
    }
    CHECK(status == XR_XIR_OK); xjson_free(params); CHECK(runtime_live && runtime_bytes);
    XlspPendingRequest *pending = xlsp_pending_request_find(&server.pending_requests, &id);
    CHECK(pending && pending->source_task_id && pending->source_task_id != barrier->task_id);
    size_t captured = runtime_live; hierarchy_producers_free(&server);
    if (scenario == 1) CHECK(xlsp_request_cancel(&server, &id));
    if (scenario == 4) ++server.source_revision;
    if (scenario == 5) server.exit_received = true;
    if (scenario == 6) server.transport = NULL;
    if (scenario == 7) server.shutdown_received = true;
    if (scenario == 9) {
        CHECK(xlsp_pending_request_remove(&server.pending_requests, &id)); owned = (XlspRequestId){0};
        CHECK(xlsp_request_id_clone(&id, &owned) == XLSP_PROTOCOL_OK);
        CHECK(xlsp_pending_request_add(&server.pending_requests, owned, "replacement", 100));
        pending = xlsp_pending_request_find(&server.pending_requests, &id); CHECK(pending);
        pending->source_task_id = UINT64_MAX - 1;
    }
    if (scenario == 2) {
        task_cancel_arm(); atomic_store(&gate.release, true); task_cancel_wait();
        CHECK(xlsp_request_cancel(&server, &id)); task_cancel_release();
    } else atomic_store(&gate.release, true);
    deadline = xr_time_monotonic_ms() + 10000; unsigned processed = 0;
    while (processed < 2) {
        processed += (unsigned)xlsp_async_poll(async);
        CHECK(xr_time_monotonic_ms() < deadline); xr_thread_sleep_ms(1);
    }
    xlsp_async_free(async); server.async = NULL;
    if (scenario == 9) {
        pending = xlsp_pending_request_find(&server.pending_requests, &id);
        CHECK(pending && pending->source_task_id == UINT64_MAX - 1);
        CHECK(xlsp_pending_request_remove(&server.pending_requests, &id));
    }
    CHECK(gate.completed == 1 && !server.pending_requests.count && server.next_request_id == 71);
    xlsp_request_id_free(&id); xlsp_transport_free(transport);
    CHECK(!runtime_live && !runtime_bytes && !lsp_json_live && !lsp_json_bytes && !task_live && !task_bytes);
    fprintf(stderr, "HIERARCHY method=%u scenario=%u captured=%zu physical0/0 JSON0/0 app0/0 producers_dead=1 exit=%u\n",
        method, scenario, captured, server.exit_received ? 1u : 0u);
    if (scenario == 2) return task_cancel_check(method);
    return 0;
}
#endif // LSP_HIERARCHY_TASK_CASES_H
