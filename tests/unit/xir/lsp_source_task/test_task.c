/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_source_task.h"
#include "app/lsp/xlsp_request_cancel.h"
#include "app/lsp/xlsp_pending_requests.h"
#include "app/lsp/xlsp_document_store.h"
#include "app/lsp/xlsp_source_completion.h"
#include "app/lsp/xlsp_navigation.h"
#include "app/lsp/xlsp_index_json.h"
#include "app/lsp/xlsp_async.h"
#include "api/xsource_compile.h"
#ifdef XR_OS_WINDOWS
#include "base/xwindows_utf8.h"
#endif
#include "os/os_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d:%s\n", __LINE__, #c); exit(1); } } while (0)
#define xr_compile_resources_new task_resource_new_impl
#define xr_compile_resources_work task_work_impl
#define xr_compile_resources_release task_resource_release_impl
#include "xir_sdk_resource_test.h"
#undef xr_compile_resources_work
#undef xr_compile_resources_new
#undef xr_compile_resources_release
#include "task_cancel.h"
#include "lsp_open_json_owner.h"
#pragma push_macro("xr_free")
#undef xr_free
#define xr_free(p) lsp_json_free(p)
#include "app/lsp/xlsp_protocol_output.c"
#pragma pop_macro("xr_free")
#include "task_sync.h"

typedef struct TaskBlock { void *pointer; size_t bytes; } TaskBlock;
static TaskBlock task_blocks[32];
static size_t task_attempts, task_fail = SIZE_MAX, task_live, task_bytes, admission_checks;
XR_FUNC void *task_test_malloc(size_t bytes) {
    if (task_attempts++ == task_fail) return NULL;
    void *pointer = sdk_fixture_malloc(bytes); if (!pointer) return NULL;
    for (size_t i = 0; i < 32; ++i) if (!task_blocks[i].pointer) {
        task_blocks[i] = (TaskBlock){pointer, bytes}; ++task_live; task_bytes += bytes; return pointer;
    }
    CHECK(false); return NULL;
}
XR_FUNC void *task_test_calloc(size_t count, size_t bytes) {
    CHECK(!count || bytes <= SIZE_MAX / count);
    void *pointer = task_test_malloc(count * bytes); if (pointer) memset(pointer, 0, count * bytes); return pointer;
}
XR_FUNC void task_test_free(void *pointer) {
    if (!pointer) return;
    for (size_t i = 0; i < 32; ++i) if (task_blocks[i].pointer == pointer) {
        CHECK(task_live && task_bytes >= task_blocks[i].bytes); --task_live; task_bytes -= task_blocks[i].bytes;
        task_blocks[i] = (TaskBlock){0}; sdk_fixture_free(pointer); return;
    }
    CHECK(false);
}

static const char root_text[] = "import { value } from \"./lib\"\nexport fn result()->i64 { /* 😀 */ return value() }\n";
static const char lib_text[] = "/* 😀 */ export fn value()->i64 { return 41 }\n";
typedef struct Gate { _Atomic bool entered, release; unsigned completed; } Gate;
static void block(void *data) {
    Gate *gate = data; atomic_store(&gate->entered, true); uint64_t deadline = xr_time_monotonic_ms() + 5000;
    while (!atomic_load(&gate->release)) { CHECK(xr_time_monotonic_ms() < deadline); xr_thread_sleep_ms(1); }
}
static void block_complete(void *data, void *result) { Gate *gate = data; (void)result; ++gate->completed; }
static void destroy_document(XrLspDocument *doc) {
    if (!doc) return;
    memset(doc->content, '?', doc->length); sdk_fixture_free(doc->content); sdk_fixture_free(doc->uri); sdk_fixture_free(doc);
}
static void add_document(XrLspServer *server, const char *uri, const char *text, int version) {
    XrLspDocument *doc = xr_calloc(1, sizeof(*doc)); CHECK(doc);
    doc->uri = xr_strdup(uri); doc->content = xr_strdup(text); CHECK(doc->uri && doc->content);
    doc->length = strlen(text); doc->version = version; doc->server = server;
    XlspDocumentInsertion insertion = {0};
    CHECK(xlsp_document_store_prepare(server->doc_table, doc->uri, doc, &insertion));
    CHECK(xlsp_document_store_commit(&insertion));
}
static XrJsonValue *parameters(const char *uri) {
    const char coordinates[] = "{\"line\":1,\"character\":43}";
    XrJsonValue *params = xjson_new_object(), *document = xjson_new_object();
    XrJsonValue *position = xjson_parse(coordinates, sizeof(coordinates) - 1), *context = xjson_new_object();
    CHECK(params && document && position && context);
    CHECK(index_json_member(document, "uri", xjson_new_string(uri)));
    CHECK(index_json_member(context, "includeDeclaration", xjson_new_bool(false)));
    CHECK(index_json_member(params, "textDocument", document)); CHECK(index_json_member(params, "position", position));
    CHECK(index_json_member(params, "context", context)); return params;
}
static XlspRequestId identity(void) {
    const char text[] = "{\"id\":\"source\\u0000λ\"}"; XlspRequestId id = {0};
    XrJsonValue *value = xjson_parse(text, sizeof(text) - 1); CHECK(value);
    CHECK(xlsp_request_id_parse(value, &id) == XLSP_PROTOCOL_OK); xjson_free(value); return id;
}
static void rejected(XrLspServer *server, const XlspRequestId *id, XrJsonValue *params,
                     unsigned mode, XrXirStatus expected) {
    size_t blocks = task_live, bytes = task_bytes, json = lsp_json_live, json_bytes = lsp_json_bytes;
    XlspPendingRequests before = server->pending_requests;
    CHECK(xlsp_source_task_submit(server, id, params, mode) == expected);
    CHECK(!memcmp(&before, &server->pending_requests, sizeof(before)));
    CHECK(task_live == blocks && task_bytes == bytes && !runtime_live && !runtime_bytes);
    CHECK(lsp_json_live == json && lsp_json_bytes == json_bytes); ++admission_checks;
}
static void check_admission(XrLspServer *server, const XlspRequestId *id, XrJsonValue *params) {
    XlspRequestId invalid = {0}, missing = {.kind = XLSP_ID_NUMBER, .as.number = 42};
    rejected(server, &invalid, params, 0, XR_XIR_BAD_STRUCTURE);
    rejected(server, id, params, 22, XR_XIR_BAD_STRUCTURE);
    rejected(server, &missing, params, 0, XR_XIR_BAD_STAGE);
    server->initialized = false; rejected(server, id, params, 0, XR_XIR_BAD_STAGE); server->initialized = true;
    server->shutdown_received = true; rejected(server, id, params, 0, XR_XIR_BAD_STAGE); server->shutdown_received = false;
    server->exit_received = true; rejected(server, id, params, 0, XR_XIR_BAD_STAGE); server->exit_received = false;
    XrLspAsync *async = server->async; server->async = NULL;
    rejected(server, id, params, 0, XR_XIR_BAD_STAGE); server->async = async;
    uint64_t revision = server->source_revision; server->source_revision = UINT64_MAX;
    rejected(server, id, params, 0, XR_XIR_BUDGET); server->source_revision = revision;
    XlspPendingRequest *pending = xlsp_pending_request_find(&server->pending_requests, id); CHECK(pending);
    pending->source_task_id = 99; rejected(server, id, params, 0, XR_XIR_BAD_STAGE); pending->source_task_id = 0;
    pending->cancelled = true; rejected(server, id, params, 0, XR_XIR_BAD_STAGE); pending->cancelled = false;
    rejected(server, id, NULL, 0, XR_XIR_BAD_STRUCTURE);
    for (size_t i = 0; i < 3; ++i) {
        task_fail = task_attempts + i; rejected(server, id, params, 0, XR_XIR_OUT_OF_MEMORY);
        CHECK(task_attempts == task_fail + 1); task_fail = SIZE_MAX;
    }
    uint64_t next = atomic_load(&async->next_task_id);
    atomic_store(&async->next_task_id, 0); rejected(server, id, params, 0, XR_XIR_BAD_STAGE);
    CHECK(!atomic_load(&async->next_task_id));
    atomic_store(&async->next_task_id, UINT64_MAX); rejected(server, id, params, 0, XR_XIR_BAD_STAGE);
    CHECK(atomic_load(&async->next_task_id) == UINT64_MAX); atomic_store(&async->next_task_id, next);
    size_t prefixes = runtime_attempts;
    XlspSourceRequest *captured = NULL;
    CHECK(xlsp_navigation_capture(server, params, 0, &captured) == XR_XIR_OK);
    xlsp_source_request_free(captured); prefixes = runtime_attempts - prefixes; CHECK(prefixes);
    for (size_t i = 0; i < prefixes; ++i) {
        runtime_fail_at = runtime_attempts + i;
        rejected(server, id, params, 0, XR_XIR_OUT_OF_MEMORY);
        CHECK(runtime_attempts > runtime_fail_at); runtime_fail_at = SIZE_MAX;
    }
}
static int run(int argc, char **argv) {
    CHECK(argc == 4); unsigned mode = (unsigned)strtoul(argv[3], NULL, 10); CHECK(mode < 16);
    (void)sdk_ledger; (void)sdk_unlimited; (void)sdk_stats;
    XrLspServer server = {0}; server.initialized = true; server.source_revision = 5; server.next_request_id = 71;
    server.transport = xlsp_transport_stdio(); CHECK(server.transport); XrLspTransport *transport = server.transport;
    XrLspAsync *async = xlsp_async_new(NULL, NULL); CHECK(async); server.async = async;
    Gate gate = {0}; XrLspTask *barrier = xlsp_task_new(block, block_complete, &gate, XLSP_TASK_NORMAL); CHECK(barrier);
    CHECK(xlsp_async_submit(async, barrier)); uint64_t deadline = xr_time_monotonic_ms() + 5000;
    while (!atomic_load(&gate.entered)) { CHECK(xr_time_monotonic_ms() < deadline); xr_thread_sleep_ms(1); }
    server.doc_table = xlsp_document_store_new(destroy_document); CHECK(server.doc_table);
    add_document(&server, argv[1], root_text, 1); add_document(&server, argv[2], lib_text, 7);
    XlspRequestId id = identity(), owned = {0}; CHECK(xlsp_request_id_clone(&id, &owned) == XLSP_PROTOCOL_OK);
    CHECK(xlsp_pending_request_add(&server.pending_requests, owned, "textDocument/query", 99));
    XrJsonValue *params = parameters(argv[1]); unsigned query = mode < 4 ? mode : mode >= 12 ? mode - 12 : 0;
    if (mode == 0) {check_admission(&server, &id, params);check_sync_navigation(&server, params, argv[2]);}
    XrXirStatus status = xlsp_source_task_submit(&server, &id, params, query);
    if (status != XR_XIR_OK) fprintf(stderr, "admission status=%u stage=%u source_status=%u\n",
        (unsigned)status, server.source_failure.stage, server.source_failure.status);
    CHECK(status == XR_XIR_OK); xjson_free(params);
    CHECK(runtime_live && runtime_bytes);
    XlspPendingRequest *pending = xlsp_pending_request_find(&server.pending_requests, &id);
    CHECK(pending && pending->source_task_id && pending->source_task_id != barrier->task_id);
    size_t captured = runtime_live;
    xlsp_document_store_free(server.doc_table); server.doc_table = NULL;
    if (mode == 4) CHECK(xlsp_request_cancel(&server, &id));
    if (mode == 5) server.shutdown_received = true;
    if (mode == 6 || mode == 7) server.exit_received = true;
    if (mode == 8) ++server.source_revision;
    if (mode == 9) {
        CHECK(xlsp_pending_request_remove(&server.pending_requests, &id)); owned = (XlspRequestId){0};
        CHECK(xlsp_request_id_clone(&id, &owned) == XLSP_PROTOCOL_OK);
        CHECK(xlsp_pending_request_add(&server.pending_requests, owned, "new/request", 100));
        pending = xlsp_pending_request_find(&server.pending_requests, &id); CHECK(pending); pending->source_task_id = UINT64_MAX - 1;
    }
    if (mode == 10) server.transport = NULL;
    if (mode == 11) CHECK(xlsp_request_cancel(&server, &id));
    if (mode >= 12) {
        task_cancel_arm(); atomic_store(&gate.release, true);
        task_cancel_wait(); CHECK(xlsp_request_cancel(&server, &id)); task_cancel_release();
    }
    if (mode == 7 || mode == 11) {
        atomic_store(&gate.release, true); xlsp_async_free(async); server.async = NULL;
    } else {
        atomic_store(&gate.release, true); deadline = xr_time_monotonic_ms() + 10000; unsigned processed = 0;
        while (processed < 2) {
            processed += (unsigned)xlsp_async_poll(async);
            CHECK(xr_time_monotonic_ms() < deadline); xr_thread_sleep_ms(1);
        }
        xlsp_async_free(async); server.async = NULL;
    }
    CHECK(gate.completed == 1 && !runtime_live && !runtime_bytes && !lsp_json_live && !lsp_json_bytes);
    CHECK(server.next_request_id == 71);
    if (mode == 9) {
        pending = xlsp_pending_request_find(&server.pending_requests, &id);
        CHECK(pending && pending->source_task_id == UINT64_MAX - 1 && !pending->cancelled);
        CHECK(xlsp_pending_request_remove(&server.pending_requests, &id));
    }
    CHECK(!server.pending_requests.count); xlsp_request_id_free(&id); xlsp_transport_free(transport);
    CHECK(!task_live && !task_bytes);
    fprintf(stderr, "STATS mode=%u captured=%zu compiler_live=%zu compiler_bytes=%zu json_live=%zu json_bytes=%zu exit=%u app_live=%zu app_bytes=%zu admission=%zu\n",
        mode, captured, runtime_live, runtime_bytes, lsp_json_live, lsp_json_bytes, server.exit_received ? 1u : 0u,
        task_live, task_bytes, admission_checks);
    if (mode >= 12) return task_cancel_check(mode);
    return 0;
}
#ifdef XLSP_HIERARCHY_TASK
#include "../lsp_hierarchy_task/task_cases.h"
#endif
#ifdef XLSP_SEMANTIC_TASK
#include "../lsp_semantic_task/task_cases.h"
#endif
#ifdef XLSP_LINKS_TASK
#include "../lsp_links_task/task_cases.h"
#endif
#ifdef XLSP_NAVIGATION_ADMISSION
#include "../lsp_navigation_admission/task_cases.h"
#endif
#ifdef XLSP_SIGNATURE_TASK
#include "../lsp_signature_task/task_cases.h"
#endif
#ifdef XLSP_PARAMETER_TASK
#include "../lsp_parameter_task/task_cases.h"
#endif
#ifdef XLSP_FOLDING_TASK
#include "../lsp_folding_task/task_cases.h"
#endif
#ifdef XLSP_EDIT_TASK
#include "../lsp_edit_task/task_cases.h"
#endif
#ifdef XLSP_SYNTAX_TASK
#include "../lsp_syntax_task/task_cases.h"
#endif
static int dispatch_run(int argc, char **argv) {
#ifdef XLSP_EDIT_TASK
    if(argc>=3&&!strcmp(argv[2],"--edit"))return edit_main(argc,argv);
#endif
#ifdef XLSP_SYNTAX_TASK
    if(argc>=3&&!strcmp(argv[2],"--syntax"))return syntax_main(argc,argv);
#endif
#ifdef XLSP_FOLDING_TASK
    if(argc>=3&&!strcmp(argv[2],"--folding"))return folding_main(argc,argv);
#endif
#ifdef XLSP_PARAMETER_TASK
    if(argc>=3&&!strcmp(argv[2],"--parameter"))return parameter_main(argc,argv);
#endif
#ifdef XLSP_SIGNATURE_TASK
    if(argc>=3&&!strcmp(argv[2],"--signature"))return signature_main(argc,argv);
#endif
#ifdef XLSP_NAVIGATION_ADMISSION
    if(argc>=3&&!strcmp(argv[2],"--nav-admission"))return navigation_admission_main(argc,argv);
#endif
#ifdef XLSP_LINKS_TASK
    if(argc>=3&&!strcmp(argv[2],"--links"))return links_main(argc,argv);
#endif
#ifdef XLSP_SEMANTIC_TASK
    if(argc>=3&&!strcmp(argv[2],"--semantic"))return semantic_main(argc,argv);
#endif
#ifdef XLSP_HIERARCHY_TASK
    if (argc == 6) return hierarchy_run(argc, argv);
#endif
    return run(argc, argv);
}
#ifdef XR_OS_WINDOWS
int wmain(int argc, wchar_t **wide) {
    XrWinPathStatus status; char **argv = xr_win_utf16_arguments(argc, wide, &status); CHECK(argv);
    int result = dispatch_run(argc, argv); xr_win_utf8_arguments_free(argc, argv); return result;
}
#else
int main(int argc, char **argv) {return dispatch_run(argc, argv);}
#endif
