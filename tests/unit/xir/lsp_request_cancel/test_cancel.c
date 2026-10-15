/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_request_cancel.h"
#include "app/lsp/xlsp_pending_requests.h"
#include "app/lsp/xlsp_async.h"
#include "base/xmalloc.h"
#include "os/os_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d:%s\n", __LINE__, #c); exit(1); } } while (0)
#define lsp_log(...) ((void)0)
#include "baseline_cancel.inc.c"
#undef lsp_log

typedef struct Gate { _Atomic bool go[2], entered[2]; } Gate;
typedef struct Stats { _Atomic int executed[2]; int callbacks[2], null_result[2], token[2], allocated, freed; } Stats;
typedef struct Work { Gate *gate; Stats *stats; XrLspTask *task; uint64_t expected_id; unsigned index; } Work;

static void execute(void *data) {
    Work *work = data; unsigned index = work->index;
    CHECK(work->task->data == data); atomic_store(&work->stats->executed[index], 1);
    atomic_store(&work->gate->entered[index], true); uint64_t deadline = xr_time_monotonic_ms() + 5000;
    while (!atomic_load(&work->gate->go[index])) { CHECK(xr_time_monotonic_ms() < deadline); xr_thread_sleep_ms(1); }
    work->task->result = work;
}
static void complete(void *data, void *result) {
    Work *work = data; unsigned index = work->index; Stats *stats = work->stats;
    CHECK(work->task->data == data && work->task->task_id == work->expected_id);
    CHECK(!result || result == work); ++stats->callbacks[index]; stats->null_result[index] = result == NULL;
    stats->token[index] = atomic_load(&work->task->cancel_requested) ? 1 : 0;
    ++stats->freed; xr_free(work);
}
static XrLspTask *submit(XrLspAsync *async, Gate *gate, Stats *stats, unsigned index, int64_t key) {
    Work *work = xr_calloc(1, sizeof(*work)); CHECK(work); ++stats->allocated;
    work->gate = gate; work->stats = stats; work->index = index;
    XrLspTask *task = key ? xlsp_task_new_ex(execute, complete, work, XLSP_TASK_NORMAL, XLSP_TASK_TYPE_GENERIC, key) :
        xlsp_task_new(execute, complete, work, XLSP_TASK_NORMAL);
    CHECK(task); work->task = task; work->expected_id = xlsp_async_submit(async, task); CHECK(work->expected_id);
    return task;
}
static void wait_entered(Gate *gate, unsigned index) {
    uint64_t deadline = xr_time_monotonic_ms() + 5000;
    while (!atomic_load(&gate->entered[index])) { CHECK(xr_time_monotonic_ms() < deadline); xr_thread_sleep_ms(1); }
}
static void wait_completed(XrLspAsync *async, int count) {
    uint64_t deadline = xr_time_monotonic_ms() + 5000;
    while (atomic_load(&async->completed_count) < count) { CHECK(xr_time_monotonic_ms() < deadline); xr_thread_sleep_ms(1); }
}
static XlspRequestId parsed(const char *text) {
    XlspRequestId id = {0}; XrJsonValue *json = xjson_parse(text, strlen(text)); CHECK(json);
    CHECK(xlsp_request_id_parse(json, &id) == XLSP_PROTOCOL_OK); xjson_free(json); return id;
}
static void bind_request(XrLspServer *server, const XlspRequestId *key, uint64_t task) {
    XlspRequestId owned = {0}; CHECK(xlsp_request_id_clone(key, &owned) == XLSP_PROTOCOL_OK);
    CHECK(xlsp_pending_request_add(&server->pending_requests, owned, "fixture/request", 99));
    XlspPendingRequest *pending = xlsp_pending_request_find(&server->pending_requests, key); CHECK(pending);
    pending->source_task_id = task;
}
static bool run_case(unsigned mode, bool baseline) {
    Gate gate = {0}; Stats stats = {0}; XrLspServer server = {0};
    XrLspAsync *async = xlsp_async_new(NULL, NULL); CHECK(async); server.async = async;
    server.source_revision = 19; server.next_request_id = -1;
    if (mode == 5) atomic_store(&async->next_task_id, (UINT64_C(1) << 48) + 17);
    int64_t worker_key = mode == 1 || mode == 10 ? 42 : 0;
    XrLspTask *tasks[2]; tasks[0] = submit(async, &gate, &stats, 0, worker_key); wait_entered(&gate, 0);
    tasks[1] = submit(async, &gate, &stats, 1, worker_key);
    uint64_t task_ids[2] = {tasks[0]->task_id, tasks[1]->task_id};
    CHECK(task_ids[0] != task_ids[1]); if (mode == 5) CHECK(task_ids[0] > INT32_MAX);
    XlspRequestId zero = {.kind = XLSP_ID_NUMBER, .as.number = 0};
    XlspRequestId string = parsed("{\"id\":\"owned\\u0000λ\"}");
    XlspRequestId extra = {0}, empty = {0}; bind_request(&server, &zero, task_ids[0]); bind_request(&server, &string, task_ids[1]);
    const XlspRequestId *target = &zero; bool expect_found = true; unsigned expected_mask = 1;
    if (mode == 0 || mode == 8) {
        CHECK(xlsp_pending_request_remove(&server.pending_requests, &zero)); expect_found = false; expected_mask = 0;
    }
    if (mode == 1 || mode == 18) {
        extra = (XlspRequestId){.kind = XLSP_ID_NUMBER, .as.number = 42}; target = &extra; expect_found = false; expected_mask = 0;
    }
    if (mode == 3 || mode == 6) { target = &string; expected_mask = 2; }
    if (mode == 4 || mode == 16) {
        extra = parsed(mode == 4 ? "{\"id\":\"owned\\u0000x\"}" : "{\"id\":\"0\"}");
        target = &extra; expect_found = false; expected_mask = 0;
    }
    if (mode == 7) {
        atomic_store(&gate.go[0], true); atomic_store(&gate.go[1], true); wait_completed(async, 2); expected_mask = 0;
    }
    if (mode == 8) {
        atomic_store(&gate.go[0], true); wait_completed(async, 1); CHECK(xlsp_async_poll(async) == 1);
        wait_entered(&gate, 1);
    }
    if (mode == 9) {
        CHECK(xlsp_pending_request_remove(&server.pending_requests, &zero));
        CHECK(xlsp_pending_request_remove(&server.pending_requests, &string));
        bind_request(&server, &zero, task_ids[1]); expected_mask = 2;
    }
    if (mode == 10) {
        extra = (XlspRequestId){.kind = XLSP_ID_NUMBER, .as.number = 42}; target = &extra;
        bind_request(&server, target, 0); expected_mask = 0;
    }
    if (mode == 11) { server.async = NULL; expected_mask = 0; }
    if (mode == 12 || mode == 13 || mode == 17) {
        expect_found = false; expected_mask = 0; if (mode == 13) target = &empty; if (mode == 17) target = NULL;
    }
    if (mode == 14 || mode == 15) {
        extra = (XlspRequestId){.kind = XLSP_ID_NUMBER, .as.number = mode == 14 ? INT32_MIN : INT32_MAX};
        CHECK(xlsp_pending_request_remove(&server.pending_requests, &zero)); bind_request(&server, &extra, task_ids[0]); target = &extra;
    }
    XrLspServer before = server;
    bool found = baseline ? pending_request_cancel(mode == 12 ? NULL : &server, target) :
        xlsp_request_cancel(mode == 12 ? NULL : &server, target);
    if (mode == 6) CHECK((baseline ? pending_request_cancel(&server, target) : xlsp_request_cancel(&server, target)) == found);
    unsigned mask = 0;
    for (unsigned i = 0; i < 2; ++i)
        if (stats.callbacks[i] ? stats.token[i] : atomic_load(&tasks[i]->cancel_requested)) mask |= 1u << i;
    XrLspServer expected = before;
    if (expect_found) {
        XlspPendingRequest *pending = xlsp_pending_request_find(&expected.pending_requests, target); CHECK(pending);
        pending->cancelled = true;
    }
    bool table_ok = !memcmp(&expected, &server, sizeof(server));
    atomic_store(&gate.go[0], true); atomic_store(&gate.go[1], true);
    uint64_t deadline = xr_time_monotonic_ms() + 5000;
    while (stats.callbacks[0] + stats.callbacks[1] != 2) {
        xlsp_async_poll(async); CHECK(xr_time_monotonic_ms() < deadline); xr_thread_sleep_ms(1);
    }
    xlsp_async_free(async); server.async = NULL;
    CHECK(stats.allocated == 2 && stats.freed == 2 && stats.callbacks[0] == 1 && stats.callbacks[1] == 1);
    CHECK(atomic_load(&stats.executed[0]) == 1);
    CHECK(atomic_load(&stats.executed[1]) == ((mask & 2) ? 0 : 1));
    CHECK(stats.null_result[0] == ((mask & 1) ? 1 : 0) && stats.null_result[1] == ((mask & 2) ? 1 : 0));
    for (int i = 0; i < MAX_PENDING_REQUESTS; ++i)
        if (server.pending_requests.requests[i].id.kind != XLSP_ID_NONE)
            CHECK(xlsp_pending_request_remove(&server.pending_requests, &server.pending_requests.requests[i].id));
    CHECK(!server.pending_requests.count); xlsp_request_id_free(&string); xlsp_request_id_free(&extra);
    bool passed = found == expect_found && mask == expected_mask && table_ok;
    printf("{\"mode\":%u,\"baseline\":%s,\"found\":%s,\"worker_key\":%lld,\"cancel_mask\":%u,\"expected_mask\":%u,\"node_callbacks\":[%d,%d],\"owners_freed\":%d,\"passed\":%s}\n",
        mode, baseline ? "true" : "false", found ? "true" : "false", (long long)worker_key, mask, expected_mask,
        stats.callbacks[0], stats.callbacks[1], stats.freed, passed ? "true" : "false");
    return passed;
}
int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "baseline")) return run_case((unsigned)strtoul(argv[2], NULL, 10), true) ? 0 : 1;
    for (unsigned mode = 0; mode < 19; ++mode) if (!run_case(mode, false)) return 1;
    return 0;
}
