/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#define wmain document_existing_wmain
#include "../lsp_document_owner/test_document.c"
#undef wmain
#include "app/lsp/xlsp_document_notification.h"
#include "app/lsp/xlsp_workspace.h"
static uint64_t queue_clock = 1000;
static unsigned queue_publications, queue_index_calls;
XR_FUNC uint64_t diagnostic_queue_now(void) { return queue_clock; }
XR_FUNC void xlsp_publish_diagnostics(XrLspServer *server, XrLspDocument *doc) {
    CHECK(server && doc && doc->server == server); ++queue_publications;
}
XR_FUNC void xlsp_workspace_index_file(XrLspServer *server, const char *uri, const char *path) {
    CHECK(server && uri && !path); ++queue_index_calls;
}
#include "notification_handlers.inc.c"
#pragma push_macro("xr_realloc")
#undef xr_realloc
#define xr_realloc document_test_realloc
#define xr_time_monotonic_ms diagnostic_queue_now
#include "before_schedule.inc.c"
#undef xr_time_monotonic_ms
#pragma pop_macro("xr_realloc")

static void queue_zero(void) {
    CHECK(!runtime_live && !runtime_bytes && !lsp_json_live && !lsp_json_bytes && !app_live && !app_bytes);
}
static void queue_init(XrLspServer *server, XrLspDocument *docs, size_t count) {
    memset(server, 0, sizeof(*server)); server->config.diagnostics_enabled = true;
    memset(docs, 0, count * sizeof(*docs));
    for (size_t i = 0; i < count; ++i) { docs[i].server = server; docs[i].last_change_time = 17; docs[i].diagnostic_deadline = 23; }
    queue_clock = 1000;
}
static void queue_failed_state(XrLspServer *server, const XrLspServer *before,
    XrLspDocument *doc, const XrLspDocument *saved) {
    XrLspServer expected = *before; expected.exit_received = true;
    CHECK(!memcmp(server, &expected, sizeof(*server)) && !memcmp(doc, saved, sizeof(*doc)));
}
static size_t queue_growth(size_t fault) {
    XrLspServer server; XrLspDocument docs[65]; queue_init(&server, docs, 65);
    size_t begin = app_attempts; app_fail = fault == SIZE_MAX ? SIZE_MAX : begin + fault;
    size_t stopped = 65;
    for (size_t i = 0; i < 65; ++i) {
        XrLspServer before = server; XrLspDocument saved = docs[i];
        xlsp_schedule_diagnostics(&server, &docs[i]);
        if (server.exit_received) { queue_failed_state(&server, &before, &docs[i], &saved); stopped = i; break; }
        CHECK(server.pending_diag_count == (int)i + 1 && docs[i].diag_pending && docs[i].diagnostic_deadline == 1300);
        CHECK(server.pending_diag[i] == &docs[i]);
    }
    size_t sites = app_attempts - begin;
    if (fault == SIZE_MAX) CHECK(stopped == 65 && sites == 4 && server.pending_diag_capacity == 128);
    else {
        const size_t stopped_at[] = {0, 16, 32, 64}; CHECK(fault < 4 && stopped == stopped_at[fault] && app_attempts > app_fail);
        for (size_t i = 0; i < stopped; ++i) CHECK(server.pending_diag[i] == &docs[i] && docs[i].diag_pending);
        app_fail = SIZE_MAX; server.exit_received = false;
        for (size_t i = stopped; i < 65; ++i) xlsp_schedule_diagnostics(&server, &docs[i]);
        CHECK(!server.exit_received && server.pending_diag_count == 65 && server.pending_diag_capacity == 128);
    }
    app_fail = SIZE_MAX; begin = app_attempts; queue_clock = 2000; server.config.diagnostic_debounce_ms = 7;
    for (size_t i = 0; i < 65; ++i) {
        xlsp_schedule_diagnostics(&server, &docs[i]);
        CHECK(server.pending_diag[i] == &docs[i] && docs[i].diag_pending && docs[i].last_change_time == 2000 && docs[i].diagnostic_deadline == 2007);
    }
    CHECK(app_attempts == begin && server.pending_diag_count == 65);
    document_test_free(server.pending_diag); queue_zero(); return sites;
}
static unsigned queue_invalid(void) {
    for (unsigned mode = 0; mode < 10; ++mode) {
        XrLspServer server, other = {0}; XrLspDocument doc; queue_init(&server, &doc, 1); XrLspDocument *slot = &doc;
        switch (mode) {
        case 0: server.pending_diag_count = -1; break;
        case 1: server.pending_diag_capacity = -1; break;
        case 2: server.pending_diag_count = 1; break;
        case 3: server.pending_diag_capacity = 1; break;
        case 4: server.pending_diag = &slot; break;
        case 5: doc.diag_pending = true; break;
        case 6: doc.server = &other; break;
        case 7: server.pending_diag = &slot; server.pending_diag_count = server.pending_diag_capacity = INT_MAX; break;
        case 8: queue_clock = UINT64_MAX - 1; server.config.diagnostic_debounce_ms = 2; break;
        case 9: queue_clock = UINT64_MAX; break;
        }
        XrLspServer before = server; XrLspDocument saved = doc; size_t attempts = app_attempts;
        xlsp_schedule_diagnostics(&server, &doc); queue_failed_state(&server, &before, &doc, &saved); CHECK(app_attempts == attempts);
    }
    XrLspServer server; XrLspDocument doc; queue_init(&server, &doc, 1);
    server.config.diagnostics_enabled = false; XrLspServer before = server; XrLspDocument saved = doc;
    xlsp_schedule_diagnostics(&server, &doc); CHECK(!memcmp(&before, &server, sizeof(server)) && !memcmp(&saved, &doc, sizeof(doc)));
    server.config.diagnostics_enabled = true; server.exit_received = true; before = server;
    xlsp_schedule_diagnostics(&server, &doc); CHECK(!memcmp(&before, &server, sizeof(server)) && !memcmp(&saved, &doc, sizeof(doc)));
    xlsp_schedule_diagnostics(NULL, &doc); server.exit_received = false; xlsp_schedule_diagnostics(&server, NULL); CHECK(server.exit_received);
    queue_zero(); return 10;
}
static XrJsonValue *queue_change(const char *uri) {
    const char *format = "{\"textDocument\":{\"uri\":\"%s\",\"version\":2},\"contentChanges\":[{\"text\":\"export fn main()->i64 { return 2 }\\n\"}]}";
    size_t size = strlen(uri) + strlen(format) + 1; char *text = document_test_malloc(size); CHECK(text);
    int length = snprintf(text, size, format, uri); CHECK(length > 0 && (size_t)length < size);
    XrJsonValue *value = xjson_parse(text, (size_t)length); document_test_free(text); CHECK(value); return value;
}
static void queue_source_failure(const char *uri) {
    XrLspServer server; XrLspDocument unused; queue_init(&server, &unused, 1);
    server.doc_table = xlsp_document_store_new(xlsp_document_destroy); CHECK(server.doc_table);
    XrLspDocument *doc = open_owned(&server, uri, "export fn main()->i64 { return 1 }\n", 1);
    XrJsonValue *input = queue_change(uri); app_fail = app_attempts;
    xlsp_handle_td_did_change(&server, input); CHECK(app_attempts > app_fail); app_fail = SIZE_MAX; xjson_free(input);
    CHECK(server.exit_received && doc->version == 2 && strstr(doc->content, "return 2") &&
        doc->source_open && doc->source_buffer && doc->syntax_snapshot && server.source_revision == 2);
    CHECK(!server.pending_diag && !server.pending_diag_count && !server.pending_diag_capacity && !doc->diag_pending &&
        !doc->last_change_time && !doc->diagnostic_deadline && !queue_publications && !queue_index_calls);
    xlsp_document_close(&server, uri); CHECK(!xlsp_document_get(&server, uri) && !server.source_workspace);
    xlsp_document_store_free(server.doc_table); queue_zero();
}
static int queue_before(bool deadline) {
    XrLspServer server; XrLspDocument doc; queue_init(&server, &doc, 1);
    if (deadline) { queue_clock = UINT64_MAX - 1; server.config.diagnostic_debounce_ms = 2; }
    else app_fail = app_attempts;
    before_xlsp_schedule_diagnostics(&server, &doc); app_fail = SIZE_MAX;
    if (deadline) CHECK(!server.exit_received && doc.diag_pending && doc.diagnostic_deadline == 0 && server.pending_diag_count == 1);
    else CHECK(!server.exit_received && !doc.diag_pending && !server.pending_diag_count && doc.diagnostic_deadline == 1300);
    document_test_free(server.pending_diag); queue_zero();
    fputs(deadline ? "QUEUE_BEFORE deadline_wrapped_to_zero=1 compiler0/0 JSON0/0 app0/0\n" :
        "QUEUE_BEFORE growth_OOM_lost_work=1 compiler0/0 JSON0/0 app0/0\n", stderr); return 1;
}
static int queue_run(int argc, char **argv) {
    CHECK(argc == 3);
    if (!strcmp(argv[2], "--before-oom")) return queue_before(false);
    if (!strcmp(argv[2], "--before-deadline")) return queue_before(true);
    bool resources = !strcmp(argv[2], "--resources"); CHECK(resources || !strcmp(argv[2], "--normal"));
    size_t sites = queue_growth(SIZE_MAX); queue_growth(0); queue_growth(1);
    if (resources) { queue_growth(2); queue_growth(3); }
    unsigned invalid = queue_invalid(); queue_clock = 1000; queue_source_failure(argv[1]);
    printf("Diagnostic queue sites=%zu full_FI=%u invalid=%u deduplicated=65 actual_changed_Source_OOM_exit=1 owned_cleanup=1 compiler0/0 JSON0/0 app0/0\n",
        sites, resources ? 1u : 0u, invalid); return 0;
}
#ifdef XR_OS_WINDOWS
int wmain(int argc, wchar_t **wide) {
    XrWinPathStatus status; char **argv = xr_win_utf16_arguments(argc, wide, &status); CHECK(argv);
    int result = queue_run(argc, argv); xr_win_utf8_arguments_free(argc, argv); return result;
}
#else
int main(int argc, char **argv) { return queue_run(argc, argv); }
#endif
