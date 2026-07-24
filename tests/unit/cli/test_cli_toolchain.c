/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_cli_toolchain.c - Unit tests for AOT target/toolchain planning
 */

#include "../test_framework.h"
#include "app/cli/xcli_toolchain.h"

#include <stdlib.h>
#ifndef _WIN32
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static void set_test_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value)
        setenv(name, value, 1);
    else
        unsetenv(name);
#endif
}

TEST(parse_native_target) {
    XrCliBuildTarget target;
    char err[256];

    ASSERT_TRUE(xr_cli_build_target_parse(NULL, &target, err, sizeof(err)));
    ASSERT_TRUE(target.is_native);
    ASSERT_STR_EQ(target.name, "native");
    ASSERT_NULL(target.zig_triple);
    ASSERT_EQ_INT(target.endian, XR_CLI_TARGET_ENDIAN_NATIVE);
    ASSERT_TRUE(target.pointer_bits == 32 || target.pointer_bits == 64);
    ASSERT_STR_EQ(xr_cli_build_target_default_output(&target), "a.out");
}

TEST(list_supported_targets) {
    size_t count = 0;
    const char *const *targets = xr_cli_build_target_supported_names(&count);

    ASSERT_NOT_NULL(targets);
    ASSERT_EQ_INT((int) count, 11);
    ASSERT_STR_EQ(targets[0], "native");
    ASSERT_STR_EQ(targets[1], "x86_64-unknown-none");
    ASSERT_STR_EQ(targets[2], "riscv32imac-unknown-none-elf");
    ASSERT_STR_EQ(targets[3], "riscv64gc-unknown-none-elf");
    ASSERT_STR_EQ(targets[4], "thumbv7em-none-eabi");
    ASSERT_STR_EQ(targets[5], "i386-linux-musl");
    ASSERT_STR_EQ(targets[6], "x86_64-linux-musl");
    ASSERT_STR_EQ(targets[8], "powerpc64-linux-musl");
    ASSERT_STR_EQ(targets[10], "aarch64-windows-gnu");
}

TEST(parse_x86_64_none_target) {
    XrCliBuildTarget target;
    char err[256];

    ASSERT_TRUE(xr_cli_build_target_parse("x86_64-unknown-none", &target, err, sizeof(err)));
    ASSERT_FALSE(target.is_native);
    ASSERT_STR_EQ(target.name, "x86_64-unknown-none");
    ASSERT_STR_EQ(target.zig_triple, "x86_64-freestanding-none");
    ASSERT_EQ_INT(target.arch, XR_CLI_TARGET_ARCH_X86_64);
    ASSERT_EQ_INT(target.os, XR_CLI_TARGET_OS_NONE);
    ASSERT_EQ_INT(target.abi, XR_CLI_TARGET_ABI_NONE);
    ASSERT_EQ_INT(target.endian, XR_CLI_TARGET_ENDIAN_LITTLE);
    ASSERT_EQ_INT(target.pointer_bits, 64);
    ASSERT_STR_EQ(xr_cli_build_target_default_output(&target), "a.out");
}

TEST(parse_linux_musl_target) {
    XrCliBuildTarget target;
    char err[256];

    ASSERT_TRUE(xr_cli_build_target_parse("x86_64-linux-musl", &target, err, sizeof(err)));
    ASSERT_FALSE(target.is_native);
    ASSERT_STR_EQ(target.name, "x86_64-linux-musl");
    ASSERT_STR_EQ(target.zig_triple, "x86_64-linux-musl");
    ASSERT_EQ_INT(target.arch, XR_CLI_TARGET_ARCH_X86_64);
    ASSERT_EQ_INT(target.os, XR_CLI_TARGET_OS_LINUX);
    ASSERT_EQ_INT(target.abi, XR_CLI_TARGET_ABI_MUSL);
    ASSERT_EQ_INT(target.endian, XR_CLI_TARGET_ENDIAN_LITTLE);
    ASSERT_EQ_INT(target.pointer_bits, 64);
    ASSERT_STR_EQ(xr_cli_build_target_default_output(&target), "a.out");
}

TEST(parse_i386_linux_musl_target) {
    XrCliBuildTarget target;
    char err[256];

    ASSERT_TRUE(xr_cli_build_target_parse("i386-linux-musl", &target, err, sizeof(err)));
    ASSERT_FALSE(target.is_native);
    ASSERT_STR_EQ(target.name, "i386-linux-musl");
    ASSERT_STR_EQ(target.zig_triple, "x86-linux-musl");
    ASSERT_EQ_INT(target.arch, XR_CLI_TARGET_ARCH_X86);
    ASSERT_EQ_INT(target.os, XR_CLI_TARGET_OS_LINUX);
    ASSERT_EQ_INT(target.abi, XR_CLI_TARGET_ABI_MUSL);
    ASSERT_EQ_INT(target.endian, XR_CLI_TARGET_ENDIAN_LITTLE);
    ASSERT_EQ_INT(target.pointer_bits, 32);
}

TEST(parse_powerpc64_linux_musl_target) {
    XrCliBuildTarget target;
    char err[256];

    ASSERT_TRUE(xr_cli_build_target_parse("powerpc64-linux-musl", &target, err, sizeof(err)));
    ASSERT_FALSE(target.is_native);
    ASSERT_STR_EQ(target.name, "powerpc64-linux-musl");
    ASSERT_STR_EQ(target.zig_triple, "powerpc64-linux-musl");
    ASSERT_EQ_INT(target.arch, XR_CLI_TARGET_ARCH_POWERPC64);
    ASSERT_EQ_INT(target.os, XR_CLI_TARGET_OS_LINUX);
    ASSERT_EQ_INT(target.abi, XR_CLI_TARGET_ABI_MUSL);
    ASSERT_EQ_INT(target.endian, XR_CLI_TARGET_ENDIAN_BIG);
    ASSERT_EQ_INT(target.pointer_bits, 64);
}

TEST(parse_riscv32_none_elf_target) {
    XrCliBuildTarget target;
    char err[256];

    ASSERT_TRUE(
        xr_cli_build_target_parse("riscv32imac-unknown-none-elf", &target, err, sizeof(err)));
    ASSERT_FALSE(target.is_native);
    ASSERT_STR_EQ(target.name, "riscv32imac-unknown-none-elf");
    ASSERT_STR_EQ(target.zig_triple, "riscv32-freestanding-none");
    ASSERT_EQ_INT(target.arch, XR_CLI_TARGET_ARCH_RISCV32);
    ASSERT_EQ_INT(target.os, XR_CLI_TARGET_OS_NONE);
    ASSERT_EQ_INT(target.abi, XR_CLI_TARGET_ABI_NONE);
    ASSERT_EQ_INT(target.endian, XR_CLI_TARGET_ENDIAN_LITTLE);
    ASSERT_EQ_INT(target.pointer_bits, 32);
    ASSERT_STR_EQ(xr_cli_build_target_default_output(&target), "a.out");
}

TEST(parse_riscv64_none_elf_target) {
    XrCliBuildTarget target;
    char err[256];

    ASSERT_TRUE(xr_cli_build_target_parse("riscv64gc-unknown-none-elf", &target, err, sizeof(err)));
    ASSERT_FALSE(target.is_native);
    ASSERT_STR_EQ(target.name, "riscv64gc-unknown-none-elf");
    ASSERT_STR_EQ(target.zig_triple, "riscv64-freestanding-none");
    ASSERT_EQ_INT(target.arch, XR_CLI_TARGET_ARCH_RISCV64);
    ASSERT_EQ_INT(target.os, XR_CLI_TARGET_OS_NONE);
    ASSERT_EQ_INT(target.abi, XR_CLI_TARGET_ABI_NONE);
    ASSERT_EQ_INT(target.endian, XR_CLI_TARGET_ENDIAN_LITTLE);
    ASSERT_EQ_INT(target.pointer_bits, 64);
    ASSERT_STR_EQ(xr_cli_build_target_default_output(&target), "a.out");
}

TEST(parse_thumbv7em_none_eabi_target) {
    XrCliBuildTarget target;
    char err[256];

    ASSERT_TRUE(xr_cli_build_target_parse("thumbv7em-none-eabi", &target, err, sizeof(err)));
    ASSERT_FALSE(target.is_native);
    ASSERT_STR_EQ(target.name, "thumbv7em-none-eabi");
    ASSERT_STR_EQ(target.zig_triple, "thumb-freestanding-eabi");
    ASSERT_STR_EQ(target.cpu, "cortex_m4");
    ASSERT_EQ_INT(target.arch, XR_CLI_TARGET_ARCH_THUMBV7EM);
    ASSERT_EQ_INT(target.os, XR_CLI_TARGET_OS_NONE);
    ASSERT_EQ_INT(target.abi, XR_CLI_TARGET_ABI_EABI);
    ASSERT_EQ_INT(target.endian, XR_CLI_TARGET_ENDIAN_LITTLE);
    ASSERT_EQ_INT(target.pointer_bits, 32);
    ASSERT_STR_EQ(xr_cli_build_target_default_output(&target), "a.out");
}

TEST(parse_windows_gnu_target) {
    XrCliBuildTarget target;
    char err[256];

    ASSERT_TRUE(xr_cli_build_target_parse("aarch64-windows-gnu", &target, err, sizeof(err)));
    ASSERT_FALSE(target.is_native);
    ASSERT_STR_EQ(target.name, "aarch64-windows-gnu");
    ASSERT_STR_EQ(target.zig_triple, "aarch64-windows-gnu");
    ASSERT_EQ_INT(target.arch, XR_CLI_TARGET_ARCH_AARCH64);
    ASSERT_EQ_INT(target.os, XR_CLI_TARGET_OS_WINDOWS);
    ASSERT_EQ_INT(target.abi, XR_CLI_TARGET_ABI_GNU);
    ASSERT_EQ_INT(target.endian, XR_CLI_TARGET_ENDIAN_LITTLE);
    ASSERT_EQ_INT(target.pointer_bits, 64);
    ASSERT_STR_EQ(xr_cli_build_target_default_output(&target), "a.exe");
}

TEST(reject_unknown_target) {
    XrCliBuildTarget target;
    char err[256];

    ASSERT_FALSE(xr_cli_build_target_parse("sparc64-plan9", &target, err, sizeof(err)));
}

TEST(resolve_auto_toolchain) {
    XrCliBuildTarget target;
    XrCliToolchainPlan plan;
    char err[256];

    ASSERT_TRUE(xr_cli_build_target_parse("x86_64-linux-musl", &target, err, sizeof(err)));
    ASSERT_TRUE(xr_cli_toolchain_resolve(XR_CLI_TOOLCHAIN_AUTO, &target, "cc", "/opt/zig", &plan,
                                         err, sizeof(err)));
    ASSERT_EQ_INT(plan.kind, XR_CLI_TOOLCHAIN_ZIG);
    ASSERT_STR_EQ(plan.program, "/opt/zig");

    ASSERT_TRUE(xr_cli_build_target_parse("native", &target, err, sizeof(err)));
    ASSERT_TRUE(xr_cli_toolchain_resolve(XR_CLI_TOOLCHAIN_AUTO, &target, "clang", NULL, &plan, err,
                                         sizeof(err)));
    ASSERT_EQ_INT(plan.kind, XR_CLI_TOOLCHAIN_HOST);
    ASSERT_STR_EQ(plan.program, "clang");
}

TEST(resolve_env_zig_toolchain) {
    XrCliBuildTarget target;
    XrCliToolchainPlan plan;
    char err[256];

    set_test_env("XRAY_ZIG", "/opt/xray-test/zig");
    ASSERT_TRUE(xr_cli_build_target_parse("x86_64-linux-musl", &target, err, sizeof(err)));
    ASSERT_TRUE(xr_cli_toolchain_resolve(XR_CLI_TOOLCHAIN_AUTO, &target, "cc", NULL, &plan, err,
                                         sizeof(err)));
    ASSERT_EQ_INT(plan.kind, XR_CLI_TOOLCHAIN_ZIG);
    ASSERT_STR_EQ(plan.program, "/opt/xray-test/zig");
    set_test_env("XRAY_ZIG", NULL);
}

TEST(resolve_bundled_zig_toolchain) {
#ifndef _WIN32
    char root_template[] = "/tmp/xray_tc_bundle_XXXXXX";
    char tools_dir[512];
    char zig_dir[512];
    char bin_dir[512];
    char zig_path[512];
    char xray_hint[512];
    XrCliBuildTarget target;
    XrCliToolchainPlan plan;
    char err[256];
    FILE *f;
    char *root;

    set_test_env("XRAY_ZIG", NULL);
    root = mkdtemp(root_template);
    ASSERT_NOT_NULL(root);
    snprintf(tools_dir, sizeof(tools_dir), "%s/tools", root);
    snprintf(zig_dir, sizeof(zig_dir), "%s/zig", tools_dir);
    snprintf(bin_dir, sizeof(bin_dir), "%s/bin", zig_dir);
    ASSERT_EQ_INT(mkdir(tools_dir, 0700), 0);
    ASSERT_EQ_INT(mkdir(zig_dir, 0700), 0);
    ASSERT_EQ_INT(mkdir(bin_dir, 0700), 0);
    snprintf(zig_path, sizeof(zig_path), "%s/zig", bin_dir);
    f = fopen(zig_path, "w");
    ASSERT_NOT_NULL(f);
    fclose(f);
    ASSERT_EQ_INT(chmod(zig_path, 0700), 0);
    snprintf(xray_hint, sizeof(xray_hint), "%s/xray", root);

    ASSERT_TRUE(xr_cli_build_target_parse("x86_64-linux-musl", &target, err, sizeof(err)));
    ASSERT_TRUE(xr_cli_toolchain_resolve_ex(XR_CLI_TOOLCHAIN_AUTO, &target, "cc", NULL, xray_hint,
                                            &plan, err, sizeof(err)));
    ASSERT_EQ_INT(plan.kind, XR_CLI_TOOLCHAIN_ZIG);
    ASSERT_STR_EQ(plan.program, zig_path);

    unlink(zig_path);
    rmdir(bin_dir);
    rmdir(zig_dir);
    rmdir(tools_dir);
    rmdir(root);
#endif
}

TEST(reject_cross_host_toolchain) {
    XrCliBuildTarget target;
    XrCliToolchainPlan plan;
    char err[256];

    ASSERT_TRUE(xr_cli_build_target_parse("x86_64-linux-musl", &target, err, sizeof(err)));
    ASSERT_FALSE(xr_cli_toolchain_resolve(XR_CLI_TOOLCHAIN_HOST, &target, "cc", NULL, &plan, err,
                                          sizeof(err)));
}

TEST(reject_cross_clang_toolchain) {
    XrCliBuildTarget target;
    XrCliToolchainPlan plan;
    char err[256];

    ASSERT_TRUE(xr_cli_build_target_parse("aarch64-linux-musl", &target, err, sizeof(err)));
    ASSERT_FALSE(xr_cli_toolchain_resolve(XR_CLI_TOOLCHAIN_CLANG, &target, "clang", NULL, &plan,
                                          err, sizeof(err)));
}

TEST(find_missing_executable) {
    char out[256];

    ASSERT_FALSE(xr_cli_toolchain_find_executable("/definitely/not/xray-zig", out, sizeof(out)));
}

TEST(build_zig_standalone_command) {
    XrCliBuildTarget target;
    XrCliToolchainPlan plan;
    XrCliToolchainCommand cmd;
    char err[256];

    ASSERT_TRUE(xr_cli_build_target_parse("x86_64-linux-musl", &target, err, sizeof(err)));
    ASSERT_TRUE(xr_cli_toolchain_resolve(XR_CLI_TOOLCHAIN_ZIG, &target, "cc", "/opt/zig", &plan,
                                         err, sizeof(err)));
    ASSERT_TRUE(xr_cli_toolchain_build_standalone(&plan, &target, "-O2", "app", "app.c",
                                                  "/include/aot", "/include/runtime", NULL, false,
                                                  &cmd, err, sizeof(err)));

    ASSERT_STR_EQ(cmd.program, "/opt/zig");
    ASSERT_STR_EQ(cmd.argv[0], "/opt/zig");
    ASSERT_STR_EQ(cmd.argv[1], "cc");
    ASSERT_STR_EQ(cmd.argv[2], "-target");
    ASSERT_STR_EQ(cmd.argv[3], "x86_64-linux-musl");
    ASSERT_STR_EQ(cmd.argv[4], "-O2");
    ASSERT_STR_EQ(cmd.argv[5], "-o");
    ASSERT_STR_EQ(cmd.argv[6], "app");
    ASSERT_STR_EQ(cmd.argv[7], "app.c");
    ASSERT_STR_EQ(cmd.argv[8], "-I/include/aot");
    ASSERT_STR_EQ(cmd.argv[9], "-I/include/runtime");
}

TEST(build_thumb_standalone_command_uses_cpu) {
    XrCliBuildTarget target;
    XrCliToolchainPlan plan;
    XrCliToolchainCommand cmd;
    char err[256];

    ASSERT_TRUE(xr_cli_build_target_parse("thumbv7em-none-eabi", &target, err, sizeof(err)));
    ASSERT_TRUE(xr_cli_toolchain_resolve(XR_CLI_TOOLCHAIN_ZIG, &target, "cc", "/opt/zig", &plan,
                                         err, sizeof(err)));
    ASSERT_TRUE(xr_cli_toolchain_build_standalone(&plan, &target, "-O2", "firmware.elf",
                                                  "firmware.c", "/include/aot", "/include/runtime",
                                                  NULL, false, &cmd, err, sizeof(err)));

    ASSERT_STR_EQ(cmd.argv[0], "/opt/zig");
    ASSERT_STR_EQ(cmd.argv[1], "cc");
    ASSERT_STR_EQ(cmd.argv[2], "-target");
    ASSERT_STR_EQ(cmd.argv[3], "thumb-freestanding-eabi");
    ASSERT_STR_EQ(cmd.argv[4], "-mcpu=cortex_m4");
    ASSERT_STR_EQ(cmd.argv[5], "-O2");
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("AOT Target Parser");
RUN_TEST(parse_native_target);
RUN_TEST(list_supported_targets);
RUN_TEST(parse_x86_64_none_target);
RUN_TEST(parse_riscv32_none_elf_target);
RUN_TEST(parse_riscv64_none_elf_target);
RUN_TEST(parse_thumbv7em_none_eabi_target);
RUN_TEST(parse_i386_linux_musl_target);
RUN_TEST(parse_linux_musl_target);
RUN_TEST(parse_powerpc64_linux_musl_target);
RUN_TEST(parse_windows_gnu_target);
RUN_TEST(reject_unknown_target);

RUN_TEST_SUITE("AOT Toolchain Planner");
RUN_TEST(resolve_auto_toolchain);
RUN_TEST(resolve_env_zig_toolchain);
RUN_TEST(resolve_bundled_zig_toolchain);
RUN_TEST(reject_cross_host_toolchain);
RUN_TEST(reject_cross_clang_toolchain);
RUN_TEST(find_missing_executable);
RUN_TEST(build_zig_standalone_command);
RUN_TEST(build_thumb_standalone_command_uses_cpu);
TEST_MAIN_END()
