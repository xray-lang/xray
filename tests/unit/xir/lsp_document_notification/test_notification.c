/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#define wmain document_existing_wmain
#include "../lsp_document_owner/test_document.c"
#undef wmain
#include "app/lsp/xlsp_document_notification.h"
#include "app/lsp/xlsp_workspace.h"
#include "cases.h"
static unsigned notification_published, notification_indexed;
XR_FUNC void xlsp_publish_diagnostics(XrLspServer *server, XrLspDocument *doc) {
    CHECK(server && doc && doc->server == server); ++notification_published;
}
XR_FUNC void xlsp_workspace_index_file(XrLspServer *server, const char *uri, const char *path) {
    CHECK(server && uri && !path); ++notification_indexed;
}
#include "notification_handlers.inc.c"
#include "before_handlers.inc.c"

static const char notification_text[] = "export fn main()->i64 { return 1 }\n";
static const char open_template[] = "{\"textDocument\":{\"uri\":\"@U@\",\"languageId\":\"xray\",\"version\":1,\"text\":\"export fn main()->i64 { return 1 }\\n\"}}";
static const char change_template[] = "{\"textDocument\":{\"uri\":\"@U@\",\"version\":2},\"contentChanges\":[{\"text\":\"export fn main()->i64 { return 2 }\\n\"}]}";
static XrJsonValue *notification_input(const char *pattern, const char *uri) {
    size_t count = 0, length = strlen(pattern), uri_length = strlen(uri);
    for (const char *at = pattern; (at = strstr(at, "@U@")); at += 3) ++count;
    CHECK(uri_length <= SIZE_MAX - length && count <= (SIZE_MAX - length - 1) / (uri_length + 1));
    size_t capacity = length + count * (uri_length + 1) + 1;
    char *text = document_test_malloc(capacity); CHECK(text); size_t used = 0;
    for (const char *at = pattern; *at;) {
        if (!strncmp(at, "@U@", 3)) { memcpy(text + used, uri, uri_length); used += uri_length; at += 3; }
        else text[used++] = *at++;
    }
    CHECK(used < capacity); text[used] = 0;
    XrJsonValue *value = xjson_parse(text, used); memset(text, '?', used); document_test_free(text); CHECK(value); return value;
}
static void notification_call(XrLspServer *server, unsigned kind, XrJsonValue *input) {
    if (kind == 1) xlsp_handle_td_did_open(server, input);
    else if (kind == 2) xlsp_handle_td_did_change(server, input);
    else { CHECK(kind == 3); xlsp_handle_td_did_close(server, input); }
}
static void notification_dispose(XrJsonValue *input) {
    XrJsonValue *document = xjson_get(input, "textDocument");
    XrJsonValue *values[] = {xjson_get(document, "uri"), xjson_get(document, "text")};
    for (size_t i = 0; i < 2; ++i) if (values[i] && values[i]->type == XR_JSON_STRING)
        memset(values[i]->as.string, '?', values[i]->string_len);
    XrJsonValue *changes = xjson_get(input, "contentChanges");
    if (changes && changes->type == XR_JSON_ARRAY) for (int i = 0; i < changes->as.array.count; ++i) {
        XrJsonValue *text = xjson_get(changes->as.array.items[i], "text");
        if (text && text->type == XR_JSON_STRING) memset(text->as.string, '?', text->string_len);
    }
    xjson_free(input);
}
static void notification_zero(void) {
    CHECK(!runtime_live && !runtime_bytes && !lsp_json_live && !lsp_json_bytes && !app_live && !app_bytes);
}
static void notification_server(XrLspServer *server) {
    memset(server, 0, sizeof(*server)); server->initialized = true; server->config.diagnostics_enabled = true;
    server->doc_table = xlsp_document_store_new(xlsp_document_destroy); CHECK(server->doc_table);
    notification_published = notification_indexed = 0;
}
static void notification_free(XrLspServer *server) {
    xlsp_document_store_free(server->doc_table); server->doc_table = NULL;
    xlsp_source_workspace_free(server->source_workspace); server->source_workspace = NULL;
    document_test_free(server->pending_diag); server->pending_diag = NULL; notification_zero();
}
static size_t notification_cases_run(XrLspServer *server, XrLspDocument *doc, const char *uri) {
    size_t count = sizeof(notification_cases) / sizeof(notification_cases[0]);
    for (size_t i = 0; i < count; ++i) {
        const struct NotificationCase *entry = &notification_cases[i]; XrJsonValue *input = notification_input(entry->text, uri);
        size_t resource_calls = runtime_attempts, json_calls = lsp_json_calls, app_calls = app_attempts;
        XlspDocumentNotification out = {0};
        XrXirStatus status = xlsp_document_notification_decode(input, (XlspDocumentNotificationKind)entry->kind, &out);
        if ((status != XR_XIR_OK) != (entry->status != 0)) fprintf(stderr, "case=%zu status=%u %s\n", i, status, entry->text);
        CHECK((status != XR_XIR_OK) == (entry->status != 0));
        CHECK(runtime_attempts == resource_calls && lsp_json_calls == json_calls && app_attempts == app_calls);
        if (status == XR_XIR_OK) CHECK(out.kind == (XlspDocumentNotificationKind)entry->kind && out.uri);
        else {
            CHECK(!out.kind && !out.uri && !out.text && !out.changes && !out.version);
            XrLspServer before = *server; XrLspDocument document = *doc; XrLspDocTable table = *server->doc_table;
            size_t live = runtime_live, bytes = runtime_bytes, app = app_live, app_size = app_bytes;
            unsigned published = notification_published, indexed = notification_indexed;
            notification_call(server, entry->kind, input);
            CHECK(!memcmp(&before, server, sizeof(before)) && !memcmp(&document, doc, sizeof(document)) &&
                !memcmp(&table, server->doc_table, sizeof(table)));
            CHECK(live == runtime_live && bytes == runtime_bytes && app == app_live && app_size == app_bytes &&
                published == notification_published && indexed == notification_indexed && runtime_attempts == resource_calls);
        }
        xjson_free(input);
    }
    XrJsonValue *input = notification_input(open_template, uri); XlspDocumentNotification out = {.version = -1}, saved = out;
    CHECK(xlsp_document_notification_decode(input, XLSP_DOCUMENT_NOTIFICATION_OPEN, &out) == XR_XIR_BAD_STRUCTURE &&
        !memcmp(&out, &saved, sizeof(out)));
    out = (XlspDocumentNotification){0}; XrJsonValue *document = xjson_get(input, "textDocument");
    int capacity = document->as.object.capacity; document->as.object.capacity = 0;
    CHECK(xlsp_document_notification_decode(input, XLSP_DOCUMENT_NOTIFICATION_OPEN, &out) == XR_XIR_BAD_STRUCTURE && !out.kind);
    document->as.object.capacity = capacity;
    XrJsonValue *language = xjson_get(document, "languageId"); unsigned char invalid = 255; memcpy(language->as.string, &invalid, 1);
    CHECK(xlsp_document_notification_decode(input, XLSP_DOCUMENT_NOTIFICATION_OPEN, &out) == XR_XIR_BAD_STRUCTURE && !out.kind);
    xjson_free(input); return count;
}
static int notification_before(const char *uri, bool close) {
    XrLspServer server; notification_server(&server); open_owned(&server, uri, notification_text, 1);
    XrJsonValue *input = notification_input(close ? "{\"textDocument\":{\"uri\":\"@U@\\u0000suffix\"}}" :
        "{\"textDocument\":{\"uri\":\"@U@\",\"uri\":\"file:///different.xr\",\"version\":2},\"contentChanges\":[{\"text\":\"export fn main()->i64 { return 2 }\\n\"}]}", uri);
    if (close) {
        before_xlsp_handle_td_did_close(&server, input);
        CHECK(!xlsp_document_get(&server, uri) && server.source_revision == 2 && notification_indexed == 1);
    } else {
        before_xlsp_handle_td_did_change(&server, input); XrLspDocument *doc = xlsp_document_get(&server, uri);
        CHECK(doc && doc->version == 2 && strstr(doc->content, "return 2") && server.source_revision == 2);
    }
    xjson_free(input); notification_free(&server);
    fputs(close ? "NOTIFICATION_BEFORE NUL_URI_closed_prefix=1 compiler0/0 JSON0/0 app0/0\n" :
        "NOTIFICATION_BEFORE duplicate_URI_changed_first=1 compiler0/0 JSON0/0 app0/0\n", stderr); return 1;
}
static int notification_normal(const char *uri) {
    XrLspServer server; notification_server(&server);
    XrJsonValue *input = notification_input(open_template, uri); notification_call(&server, 1, input); notification_dispose(input);
    XrLspDocument *doc = xlsp_document_get(&server, uri);
    CHECK(doc && doc->version == 1 && doc->source_buffer && doc->source_open && doc->syntax_snapshot &&
        !strcmp(doc->content, notification_text) && server.source_revision == 1 && notification_published == 1);
    size_t count = notification_cases_run(&server, doc, uri);
    input = notification_input(change_template, uri); notification_call(&server, 2, input); notification_dispose(input);
    CHECK(doc->version == 2 && strstr(doc->content, "return 2") && server.source_revision == 2 &&
        server.pending_diag_count == 1 && server.pending_diag[0] == doc);
    input = notification_input("{\"textDocument\":{\"uri\":\"@U@\"}}", uri);
    notification_call(&server, 3, input); notification_dispose(input);
    CHECK(!xlsp_document_get(&server, uri) && server.source_revision == 3 && !server.pending_diag_count &&
        !server.source_workspace && notification_indexed == 1);
    input = notification_input("{\"textDocument\":{\"uri\":\"@U@\",\"languageId\":\"xray\",\"version\":-1,\"text\":\"export fn broken( {\\n\"}}", uri);
    notification_call(&server, 1, input); notification_dispose(input); doc = xlsp_document_get(&server, uri);
    CHECK(doc && doc->version == -1 && doc->parse_error && !strcmp(doc->content, "export fn broken( {\n") && notification_published == 2);
    XrJsonValue *diagnostics = NULL;
    CHECK(xlsp_source_open_diagnostics_json(doc->source_open, doc->source_buffer, doc->syntax_snapshot, &diagnostics) == XR_XIR_OK &&
        diagnostics && diagnostics->type == XR_JSON_ARRAY && diagnostics->as.array.count > 0); xjson_free(diagnostics);
    input = notification_input(change_template, uri); notification_call(&server, 2, input); notification_dispose(input);
    CHECK(doc->version == 2 && !doc->parse_error && strstr(doc->content, "return 2") && server.pending_diag_count == 1);
    input = notification_input("{\"textDocument\":{\"uri\":\"@U@\"}}", uri); notification_call(&server, 3, input); notification_dispose(input);
    CHECK(server.source_revision == 6 && !server.pending_diag_count && notification_indexed == 2 && !server.source_workspace);
    notification_free(&server);
    printf("Notification cases=%zu mutations=3 rejected_no_Source_work=47 actual_open_change_close=6 recovered_edit=1 producer_poisoned_dead=1 compiler0/0 JSON0/0 app0/0\n", count); return 0;
}
static int notification_fault(const char *uri, unsigned kind, size_t point) {
    CHECK(kind == 1 || kind == 2); XrLspServer server; notification_server(&server);
    XrLspDocument *doc = kind == 2 ? open_owned(&server, uri, notification_text, 1) : NULL;
    XrJsonValue *input = notification_input(kind == 1 ? open_template : change_template, uri);
    XrLspServer saved = server; XrLspDocTable table = *server.doc_table; XrLspDocument saved_doc = {0}; if (doc) saved_doc = *doc;
    size_t live = runtime_live, bytes = runtime_bytes, app = app_live, app_size = app_bytes;
    size_t start = runtime_attempts, app_start = app_attempts, json_start = lsp_json_calls;
    runtime_fail_at = point == SIZE_MAX ? SIZE_MAX : start + point;
    notification_call(&server, kind, input); size_t sites = runtime_attempts - start;
    bool injected = runtime_fail_at != SIZE_MAX && runtime_attempts > runtime_fail_at; runtime_fail_at = SIZE_MAX;
    if (point == SIZE_MAX) {
        XrLspDocument *updated = xlsp_document_get(&server, uri); CHECK(updated && updated->version == (int)kind && sites);
    } else {
        CHECK(injected && !memcmp(&server, &saved, sizeof(server)) && !memcmp(server.doc_table, &table, sizeof(table)));
        if (doc) CHECK(!memcmp(doc, &saved_doc, sizeof(*doc)));
        CHECK(live == runtime_live && bytes == runtime_bytes && app == app_live && app_size == app_bytes && !notification_published);
    }
    size_t app_sites = app_attempts - app_start, json_sites = lsp_json_calls - json_start;
    xjson_free(input); notification_free(&server);
    printf("Notification fault kind=%u source=%zu app=%zu json=%zu injected=%u compiler0/0 JSON0/0 app0/0\n",
        kind, sites, app_sites, json_sites, injected ? 1u : 0u); return 0;
}
static int notification_run(int argc, char **argv) {
    CHECK(argc >= 3); const char *uri = argv[1];
    if (!strcmp(argv[2], "--before-close")) return notification_before(uri, true);
    if (!strcmp(argv[2], "--before-change")) return notification_before(uri, false);
    if (!strcmp(argv[2], "--normal")) return notification_normal(uri);
    CHECK(argc == 5 && !strcmp(argv[2], "--fault"));
    return notification_fault(uri, (unsigned)strtoul(argv[3], NULL, 10), (size_t)strtoull(argv[4], NULL, 10));
}
#ifdef XR_OS_WINDOWS
int wmain(int argc, wchar_t **wide) {
    XrWinPathStatus status; char **argv = xr_win_utf16_arguments(argc, wide, &status); CHECK(argv);
    int result = notification_run(argc, argv); xr_win_utf8_arguments_free(argc, argv); return result;
}
#else
int main(int argc, char **argv) { return notification_run(argc, argv); }
#endif
