/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcoro_monitor.c - Coroutine monitoring module
 *
 * Provides CLI real-time monitoring and HTTP API monitoring.
 * Supports --coro-watch and --coro-http command line arguments.
 */

#include "xcoroutine.h"
#include "../base/xchecks.h"
#include "../base/xtime.h"
#include "xworker.h"
#include "xexec_state.h"
#include "xproc.h"
#include "../runtime/xisolate_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../base/xthread.h"
#include "../base/xmalloc.h"

// ========== Thread-Local State (per-Isolate monitor support) ==========

static XR_THREAD_LOCAL XrayIsolate *tls_monitor_isolate = NULL;
static XR_THREAD_LOCAL int tls_watch_interval_ms = 0;
static XR_THREAD_LOCAL int tls_http_port = 0;
static XR_THREAD_LOCAL volatile bool tls_monitor_running = false;
static XR_THREAD_LOCAL xr_thread_t tls_watch_thread;

// ========== ANSI Escape Sequences ==========

#define ANSI_CLEAR "\033[2J\033[H"    // Clear screen and move cursor to top-left
#define ANSI_HIDE_CURSOR "\033[?25l"  // Hide cursor
#define ANSI_SHOW_CURSOR "\033[?25h"  // Show cursor
#define ANSI_BOLD "\033[1m"
#define ANSI_RESET "\033[0m"
#define ANSI_GREEN "\033[32m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_RED "\033[31m"
#define ANSI_CYAN "\033[36m"

// ========== Monitor Output Functions ==========

static void print_header(int interval_ms) {
    // Clear screen: try ANSI first
    printf("\033[H\033[2J");
    fflush(stdout);

    printf("╔══════════════════════════════════════════════════════════════════════════╗\n");
    printf(
        "║" ANSI_BOLD ANSI_CYAN
        "                    Coroutine Monitor Dashboard                               " ANSI_RESET
        "║\n");
    printf("║  Refresh interval: %dms | Press Ctrl+C to exit                                       "
           " ║\n",
           interval_ms);
    printf("╠══════════════════════════════════════════════════════════════════════════╣\n");
    fflush(stdout);
}

static void print_stats(XrayIsolate *X) {
    XrRuntime *runtime = (XrRuntime *) xr_isolate_get_vm_state(X)->runtime;
    if (!runtime) {
        printf("║ Runtime not initialized                                                ║\n");
        return;
    }

    int ready_count = 0, blocked_count = 0;
    int active_count = xr_runtime_active_coros(runtime);
    for (int wi = 0; wi < runtime->worker_count; wi++) {
        XrWorker *w = &runtime->workers[wi];
        blocked_count += w->p.blocked_count;
        for (int p = 0; p < XR_RUNQ_COUNT; p++) {
            ready_count += xr_runq_len(&w->p.runq[p]);
        }
    }

    int total = ready_count + blocked_count + active_count;

    printf("║ " ANSI_BOLD "Stats" ANSI_RESET "  Total: " ANSI_CYAN "%-6d" ANSI_RESET
           " | Running: " ANSI_GREEN "%-4d" ANSI_RESET " | Ready: " ANSI_YELLOW "%-5d" ANSI_RESET
           " | Blocked: " ANSI_RED "%-5d" ANSI_RESET "     ║\n",
           total, active_count, ready_count, blocked_count);
}

static void print_top_coros(XrayIsolate *X, int limit) {
    XrRuntime *runtime = (XrRuntime *) xr_isolate_get_vm_state(X)->runtime;
    if (!runtime)
        return;

    printf("╠══════════════════════════════════════════════════════════════════════════╣\n");
    printf("║ " ANSI_BOLD "Top %d Coroutines" ANSI_RESET
           "                                                              ║\n",
           limit);
    printf("║ ────────────────────────────────────────────────────────────────────── ║\n");
    printf("║   ID   │ Status  │ Priority │ Reductions                                   ║\n");
    printf("║ ────────────────────────────────────────────────────────────────────── ║\n");

    int shown = 0;
    XrCoroutine *snap_buf[256];

    // Iterate all workers' run queues
    for (int wi = 0; wi < runtime->worker_count && shown < limit; wi++) {
        XrWorker *w = &runtime->workers[wi];
        for (int p = 0; p < XR_RUNQ_COUNT && shown < limit; p++) {
            int n = xr_steal_queue_snapshot(&w->p.runq[p].deque, snap_buf,
                                            (limit - shown < 256) ? (limit - shown) : 256);
            for (int i = 0; i < n && shown < limit; i++) {
                XrCoroutine *coro = snap_buf[i];
                printf("║   %-5d │ " ANSI_YELLOW "READY" ANSI_RESET
                       "   │ P%-5d │ %-10lld                             ║\n",
                       coro->id, xr_coro_get_priority(xr_coro_flags_load(coro)),
                       (long long) coro->reductions);
                shown++;
            }
        }
        // Blocked coroutines
        XrCoroutine *bc = w->p.blocked_head;
        while (bc && shown < limit) {
            printf("║   %-5d │ " ANSI_RED "BLOCKED" ANSI_RESET
                   " │ P%-5d │ %-10lld                             ║\n",
                   bc->id, xr_coro_get_priority(xr_coro_flags_load(bc)),
                   (long long) bc->reductions);
            shown++;
            bc = bc->next;
        }
    }

    if (shown == 0) {
        printf("║   (no coroutines)                                                        ║\n");
    }
}

static void print_blocked_coros(XrayIsolate *X) {
    (void) X;
    printf("╠══════════════════════════════════════════════════════════════════════════╣\n");
    printf("║ Blocked queue managed by Runtime                                         ║\n");
}

static void print_footer(void) {
    printf("╚══════════════════════════════════════════════════════════════════════════╝\n");
    fflush(stdout);
}

// ========== Monitor Thread ==========

// Monitor thread context (passed to thread)
typedef struct {
    XrayIsolate *isolate;
    int interval_ms;
    volatile bool *running;
} MonitorThreadCtx;

static void *watch_thread_func(void *arg) {
    MonitorThreadCtx *ctx = (MonitorThreadCtx *) arg;

    // Hide cursor
    printf(ANSI_HIDE_CURSOR);
    fflush(stdout);

    while (*ctx->running) {
        print_header(ctx->interval_ms);
        print_stats(ctx->isolate);
        print_top_coros(ctx->isolate, 10);
        print_blocked_coros(ctx->isolate);
        print_footer();

        // Force flush output
        fflush(stdout);

        // Sleep for specified interval
        xr_time_sleep_ms((uint64_t) ctx->interval_ms);
    }

    // Show cursor
    printf(ANSI_SHOW_CURSOR);
    fflush(stdout);

    xr_free(ctx);
    return NULL;
}

// ========== Public API ==========

void xr_coro_monitor_start(XrayIsolate *X, int watch_interval_ms, int http_port) {
    XR_DCHECK(X != NULL, "coro_monitor_start: NULL isolate");
    tls_monitor_isolate = X;
    tls_watch_interval_ms = watch_interval_ms;
    tls_http_port = http_port;

    // Start CLI monitor thread
    if (watch_interval_ms > 0) {
        printf("\n");
        printf("╔══════════════════════════════════════════════════════════════╗\n");
        printf("║              Coroutine Monitor Enabled                       ║\n");
        printf("║  Refresh interval: %d ms                                     ║\n",
               watch_interval_ms);
        printf("╚══════════════════════════════════════════════════════════════╝\n");
        printf("\nWaiting 2 seconds before starting monitor...\n\n");

        xr_thread_sleep_ms(2000);

        tls_monitor_running = true;

        // Create context for thread
        MonitorThreadCtx *ctx = xr_malloc(sizeof(MonitorThreadCtx));
        ctx->isolate = X;
        ctx->interval_ms = watch_interval_ms;
        ctx->running = &tls_monitor_running;

        xr_thread_create(&tls_watch_thread, watch_thread_func, ctx);
    }

    // HTTP monitor (hint to user, needs script support)
    if (http_port > 0) {
        printf("\n");
        printf("╔══════════════════════════════════════════════════════════════╗\n");
        printf("║              HTTP Monitor Enabled                            ║\n");
        printf("║  Port: %d                                                     ║\n", http_port);
        printf("║  Access: http://localhost:%d/                                 ║\n", http_port);
        printf("╚══════════════════════════════════════════════════════════════╝\n");
        printf("\nHint: HTTP monitor requires Coro API calls in script for data\n\n");
    }
}

static XR_UNUSED void xr_coro_monitor_stop(void) {
    if (tls_monitor_running) {
        tls_monitor_running = false;
        xr_thread_join(tls_watch_thread, NULL);
    }
}
