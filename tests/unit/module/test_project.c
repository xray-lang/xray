/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_project.c - Unit tests for xray.toml project parsing
 */

#include "../test_framework.h"
#include "module/xproject.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

static char g_tmpdir[512];

static void setup_tmpdir(void) {
    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/xray_test_project_XXXXXX");
#ifdef _WIN32
    ASSERT_EQ_INT(_mkdir(g_tmpdir), 0);
#else
    char *d = mkdtemp(g_tmpdir);
    ASSERT_NOT_NULL(d);
#endif
}

static void teardown_tmpdir(void) {
    char cmd[640];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\" >nul 2>&1", g_tmpdir);
#else
    snprintf(cmd, sizeof(cmd), "rm -rf %s", g_tmpdir);
#endif
    (void) system(cmd);
}

static void write_project_file(const char *content) {
    char path[640];
    FILE *f;

    snprintf(path, sizeof(path), "%s/xray.toml", g_tmpdir);
    f = fopen(path, "w");
    ASSERT_NOT_NULL(f);
    fputs(content, f);
    fclose(f);
}

TEST(load_target_config) {
    setup_tmpdir();
    write_project_file("[project]\n"
                       "name = \"kernel\"\n"
                       "main = \"src/main.xr\"\n"
                       "\n"
                       "[target.x86_64-linux-musl]\n"
                       "profile = \"freestanding\"\n"
                       "toolchain = \"zig\"\n"
                       "cc = \"clang\"\n"
                       "zig = \"/opt/xray/zig\"\n"
                       "sysroot = \"/opt/sysroot\"\n"
                       "linker_script = \"kernel.ld\"\n"
                       "objcopy = \"llvm-objcopy\"\n"
                       "objcopy_output = \"kernel.bin\"\n"
                       "runtime_provider = \"kernel-executor-v1\"\n"
                       "runtime_capabilities = [\"coroutine\", \"task\"]\n"
                       "runtime_hooks = [\"task_alloc\", \"submit\", \"park_wake\", "
                       "\"executor_pump\"]\n"
                       "cc_flags = [\"-ffunction-sections\", \"-fdata-sections\"]\n"
                       "ld_flags = [\"-Wl,--gc-sections\", \"-Wl,-Map,kernel.map\"]\n"
                       "objcopy_flags = [\"-O\", \"binary\"]\n");

    XrProject *project = xr_project_load(NULL, g_tmpdir);
    ASSERT_NOT_NULL(project);
    ASSERT_STR_EQ(project->name, "kernel");

    const XrTargetConfig *cfg = xr_project_find_target_config(project, "x86_64-linux-musl");
    ASSERT_NOT_NULL(cfg);
    ASSERT_STR_EQ(cfg->name, "x86_64-linux-musl");
    ASSERT_STR_EQ(cfg->profile, "freestanding");
    ASSERT_STR_EQ(cfg->toolchain, "zig");
    ASSERT_STR_EQ(cfg->cc, "clang");
    ASSERT_STR_EQ(cfg->zig, "/opt/xray/zig");
    ASSERT_STR_EQ(cfg->sysroot, "/opt/sysroot");
    ASSERT_STR_EQ(cfg->linker_script, "kernel.ld");
    ASSERT_STR_EQ(cfg->objcopy, "llvm-objcopy");
    ASSERT_STR_EQ(cfg->objcopy_output, "kernel.bin");
    ASSERT_STR_EQ(cfg->runtime_provider, "kernel-executor-v1");
    ASSERT_EQ_INT(cfg->n_runtime_capabilities, 2);
    ASSERT_STR_EQ(cfg->runtime_capabilities[0], "coroutine");
    ASSERT_STR_EQ(cfg->runtime_capabilities[1], "task");
    ASSERT_EQ_INT(cfg->n_runtime_hooks, 4);
    ASSERT_STR_EQ(cfg->runtime_hooks[3], "executor_pump");
    ASSERT_EQ_INT(cfg->n_cc_flags, 2);
    ASSERT_STR_EQ(cfg->cc_flags[0], "-ffunction-sections");
    ASSERT_STR_EQ(cfg->cc_flags[1], "-fdata-sections");
    ASSERT_EQ_INT(cfg->n_ld_flags, 2);
    ASSERT_STR_EQ(cfg->ld_flags[0], "-Wl,--gc-sections");
    ASSERT_STR_EQ(cfg->ld_flags[1], "-Wl,-Map,kernel.map");
    ASSERT_EQ_INT(cfg->n_objcopy_flags, 2);
    ASSERT_STR_EQ(cfg->objcopy_flags[0], "-O");
    ASSERT_STR_EQ(cfg->objcopy_flags[1], "binary");
    ASSERT_NULL(xr_project_find_target_config(project, "aarch64-linux-musl"));

    xr_project_free(project);
    teardown_tmpdir();
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Project Configuration");
RUN_TEST(load_target_config);
TEST_MAIN_END()
