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

#include <stddef.h>
#include <string.h>

typedef struct OsEnvEntry {
    const char *name;
    const char *value;
} OsEnvEntry;

typedef struct OsEnvFake {
    const OsEnvEntry *entries;
    size_t count;
} OsEnvFake;

static const char *os_core_fake_getenv(void *ctx, const char *name) {
    const OsEnvFake *env = (const OsEnvFake *) ctx;
    for (size_t i = 0; i < env->count; i++) {
        if (strcmp(env->entries[i].name, name) == 0)
            return env->entries[i].value;
    }
    return NULL;
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

TEST_MAIN_END()
