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

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct OsEnvEntry {
    const char *name;
    const char *value;
} OsEnvEntry;

typedef struct OsEnvFake {
    const OsEnvEntry *entries;
    size_t count;
} OsEnvFake;

typedef struct OsEnvParseRecord {
    int calls;
    const char *key;
    size_t key_len;
    const char *value;
    size_t value_len;
} OsEnvParseRecord;

static const char *os_core_fake_getenv(void *ctx, const char *name) {
    const OsEnvFake *env = (const OsEnvFake *) ctx;
    for (size_t i = 0; i < env->count; i++) {
        if (strcmp(env->entries[i].name, name) == 0)
            return env->entries[i].value;
    }
    return NULL;
}

static bool os_core_record_env_entry(void *ctx, const char *key, size_t key_len, const char *value,
                                     size_t value_len) {
    OsEnvParseRecord *record = (OsEnvParseRecord *) ctx;
    record->calls++;
    record->key = key;
    record->key_len = key_len;
    record->value = value;
    record->value_len = value_len;
    return true;
}

static const char *os_core_fake_string(void *ctx) {
    return (const char *) ctx;
}

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

TEST(os_core_sep_and_eol_match_target) {
#ifdef XR_OS_WINDOWS
    ASSERT_STR_EQ(xr_os_core_sep(), "\\");
    ASSERT_STR_EQ(xr_os_core_eol(), "\r\n");
#else
    ASSERT_STR_EQ(xr_os_core_sep(), "/");
    ASSERT_STR_EQ(xr_os_core_eol(), "\n");
#endif
}

TEST(os_core_homedir_prefers_home_env) {
    const OsEnvEntry entries[] = {
        {"HOME", "/home/env"},
#ifdef XR_OS_WINDOWS
        {"USERPROFILE", "C:\\Users\\env"},
#endif
    };
    OsEnvFake env = {entries, sizeof(entries) / sizeof(entries[0])};
    ASSERT_STR_EQ(
        xr_os_core_homedir(os_core_fake_getenv, &env, os_core_fake_string, "/home/system"),
        "/home/env");
}

TEST(os_core_homedir_skips_empty_home) {
    const OsEnvEntry entries[] = {
        {"HOME", ""},
#ifdef XR_OS_WINDOWS
        {"USERPROFILE", "C:\\Users\\env"},
#endif
    };
    OsEnvFake env = {entries, sizeof(entries) / sizeof(entries[0])};
#ifdef XR_OS_WINDOWS
    ASSERT_STR_EQ(
        xr_os_core_homedir(os_core_fake_getenv, &env, os_core_fake_string, "/home/system"),
        "C:\\Users\\env");
#else
    ASSERT_STR_EQ(
        xr_os_core_homedir(os_core_fake_getenv, &env, os_core_fake_string, "/home/system"),
        "/home/system");
#endif
}

TEST(os_core_environ_entry_parses_key_value) {
    OsEnvParseRecord record = {0};
    ASSERT_TRUE(xr_os_core_environ_entry("XRAY_ENV=hello", os_core_record_env_entry, &record));
    ASSERT_EQ_INT(record.calls, 1);
    ASSERT_EQ_UINT(record.key_len, 8);
    ASSERT_MEM_EQ(record.key, "XRAY_ENV", 8);
    ASSERT_EQ_UINT(record.value_len, 5);
    ASSERT_MEM_EQ(record.value, "hello", 5);
}

TEST(os_core_environ_entry_allows_empty_value) {
    OsEnvParseRecord record = {0};
    ASSERT_TRUE(xr_os_core_environ_entry("XRAY_EMPTY=", os_core_record_env_entry, &record));
    ASSERT_EQ_INT(record.calls, 1);
    ASSERT_EQ_UINT(record.key_len, 10);
    ASSERT_MEM_EQ(record.key, "XRAY_EMPTY", 10);
    ASSERT_EQ_UINT(record.value_len, 0);
}

TEST(os_core_environ_entry_rejects_invalid_entries) {
    OsEnvParseRecord record = {0};
    ASSERT_FALSE(xr_os_core_environ_entry(NULL, os_core_record_env_entry, &record));
    ASSERT_FALSE(xr_os_core_environ_entry("NO_EQUALS", os_core_record_env_entry, &record));
    ASSERT_FALSE(xr_os_core_environ_entry("=EMPTY_KEY", os_core_record_env_entry, &record));
    ASSERT_FALSE(xr_os_core_environ_entry("XRAY=value", NULL, &record));
    ASSERT_EQ_INT(record.calls, 0);
}

TEST(os_core_cpu_count_normalizes_invalid_raw_values) {
    ASSERT_EQ_INT(xr_os_core_cpu_count(8), 8);
    ASSERT_EQ_INT(xr_os_core_cpu_count(0), 1);
    ASSERT_EQ_INT(xr_os_core_cpu_count(-4), 1);
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

TEST(os_core_exec_result_schema_is_stable) {
    ASSERT_EQ_INT(XR_OS_CORE_EXEC_FIELD_COUNT, 3);
    ASSERT_EQ_INT(XR_OS_CORE_EXEC_STDOUT, 0);
    ASSERT_EQ_INT(XR_OS_CORE_EXEC_STDERR, 1);
    ASSERT_EQ_INT(XR_OS_CORE_EXEC_EXIT_CODE, 2);
    ASSERT_STR_EQ(XR_OS_CORE_EXEC_FIELD_NAMES[XR_OS_CORE_EXEC_STDOUT], "stdout");
    ASSERT_STR_EQ(XR_OS_CORE_EXEC_FIELD_NAMES[XR_OS_CORE_EXEC_STDERR], "stderr");
    ASSERT_STR_EQ(XR_OS_CORE_EXEC_FIELD_NAMES[XR_OS_CORE_EXEC_EXIT_CODE], "exitCode");
}

TEST(os_core_exec_buffer_capacity_doubles_from_initial_cap) {
    size_t cap = 0;
    ASSERT_TRUE(xr_os_core_exec_buffer_next_cap(0, 0, 0, &cap));
    ASSERT_EQ_UINT(cap, XR_OS_CORE_EXEC_INITIAL_CAP);

    ASSERT_TRUE(xr_os_core_exec_buffer_next_cap(4090, XR_OS_CORE_EXEC_INITIAL_CAP, 5, &cap));
    ASSERT_EQ_UINT(cap, XR_OS_CORE_EXEC_INITIAL_CAP);

    ASSERT_TRUE(xr_os_core_exec_buffer_next_cap(4090, XR_OS_CORE_EXEC_INITIAL_CAP, 6, &cap));
    ASSERT_EQ_UINT(cap, XR_OS_CORE_EXEC_INITIAL_CAP * 2);
}

TEST(os_core_exec_buffer_append_raw_preserves_nul_terminator) {
    char buf[8] = {0};
    size_t len = 0;
    ASSERT_TRUE(xr_os_core_exec_buffer_append_raw(buf, &len, sizeof(buf), "abc", 3));
    ASSERT_EQ_UINT(len, 3);
    ASSERT_STR_EQ(buf, "abc");
    ASSERT_TRUE(xr_os_core_exec_buffer_append_raw(buf, &len, sizeof(buf), "de", 2));
    ASSERT_EQ_UINT(len, 5);
    ASSERT_STR_EQ(buf, "abcde");
    ASSERT_FALSE(xr_os_core_exec_buffer_append_raw(buf, &len, sizeof(buf), "xyz", 3));
    ASSERT_EQ_UINT(len, 5);
    ASSERT_STR_EQ(buf, "abcde");
}

TEST(os_core_exec_windows_exit_code_decodes_low_byte) {
    ASSERT_EQ_INT(xr_os_core_exec_windows_exit_code(-1), -1);
    ASSERT_EQ_INT(xr_os_core_exec_windows_exit_code(0), 0);
    ASSERT_EQ_INT(xr_os_core_exec_windows_exit_code(0x1234), 0x34);
}

static XrProcId spawn_exit_code_child(int code) {
#ifdef XR_OS_WINDOWS
    char code_arg[32];
    snprintf(code_arg, sizeof(code_arg), "exit /B %d", code);
    const char *argv[] = {"cmd.exe", "/C", code_arg, NULL};
    return xr_proc_spawn("cmd.exe", argv);
#else
    char code_arg[32];
    snprintf(code_arg, sizeof(code_arg), "exit %d", code);
    const char *argv[] = {"sh", "-c", code_arg, NULL};
    return xr_proc_spawn("sh", argv);
#endif
}

static XrProcId spawn_sleep_child(void) {
#ifdef XR_OS_WINDOWS
    const char *argv[] = {"cmd.exe", "/C", "ping -n 6 127.0.0.1 >NUL", NULL};
    return xr_proc_spawn("cmd.exe", argv);
#else
    const char *argv[] = {"sh", "-c", "sleep 5", NULL};
    return xr_proc_spawn("sh", argv);
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
RUN_TEST(os_core_sep_and_eol_match_target);

RUN_TEST_SUITE("OS Core - user");
RUN_TEST(os_core_homedir_prefers_home_env);
RUN_TEST(os_core_homedir_skips_empty_home);

RUN_TEST_SUITE("OS Core - environ");
RUN_TEST(os_core_environ_entry_parses_key_value);
RUN_TEST(os_core_environ_entry_allows_empty_value);
RUN_TEST(os_core_environ_entry_rejects_invalid_entries);

RUN_TEST_SUITE("OS Core - system metrics");
RUN_TEST(os_core_cpu_count_normalizes_invalid_raw_values);
RUN_TEST(os_core_memory_bytes_converts_units);
RUN_TEST(os_core_memory_bytes_saturates_int64);
RUN_TEST(os_core_seconds_from_nsec_normalizes);
RUN_TEST(os_core_uptime_from_boot_seconds_clamps_negative_elapsed);
RUN_TEST(os_core_loadavg_helpers_normalize_values);

RUN_TEST_SUITE("OS Core - exec");
RUN_TEST(os_core_exec_result_schema_is_stable);
RUN_TEST(os_core_exec_buffer_capacity_doubles_from_initial_cap);
RUN_TEST(os_core_exec_buffer_append_raw_preserves_nul_terminator);
RUN_TEST(os_core_exec_windows_exit_code_decodes_low_byte);

RUN_TEST_SUITE("OS Proc");
RUN_TEST(os_proc_try_wait_reports_running_then_killed);
RUN_TEST(os_proc_try_wait_reaps_finished_child);

RUN_TEST_SUITE("OS Thread Local");
RUN_TEST(os_thread_local_key_is_per_thread);

RUN_TEST_SUITE("OS Pipe");
RUN_TEST(os_pipe_round_trips_bytes);

TEST_MAIN_END()
