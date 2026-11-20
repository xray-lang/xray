/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_os_core.c - Unit tests for runtime-neutral OS core helpers
 */

#include "../test_framework.h"
#include "os/os_pipe.h"
#include "os/os_proc.h"
#include "os/os_thread.h"
#include "shared/xr_os_core.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#ifndef XR_OS_WINDOWS
#include <sys/types.h>
#include <unistd.h>
#endif

TEST(os_core_platform_matches_target) {
#ifdef XR_OS_WINDOWS
    ASSERT_STR_EQ(xr_os_core_platform(), "windows");
#elif defined(XR_OS_MACOS)
    ASSERT_STR_EQ(xr_os_core_platform(), "darwin");
#elif defined(XR_OS_LINUX)
    ASSERT_STR_EQ(xr_os_core_platform(), "linux");
#elif defined(XR_OS_BSD)
    ASSERT_STR_EQ(xr_os_core_platform(), "freebsd");
#else
    ASSERT_STR_EQ(xr_os_core_platform(), "unknown");
#endif
}

TEST(os_core_arch_matches_target) {
#ifdef XR_ARCH_ARM64
    ASSERT_STR_EQ(xr_os_core_arch(), "arm64");
#elif defined(XR_ARCH_X86_64)
    ASSERT_STR_EQ(xr_os_core_arch(), "x64");
#elif defined(XR_ARCH_X86)
    ASSERT_STR_EQ(xr_os_core_arch(), "x86");
#elif defined(XR_ARCH_ARM)
    ASSERT_STR_EQ(xr_os_core_arch(), "arm");
#elif defined(XR_ARCH_POWERPC64)
    ASSERT_STR_EQ(xr_os_core_arch(), "ppc64");
#elif defined(XR_ARCH_RISCV64)
    ASSERT_STR_EQ(xr_os_core_arch(), "riscv64");
#else
    ASSERT_STR_EQ(xr_os_core_arch(), "unknown");
#endif
}

TEST(os_core_memory_bytes_converts_units) {
    ASSERT_EQ_INT(xr_os_core_memory_bytes(128, 4096), 524288);
    ASSERT_EQ_INT(xr_os_core_memory_bytes(0, 4096), 0);
    ASSERT_EQ_INT(xr_os_core_memory_bytes(128, 0), 0);
}

TEST(os_core_memory_bytes_saturates_int64) {
    ASSERT_EQ_INT(xr_os_core_memory_bytes(UINT64_MAX, 2), INT64_MAX);
}

TEST(os_core_seconds_from_nsec_normalizes) {
    ASSERT_FLOAT_EQ(xr_os_core_seconds_from_nsec(2, 500000000), 2.5, 0.000001);
    ASSERT_FLOAT_EQ(xr_os_core_seconds_from_nsec(-1, 500000000), 0.0, 0.000001);
    ASSERT_FLOAT_EQ(xr_os_core_seconds_from_nsec(2, -1), 2.0, 0.000001);
}

TEST(os_core_uptime_from_boot_seconds_clamps_negative_elapsed) {
    ASSERT_FLOAT_EQ(xr_os_core_uptime_from_boot_seconds(120, 100), 20.0, 0.000001);
    ASSERT_FLOAT_EQ(xr_os_core_uptime_from_boot_seconds(100, 100), 0.0, 0.000001);
    ASSERT_FLOAT_EQ(xr_os_core_uptime_from_boot_seconds(90, 100), 0.0, 0.000001);
}

TEST(os_core_loadavg_helpers_normalize_values) {
    double avg[3] = {1.0, 1.0, 1.0};
    xr_os_core_loadavg_zero(avg);
    ASSERT_FLOAT_EQ(avg[0], 0.0, 0.000001);
    ASSERT_FLOAT_EQ(avg[1], 0.0, 0.000001);
    ASSERT_FLOAT_EQ(avg[2], 0.0, 0.000001);

    xr_os_core_loadavg_set(avg, 1.25, -2.0, 3.5);
    ASSERT_FLOAT_EQ(avg[0], 1.25, 0.000001);
    ASSERT_FLOAT_EQ(avg[1], 0.0, 0.000001);
    ASSERT_FLOAT_EQ(avg[2], 3.5, 0.000001);
    ASSERT_FLOAT_EQ(xr_os_core_loadavg_from_fixed(65536 * 2), 2.0, 0.000001);
}

static XrProcId spawn_exit_code_child(int code) {
#ifdef XR_OS_WINDOWS
    char code_arg[32];
    snprintf(code_arg, sizeof(code_arg), "exit /B %d", code);
    const char *argv[] = {"cmd.exe", "/C", code_arg, NULL};
    XrProcId child = XR_PROC_INVALID;
    XrProcSpawnOptions options = {0};
    options.memory = xr_proc_system_memory();
    (void)xr_proc_spawn("cmd.exe", argv, &options, &child);
    return child;
#else
    char code_arg[32];
    snprintf(code_arg, sizeof(code_arg), "exit %d", code);
    const char *argv[] = {"sh", "-c", code_arg, NULL};
    XrProcId child = XR_PROC_INVALID;
    XrProcSpawnOptions options = {0};
    options.memory = xr_proc_system_memory();
    (void)xr_proc_spawn("sh", argv, &options, &child);
    return child;
#endif
}

static XrProcId spawn_sleep_child(void) {
#ifdef XR_OS_WINDOWS
    const char *argv[] = {"cmd.exe", "/C", "ping -n 6 127.0.0.1 >NUL", NULL};
    XrProcId child = XR_PROC_INVALID;
    XrProcSpawnOptions options = {0};
    options.memory = xr_proc_system_memory();
    (void)xr_proc_spawn("cmd.exe", argv, &options, &child);
    return child;
#else
    const char *argv[] = {"sh", "-c", "sleep 5", NULL};
    XrProcId child = XR_PROC_INVALID;
    XrProcSpawnOptions options = {0};
    options.memory = xr_proc_system_memory();
    (void)xr_proc_spawn("sh", argv, &options, &child);
    return child;
#endif
}

typedef struct ThreadLocalKeyCtx {
    xr_thread_local_key_t key;
    void *initial;
    void *after_set;
} ThreadLocalKeyCtx;

static int g_thread_local_main_value;
static int g_thread_local_child_value;

static void *thread_local_key_worker(void *arg) {
    ThreadLocalKeyCtx *ctx = (ThreadLocalKeyCtx *) arg;
    ctx->initial = xr_thread_local_get(ctx->key);
    xr_thread_local_set(ctx->key, &g_thread_local_child_value);
    ctx->after_set = xr_thread_local_get(ctx->key);
    return NULL;
}

TEST(os_proc_try_wait_reports_running_then_killed) {
    XrProcId pid = spawn_sleep_child();
    ASSERT_NE(pid, XR_PROC_INVALID);

    int code = 12345;
    ASSERT_EQ_INT(xr_proc_try_wait(pid, &code), XR_PROC_WAIT_RUNNING);
    ASSERT_EQ_INT(code, 12345);

    ASSERT_EQ_INT(xr_proc_kill(pid, 9), 0);
    ASSERT_EQ_INT(xr_proc_wait(pid, &code), 0);
    ASSERT_EQ_INT(code, -1);
}

TEST(os_proc_try_wait_reaps_finished_child) {
    XrProcId pid = spawn_exit_code_child(7);
    ASSERT_NE(pid, XR_PROC_INVALID);

    int code = -1;
    XrProcWaitResult result = XR_PROC_WAIT_RUNNING;
    for (int i = 0; i < 200; i++) {
        result = xr_proc_try_wait(pid, &code);
        if (result != XR_PROC_WAIT_RUNNING)
            break;
        xr_thread_sleep_ms(10);
    }

    ASSERT_EQ_INT(result, XR_PROC_WAIT_EXITED);
    ASSERT_EQ_INT(code, 7);
}

#ifndef XR_OS_WINDOWS
/* Each worker spawns group leaders one at a time, so at most one child per
 * worker is alive. Contention between workers widens the window in which a
 * second setpgid on the same child made Darwin report a spurious EPERM.
 * Under sustained fork load from several processes Darwin also refuses fork
 * or exec with EAGAIN or ENOMEM for a while and may kill fresh children. That
 * is host throttling, not a spawn defect: those attempts back off and retry
 * within a bounded budget, and a child's exit status is not asserted. */
enum { GROUP_SPAWN_WORKERS = 4, GROUP_SPAWN_ROUNDS = 200, GROUP_SPAWN_RETRIES = 200 };
typedef struct GroupSpawnCtx {
    int spawn_failures;
    int not_leader;
    int bad_reap;
    int throttled;
    int first_reap_errno;
    XrOsProcStatus first_status;
    uint32_t first_os_error;
} GroupSpawnCtx;

static bool group_spawn_throttled(XrOsProcStatus status, uint32_t os_error) {
    return status == XR_PROC_OUT_OF_MEMORY || (status == XR_PROC_IO && os_error == EAGAIN);
}

static void *group_spawn_worker(void *arg) {
    GroupSpawnCtx *ctx = (GroupSpawnCtx *) arg;
    const char *argv[] = {"true", NULL};
    for (int round = 0; round < GROUP_SPAWN_ROUNDS; ++round) {
        XrProcSpawnOptions options = {0};
        options.memory = xr_proc_system_memory();
        options.new_process_group = true;
        uint32_t os_error = 0;
        options.os_error = &os_error;
        XrProcId pid = XR_PROC_INVALID;
        XrOsProcStatus status = xr_proc_spawn("true", argv, &options, &pid);
        for (int retry = 0; retry < GROUP_SPAWN_RETRIES && group_spawn_throttled(status, os_error); ++retry) {
            ctx->throttled++;
            xr_thread_sleep_ms(5);
            status = xr_proc_spawn("true", argv, &options, &pid);
        }
        if (status != XR_PROC_OK) {
            if (!ctx->spawn_failures++) {
                ctx->first_status = status;
                ctx->first_os_error = os_error;
            }
            continue;
        }
        /* Spawn returns only after exec, so the child already leads its
         * group; an exited, unreaped leader may no longer be queryable. */
        errno = 0;
        pid_t group = getpgid((pid_t) pid);
        if (group != (pid_t) pid && !(group < 0 && errno == ESRCH))
            ctx->not_leader++;
        int code = -1;
        if (xr_proc_wait(pid, &code) != 0 && !ctx->bad_reap++)
            ctx->first_reap_errno = errno;
        if (xr_proc_close(pid) != 0 && !ctx->bad_reap++)
            ctx->first_reap_errno = errno;
    }
    return NULL;
}

TEST(os_proc_new_group_spawn_survives_contention) {
    GroupSpawnCtx ctx[GROUP_SPAWN_WORKERS];
    xr_thread_t threads[GROUP_SPAWN_WORKERS];
    memset(ctx, 0, sizeof(ctx));
    for (int i = 0; i < GROUP_SPAWN_WORKERS; ++i)
        ASSERT_TRUE(xr_thread_create(&threads[i], group_spawn_worker, &ctx[i]));
    for (int i = 0; i < GROUP_SPAWN_WORKERS; ++i)
        ASSERT_EQ_INT(xr_thread_join(threads[i], NULL), 0);
    for (int i = 0; i < GROUP_SPAWN_WORKERS; ++i) {
        if (ctx[i].spawn_failures || ctx[i].throttled || ctx[i].bad_reap)
            printf("    worker %d: %d spawn failures (first status=%d os_error=%u), %d throttled retries, "
                   "%d failed reaps (first errno=%d)\n",
                   i, ctx[i].spawn_failures, (int) ctx[i].first_status,
                   (unsigned) ctx[i].first_os_error, ctx[i].throttled, ctx[i].bad_reap,
                   ctx[i].first_reap_errno);
        ASSERT_EQ_INT(ctx[i].spawn_failures, 0);
        ASSERT_EQ_INT(ctx[i].not_leader, 0);
        ASSERT_EQ_INT(ctx[i].bad_reap, 0);
    }
}
#endif

TEST(os_thread_local_key_is_per_thread) {
    xr_thread_local_key_t key;
    ASSERT_TRUE(xr_thread_local_key_create(&key));
    ASSERT_EQ_PTR(xr_thread_local_get(key), NULL);
    ASSERT_TRUE(xr_thread_local_set(key, &g_thread_local_main_value));
    ASSERT_EQ_PTR(xr_thread_local_get(key), &g_thread_local_main_value);

    ThreadLocalKeyCtx ctx = {key, NULL, NULL};
    xr_thread_t thread;
    ASSERT_TRUE(xr_thread_create(&thread, thread_local_key_worker, &ctx));
    ASSERT_EQ_INT(xr_thread_join(thread, NULL), 0);

    ASSERT_EQ_PTR(ctx.initial, NULL);
    ASSERT_EQ_PTR(ctx.after_set, &g_thread_local_child_value);
    ASSERT_EQ_PTR(xr_thread_local_get(key), &g_thread_local_main_value);

    xr_thread_local_key_delete(key);
}

TEST(os_pipe_round_trips_bytes) {
    XrPipe pipe_pair;
    ASSERT_EQ_INT(xr_pipe_create(&pipe_pair, NULL), 0);
    ASSERT_NE(pipe_pair.read, XR_PIPE_INVALID);
    ASSERT_NE(pipe_pair.write, XR_PIPE_INVALID);

    const char payload[] = "pipe-ok";
    char buf[sizeof(payload)] = {0};
    ASSERT_EQ_INT(xr_pipe_write(pipe_pair.write, payload, sizeof(payload) - 1),
                  (int) sizeof(payload) - 1);
    ASSERT_EQ_INT(xr_pipe_read(pipe_pair.read, buf, sizeof(payload) - 1),
                  (int) sizeof(payload) - 1);
    ASSERT_MEM_EQ(buf, payload, sizeof(payload) - 1);

    ASSERT_EQ_INT(xr_pipe_close(pipe_pair.read), 0);
    ASSERT_EQ_INT(xr_pipe_close(pipe_pair.write), 0);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("OS Core - platform");
RUN_TEST(os_core_platform_matches_target);
RUN_TEST(os_core_arch_matches_target);

RUN_TEST_SUITE("OS Core - system metrics");
RUN_TEST(os_core_memory_bytes_converts_units);
RUN_TEST(os_core_memory_bytes_saturates_int64);
RUN_TEST(os_core_seconds_from_nsec_normalizes);
RUN_TEST(os_core_uptime_from_boot_seconds_clamps_negative_elapsed);
RUN_TEST(os_core_loadavg_helpers_normalize_values);

RUN_TEST_SUITE("OS Proc");
RUN_TEST(os_proc_try_wait_reports_running_then_killed);
RUN_TEST(os_proc_try_wait_reaps_finished_child);
#ifndef XR_OS_WINDOWS
RUN_TEST(os_proc_new_group_spawn_survives_contention);
#endif

RUN_TEST_SUITE("OS Thread Local");
RUN_TEST(os_thread_local_key_is_per_thread);

RUN_TEST_SUITE("OS Pipe");
RUN_TEST(os_pipe_round_trips_bytes);

TEST_MAIN_END()
