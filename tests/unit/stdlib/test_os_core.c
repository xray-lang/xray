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

TEST(os_core_tmpdir_prefers_tmpdir) {
    const OsEnvEntry entries[] = {
        {"TMPDIR", "/xray/tmpdir"},
        {"TMP", "/xray/tmp"},
        {"TEMP", "/xray/temp"},
    };
    OsEnvFake env = {entries, sizeof(entries) / sizeof(entries[0])};
    ASSERT_STR_EQ(xr_os_core_tmpdir(os_core_fake_getenv, &env), "/xray/tmpdir");
}

TEST(os_core_tmpdir_skips_empty_values) {
    const OsEnvEntry entries[] = {
        {"TMPDIR", ""},
        {"TMP", "/xray/tmp"},
        {"TEMP", "/xray/temp"},
    };
    OsEnvFake env = {entries, sizeof(entries) / sizeof(entries[0])};
    ASSERT_STR_EQ(xr_os_core_tmpdir(os_core_fake_getenv, &env), "/xray/tmp");
}

TEST(os_core_tmpdir_uses_temp_after_missing_tmp) {
    const OsEnvEntry entries[] = {
        {"TEMP", "/xray/temp"},
    };
    OsEnvFake env = {entries, sizeof(entries) / sizeof(entries[0])};
    ASSERT_STR_EQ(xr_os_core_tmpdir(os_core_fake_getenv, &env), "/xray/temp");
}

TEST(os_core_tmpdir_falls_back_without_env) {
    OsEnvFake env = {NULL, 0};
#ifdef XR_OS_WINDOWS
    ASSERT_STR_EQ(xr_os_core_tmpdir(os_core_fake_getenv, &env), "C:\\Windows\\Temp");
#else
    ASSERT_STR_EQ(xr_os_core_tmpdir(os_core_fake_getenv, &env), "/tmp");
#endif
    ASSERT_STR_EQ(xr_os_core_tmpdir(NULL, NULL), xr_os_core_tmpdir(os_core_fake_getenv, &env));
}

TEST(os_core_username_prefers_system_username) {
    const OsEnvEntry entries[] = {
#ifdef XR_OS_WINDOWS
        {"USERNAME", "env-user"},
#else
        {"USER", "env-user"},
#endif
        {"LOGNAME", "log-user"},
    };
    OsEnvFake env = {entries, sizeof(entries) / sizeof(entries[0])};
    ASSERT_STR_EQ(
        xr_os_core_username(os_core_fake_string, "system-user", os_core_fake_getenv, &env),
        "system-user");
}

TEST(os_core_username_falls_back_to_env) {
    const OsEnvEntry entries[] = {
#ifdef XR_OS_WINDOWS
        {"USERNAME", "win-user"},
        {"USER", "env-user"},
#else
        {"USER", "env-user"},
#endif
        {"LOGNAME", "log-user"},
    };
    OsEnvFake env = {entries, sizeof(entries) / sizeof(entries[0])};
#ifdef XR_OS_WINDOWS
    ASSERT_STR_EQ(xr_os_core_username(NULL, NULL, os_core_fake_getenv, &env), "win-user");
#else
    ASSERT_STR_EQ(xr_os_core_username(NULL, NULL, os_core_fake_getenv, &env), "env-user");
#endif
}

TEST(os_core_username_skips_empty_env_values) {
    const OsEnvEntry entries[] = {
#ifdef XR_OS_WINDOWS
        {"USERNAME", ""},
#endif
        {"USER", ""},
        {"LOGNAME", "log-user"},
    };
    OsEnvFake env = {entries, sizeof(entries) / sizeof(entries[0])};
    ASSERT_STR_EQ(xr_os_core_username(NULL, NULL, os_core_fake_getenv, &env), "log-user");
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

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("OS Core - platform");
RUN_TEST(os_core_platform_matches_target);
RUN_TEST(os_core_arch_matches_target);
RUN_TEST(os_core_sep_and_eol_match_target);

RUN_TEST_SUITE("OS Core - tmpdir");
RUN_TEST(os_core_tmpdir_prefers_tmpdir);
RUN_TEST(os_core_tmpdir_skips_empty_values);
RUN_TEST(os_core_tmpdir_uses_temp_after_missing_tmp);
RUN_TEST(os_core_tmpdir_falls_back_without_env);

RUN_TEST_SUITE("OS Core - user");
RUN_TEST(os_core_username_prefers_system_username);
RUN_TEST(os_core_username_falls_back_to_env);
RUN_TEST(os_core_username_skips_empty_env_values);
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

TEST_MAIN_END()
