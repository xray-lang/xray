/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_cli_toolchain.c - Toolchain model, discovery, and process tests
 */

#include "../test_framework.h"
#include "app/toolchain/xtc_discovery.h"
#include "app/toolchain/xtc_command.h"
#include "app/toolchain/xtc_config.h"
#include "app/toolchain/xtc_json.h"
#include "app/toolchain/xtc_model.h"
#include "app/toolchain/xtc_process.h"
#include "app/toolchain/xtc_probe_cache.h"
#include "base/xjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

typedef struct CommandCapture {
    char args[48][256];
    size_t count;
} CommandCapture;

static bool command_capture_add(void *context, const char *arg, char *err, size_t err_size) {
    CommandCapture *capture = context;
    if (!capture || !arg || capture->count >= 48 || strlen(arg) >= sizeof(capture->args[0])) {
        if (err && err_size)
            snprintf(err, err_size, "capture overflow");
        return false;
    }
    snprintf(capture->args[capture->count++], sizeof(capture->args[0]), "%s", arg);
    return true;
}

static bool command_capture_joined(void *context, const char *prefix, const char *value, char *err,
                                   size_t err_size) {
    CommandCapture *capture = context;
    if (!capture || capture->count >= 48 ||
        snprintf(capture->args[capture->count], sizeof(capture->args[0]), "%s%s",
                 prefix ? prefix : "", value ? value : "") >= (int) sizeof(capture->args[0])) {
        if (err && err_size)
            snprintf(err, err_size, "capture overflow");
        return false;
    }
    capture->count++;
    return true;
}

static bool command_capture_has(const CommandCapture *capture, const char *arg) {
    for (size_t i = 0; capture && i < capture->count; i++) {
        if (strcmp(capture->args[i], arg) == 0)
            return true;
    }
    return false;
}

TEST(semantic_command_plan_maps_gnu_and_msvc_dialects) {
    XrToolchainSelection selection = {0};
    XrNativeCompileSpec compile = {0};
    XrNativeLinkSpec link = {0};
    CommandCapture capture = {0};
    XrToolchainArgSink sink = {&capture, command_capture_add, command_capture_joined};
    char err[256];

    selection.provider = XR_TOOLCHAIN_PROVIDER_GCC;
    selection.program = selection.program_storage;
    snprintf(selection.program_storage, sizeof(selection.program_storage), "%s", "/usr/bin/gcc");
    ASSERT_TRUE(xtc_target_parse("x86_64-linux-gnu", &selection.target, err, sizeof(err)));
    compile.optimization = XR_OPTIMIZATION_SPEED;
    compile.fp_contract = XR_FP_CONTRACT_OFF;
    compile.lto = true;
    compile.pic = true;
    compile.function_sections = true;
    compile.data_sections = true;
    compile.visibility = XR_VISIBILITY_HIDDEN;
    compile.language_standard = "c11";
    ASSERT_TRUE(xtc_command_emit_driver(&selection, &selection.target, &sink, err, sizeof(err)));
    ASSERT_TRUE(xtc_command_emit_compile(&selection, &compile, &sink, err, sizeof(err)));
    ASSERT_TRUE(command_capture_has(&capture, "/usr/bin/gcc"));
    ASSERT_TRUE(command_capture_has(&capture, "-O3"));
    ASSERT_TRUE(command_capture_has(&capture, "-ffp-contract=off"));
    ASSERT_TRUE(command_capture_has(&capture, "-flto"));
    ASSERT_TRUE(command_capture_has(&capture, "-fPIC"));
    ASSERT_TRUE(command_capture_has(&capture, "-std=c11"));

    memset(&capture, 0, sizeof(capture));
    memset(&selection, 0, sizeof(selection));
    selection.provider = XR_TOOLCHAIN_PROVIDER_MSVC;
    selection.program = selection.program_storage;
    snprintf(selection.program_storage, sizeof(selection.program_storage), "%s", "cl.exe");
    ASSERT_TRUE(xtc_target_parse("x86_64-windows-msvc", &selection.target, err, sizeof(err)));
    compile.pic = false;
    compile.visibility = XR_VISIBILITY_DEFAULT;
    link.shared = true;
    link.dead_strip = true;
    link.lto = true;
    link.entry = "mainCRTStartup";
    ASSERT_TRUE(xtc_command_emit_driver(&selection, &selection.target, &sink, err, sizeof(err)));
    ASSERT_TRUE(xtc_command_emit_compile(&selection, &compile, &sink, err, sizeof(err)));
    ASSERT_TRUE(
        xtc_command_emit_link(&selection, &selection.target, &link, &sink, err, sizeof(err)));
    ASSERT_TRUE(command_capture_has(&capture, "/O2"));
    ASSERT_TRUE(command_capture_has(&capture, "/fp:strict"));
    ASSERT_TRUE(command_capture_has(&capture, "/GL"));
    ASSERT_TRUE(command_capture_has(&capture, "/LD"));
    ASSERT_TRUE(command_capture_has(&capture, "/LTCG"));
    ASSERT_TRUE(command_capture_has(&capture, "/OPT:REF"));
    ASSERT_TRUE(command_capture_has(&capture, "/ENTRY:mainCRTStartup"));
    ASSERT_FALSE(command_capture_has(&capture, "-O2"));
}

TEST(msvc_command_plan_fails_closed_for_gnu_only_intent) {
    XrToolchainSelection selection = {0};
    XrNativeCompileSpec compile = {0};
    CommandCapture capture = {0};
    XrToolchainArgSink sink = {&capture, command_capture_add, command_capture_joined};
    char err[256];
    selection.provider = XR_TOOLCHAIN_PROVIDER_MSVC;
    selection.program = selection.program_storage;
    snprintf(selection.program_storage, sizeof(selection.program_storage), "%s", "cl.exe");
    compile.pic = true;
    ASSERT_FALSE(xtc_command_emit_compile(&selection, &compile, &sink, err, sizeof(err)));
    ASSERT_TRUE(strstr(err, "unsupported by MSVC") != NULL);
}

TEST(parse_native_target_normalizes_abi) {
    XrToolchainTarget target;
    char err[256];

    ASSERT_TRUE(xtc_target_parse(NULL, &target, err, sizeof(err)));
    ASSERT_TRUE(target.is_native);
    ASSERT_STR_EQ(target.requested_name, "native");
    ASSERT_TRUE(strstr(target.name, "-apple-darwin") != NULL ||
                strstr(target.name, "-linux-gnu") != NULL ||
                strstr(target.name, "-windows-msvc") != NULL);
    ASSERT_EQ_INT(target.pointer_bits, 64);
}

TEST(list_uses_canonical_targets_only) {
    size_t count = 0;
    const char *const *targets = xtc_target_supported_names(&count);

    ASSERT_NOT_NULL(targets);
    ASSERT_EQ_INT((int) count, 14);
    ASSERT_STR_EQ(targets[0], "native");
    ASSERT_STR_EQ(targets[1], "aarch64-apple-darwin");
    ASSERT_STR_EQ(targets[3], "x86_64-linux-gnu");
    ASSERT_STR_EQ(targets[10], "x86_64-freestanding-none");
    ASSERT_STR_EQ(targets[12], "riscv64-freestanding-none");
    ASSERT_STR_EQ(targets[13], "thumb-freestanding-eabi");
}

TEST(parse_linux_musl_target) {
    XrToolchainTarget target;
    char err[256];

    ASSERT_TRUE(xtc_target_parse("x86_64-linux-musl", &target, err, sizeof(err)));
    ASSERT_STR_EQ(target.name, "x86_64-linux-musl");
    ASSERT_STR_EQ(target.zig_triple, "x86_64-linux-musl");
    ASSERT_EQ_INT(target.arch, XR_TOOLCHAIN_TARGET_ARCH_X86_64);
    ASSERT_EQ_INT(target.os, XR_TOOLCHAIN_TARGET_OS_LINUX);
    ASSERT_EQ_INT(target.abi, XR_TOOLCHAIN_TARGET_ABI_MUSL);
    ASSERT_EQ_INT(target.endian, XR_TOOLCHAIN_TARGET_ENDIAN_LITTLE);
    ASSERT_STR_EQ(xtc_target_default_output(&target), "a.out");
}

TEST(parse_freestanding_targets) {
    XrToolchainTarget target;
    char err[256];

    ASSERT_TRUE(xtc_target_parse("x86_64-freestanding-none", &target, err, sizeof(err)));
    ASSERT_EQ_INT(target.os, XR_TOOLCHAIN_TARGET_OS_NONE);
    ASSERT_EQ_INT(target.abi, XR_TOOLCHAIN_TARGET_ABI_NONE);
    ASSERT_FALSE(xtc_target_is_hosted(&target));

    ASSERT_TRUE(xtc_target_parse("riscv32-freestanding-none", &target, err, sizeof(err)));
    ASSERT_EQ_INT(target.arch, XR_TOOLCHAIN_TARGET_ARCH_RISCV32);
    ASSERT_EQ_INT(target.pointer_bits, 32);

    ASSERT_TRUE(xtc_target_parse("riscv64-freestanding-none", &target, err, sizeof(err)));
    ASSERT_EQ_INT(target.arch, XR_TOOLCHAIN_TARGET_ARCH_RISCV64);
    ASSERT_EQ_INT(target.pointer_bits, 64);

    ASSERT_TRUE(xtc_target_parse("thumb-freestanding-eabi", &target, err, sizeof(err)));
    ASSERT_EQ_INT(target.arch, XR_TOOLCHAIN_TARGET_ARCH_THUMB);
    ASSERT_EQ_INT(target.abi, XR_TOOLCHAIN_TARGET_ABI_EABI);
    ASSERT_STR_EQ(target.cpu, "cortex_m4");
}

TEST(reject_retired_target_aliases) {
    XrToolchainTarget target;
    char err[256];

    ASSERT_FALSE(xtc_target_parse("x86_64-unknown-none", &target, err, sizeof(err)));
    ASSERT_FALSE(xtc_target_parse("riscv32imac-unknown-none-elf", &target, err, sizeof(err)));
    ASSERT_FALSE(xtc_target_parse("riscv64gc-unknown-none-elf", &target, err, sizeof(err)));
    ASSERT_FALSE(xtc_target_parse("thumbv7em-none-eabi", &target, err, sizeof(err)));
}

TEST(selector_and_provider_names_are_stable) {
    XrToolchainSelector selector;
    char err[256];

    ASSERT_TRUE(xtc_selector_parse("auto", &selector, err, sizeof(err)));
    ASSERT_EQ_INT(selector, XR_TOOLCHAIN_SELECTOR_AUTO);
    ASSERT_TRUE(xtc_selector_parse("host", &selector, err, sizeof(err)));
    ASSERT_TRUE(xtc_selector_parse("clang", &selector, err, sizeof(err)));
    ASSERT_TRUE(xtc_selector_parse("gcc", &selector, err, sizeof(err)));
    ASSERT_TRUE(xtc_selector_parse("msvc", &selector, err, sizeof(err)));
    ASSERT_TRUE(xtc_selector_parse("zig", &selector, err, sizeof(err)));
    ASSERT_FALSE(xtc_selector_parse("cc", &selector, err, sizeof(err)));
    ASSERT_FALSE(xtc_selector_parse("zig-cc", &selector, err, sizeof(err)));
    ASSERT_STR_EQ(xtc_provider_name(XR_TOOLCHAIN_PROVIDER_APPLE_CLANG), "apple-clang");
    ASSERT_STR_EQ(xtc_provider_name(XR_TOOLCHAIN_PROVIDER_LLVM_CLANG), "llvm-clang");
    ASSERT_STR_EQ(xtc_reason_code_name(XR_TOOLCHAIN_REASON_LINK_PROBE_FAILED), "LINK_PROBE_FAILED");
}

TEST(find_missing_executable) {
    char out[256];
    ASSERT_FALSE(xtc_find_executable("/definitely/not/xray-provider", out, sizeof(out)));
}

TEST(cross_target_rejects_explicit_host_without_fallback) {
    XrToolchainRequest request = {0};
    XrToolchainCandidates candidates;
    char err[256];

    ASSERT_TRUE(xtc_target_parse("x86_64-linux-musl", &request.target, err, sizeof(err)));
    request.selector = XR_TOOLCHAIN_SELECTOR_HOST;
    ASSERT_FALSE(xtc_discover_candidates(&request, &candidates, err, sizeof(err)));
}

#ifndef _WIN32
static bool write_executable(const char *path, const char *content) {
    FILE *file = fopen(path, "wb");
    if (!file)
        return false;
    bool ok = fwrite(content, 1, strlen(content), file) == strlen(content) && fclose(file) == 0;
    return ok && chmod(path, 0700) == 0;
}
#endif

TEST(explicit_provider_has_no_fallback) {
#ifndef _WIN32
    char root_template[] = "/tmp/xray_xtc_explicit_XXXXXX";
    char fake_clang[512];
    char err[256];
    XrToolchainRequest request = {0};
    XrToolchainSelection selection;
    char *root = mkdtemp(root_template);
    ASSERT_NOT_NULL(root);
    snprintf(fake_clang, sizeof(fake_clang), "%s/fake-clang", root);
    ASSERT_TRUE(write_executable(fake_clang, "#!/bin/sh\nprintf 'Apple clang version 99.1\\n'\n"));
    ASSERT_TRUE(xtc_target_parse("native", &request.target, err, sizeof(err)));
    request.selector = XR_TOOLCHAIN_SELECTOR_GCC;
    request.cc = fake_clang;
    ASSERT_FALSE(xtc_select_discovered(&request, &selection, err, sizeof(err)));
    ASSERT_EQ_INT(selection.reason, XR_TOOLCHAIN_REASON_PROVIDER_EXPLICIT_NO_FALLBACK);
    unlink(fake_clang);
    rmdir(root);
#endif
}

TEST(explicit_clang_is_classified_from_banner) {
#ifndef _WIN32
    char root_template[] = "/tmp/xray_xtc_clang_XXXXXX";
    char fake_clang[512];
    char err[256];
    XrToolchainRequest request = {0};
    XrToolchainSelection selection;
    char canonical[512];
    char *root = mkdtemp(root_template);
    ASSERT_NOT_NULL(root);
    snprintf(fake_clang, sizeof(fake_clang), "%s/fake-clang", root);
    ASSERT_TRUE(write_executable(fake_clang, "#!/bin/sh\nprintf 'clang version 88.0.0\\n'\n"));
    ASSERT_TRUE(xtc_target_parse("native", &request.target, err, sizeof(err)));
    request.selector = XR_TOOLCHAIN_SELECTOR_CLANG;
    request.cc = fake_clang;
    ASSERT_TRUE(xtc_select_discovered(&request, &selection, err, sizeof(err)));
    ASSERT_EQ_INT(selection.provider, XR_TOOLCHAIN_PROVIDER_LLVM_CLANG);
    ASSERT_EQ_INT(selection.readiness, XR_TOOLCHAIN_RUNNABLE);
    ASSERT_TRUE(xtc_find_executable(fake_clang, canonical, sizeof(canonical)));
    ASSERT_STR_EQ(selection.program, canonical);
    unlink(fake_clang);
    rmdir(root);
#endif
}

TEST(config_update_is_atomic_and_round_trips_escaped_paths) {
#ifndef _WIN32
    char root_template[] = "/tmp/xray_xtc_config_XXXXXX";
    char config_path[512];
    char lock_path[540];
    char err[256];
    const char *compiler = "/tmp/compiler-\"quoted\"-\\path";
    XrToolchainConfig config;
    bool exists = false;
    char *root = mkdtemp(root_template);
    ASSERT_NOT_NULL(root);
    snprintf(config_path, sizeof(config_path), "%s/toolchains.toml", root);
    snprintf(lock_path, sizeof(lock_path), "%s.lock", config_path);

    ASSERT_TRUE(xtc_config_use(config_path, "native", XR_TOOLCHAIN_SELECTOR_CLANG, compiler, NULL,
                               err, sizeof(err)));
    ASSERT_TRUE(xtc_config_load(config_path, &config, &exists, err, sizeof(err)));
    ASSERT_TRUE(exists);
    ASSERT_TRUE(config.has_native);
    ASSERT_EQ_INT(config.native.selector, XR_TOOLCHAIN_SELECTOR_CLANG);
    ASSERT_STR_EQ(config.native.compiler, compiler);

    ASSERT_TRUE(xtc_config_reset(config_path, "native", err, sizeof(err)));
    ASSERT_TRUE(xtc_config_load(config_path, &config, &exists, err, sizeof(err)));
    ASSERT_FALSE(config.has_native);
    ASSERT_TRUE(xtc_config_reset(config_path, NULL, err, sizeof(err)));
    ASSERT_FALSE(xtc_config_load(config_path, &config, &exists, err, sizeof(err)) && exists);
    unlink(lock_path);
    rmdir(root);
#endif
}

TEST(config_corruption_is_preserved) {
#ifndef _WIN32
    char root_template[] = "/tmp/xray_xtc_corrupt_XXXXXX";
    char config_path[512];
    char lock_path[540];
    char err[256];
    char content[64] = {0};
    char *root = mkdtemp(root_template);
    ASSERT_NOT_NULL(root);
    snprintf(config_path, sizeof(config_path), "%s/toolchains.toml", root);
    snprintf(lock_path, sizeof(lock_path), "%s.lock", config_path);
    ASSERT_TRUE(write_executable(config_path, "schema = [broken\n"));
    ASSERT_FALSE(xtc_config_use(config_path, "native", XR_TOOLCHAIN_SELECTOR_HOST, NULL, NULL, err,
                                sizeof(err)));
    FILE *file = fopen(config_path, "rb");
    ASSERT_NOT_NULL(file);
    ASSERT_TRUE(fread(content, 1, sizeof(content) - 1, file) > 0);
    fclose(file);
    ASSERT_STR_EQ(content, "schema = [broken\n");
    unlink(config_path);
    unlink(lock_path);
    rmdir(root);
#endif
}

TEST(process_capture_and_output_limit) {
#ifndef _WIN32
    char shell[1200];
    char err[256];
    XrProcessSpec spec;
    XrProcessResult result;
    ASSERT_TRUE(xtc_find_executable("sh", shell, sizeof(shell)));
    xtc_process_spec_init(&spec, shell, 5000);
    spec.argv[1] = "-c";
    spec.argv[2] = "printf 123456789; printf abcdefghi >&2";
    spec.argv[3] = NULL;
    spec.output_limit = 5;
    ASSERT_TRUE(xtc_process_run(&spec, &result, err, sizeof(err)));
    ASSERT_EQ_INT(result.exit_code, 0);
    ASSERT_STR_EQ(result.stdout_data, "12345");
    ASSERT_STR_EQ(result.stderr_data, "abcde");
    ASSERT_TRUE(result.output_truncated);
    xtc_process_result_free(&result);
#endif
}

TEST(process_timeout_is_bounded) {
#ifndef _WIN32
    char shell[1200];
    char err[256];
    XrProcessSpec spec;
    XrProcessResult result;
    ASSERT_TRUE(xtc_find_executable("sh", shell, sizeof(shell)));
    xtc_process_spec_init(&spec, shell, 30);
    spec.argv[1] = "-c";
    spec.argv[2] = "sleep 5";
    spec.argv[3] = NULL;
    ASSERT_TRUE(xtc_process_run(&spec, &result, err, sizeof(err)));
    ASSERT_TRUE(result.timed_out);
    ASSERT_TRUE(result.duration_ms < 2000);
    xtc_process_result_free(&result);
#endif
}

TEST(probe_json_schema_v1_is_valid_and_escaped) {
    XrToolchainProbeOptions options = {0};
    XrToolchainProbeResult result = {0};
    char err[256];
    ASSERT_TRUE(xtc_target_parse("native", &options.request.target, err, sizeof(err)));
    options.request.selector = XR_TOOLCHAIN_SELECTOR_AUTO;
    options.profile = XR_TOOLCHAIN_PROFILE_HOSTED;
    result.selection.provider = XR_TOOLCHAIN_PROVIDER_LLVM_CLANG;
    result.selection.ownership = XR_TOOLCHAIN_OWNERSHIP_EXTERNAL;
    result.selection.program = result.selection.program_storage;
    snprintf(result.selection.program_storage, sizeof(result.selection.program_storage), "%s",
             "/tmp/clang with spaces");
    snprintf(result.selection.version, sizeof(result.selection.version), "%s",
             "clang \"test\"\nline");
    snprintf(result.selection.runtime_artifact, sizeof(result.selection.runtime_artifact), "%s",
             "xray-rt-coro-test-v1");
    snprintf(result.selection.probe_fingerprint, sizeof(result.selection.probe_fingerprint), "%s",
             "sha256:test");
    snprintf(result.runtime.sdk_digest, sizeof(result.runtime.sdk_digest), "%s", "sha256:sdk");
    result.selection.readiness = XR_TOOLCHAIN_READY;
    result.c_compile = XR_TOOLCHAIN_CAPABILITY_OK;
    result.sdk_compile = XR_TOOLCHAIN_CAPABILITY_OK;
    result.runtime_link = XR_TOOLCHAIN_CAPABILITY_OK;
    result.native_run = XR_TOOLCHAIN_CAPABILITY_OK;
    result.cross = XR_TOOLCHAIN_CAPABILITY_UNSUPPORTED;
    result.lto = XR_TOOLCHAIN_CAPABILITY_OK;
    snprintf(result.cache, sizeof(result.cache), "%s", "miss");
    result.diagnostic_count = 1;
    result.diagnostics[0].code = XR_TOOLCHAIN_REASON_EXTERNAL_INSTALL_REQUIRED;
    snprintf(result.diagnostics[0].stage, sizeof(result.diagnostics[0].stage), "%s", "discover");
    snprintf(result.diagnostics[0].message, sizeof(result.diagnostics[0].message), "%s",
             "install \"tools\"");

    FILE *file = tmpfile();
    ASSERT_NOT_NULL(file);
    ASSERT_TRUE(xtc_probe_json_write(file, "native", &options, &result, true, "1.2.3", "abc"));
    ASSERT_TRUE(fflush(file) == 0);
    ASSERT_TRUE(fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    ASSERT_TRUE(length > 0);
    ASSERT_TRUE(fseek(file, 0, SEEK_SET) == 0);
    char *json = malloc((size_t) length + 1);
    ASSERT_NOT_NULL(json);
    ASSERT_EQ_INT((int) fread(json, 1, (size_t) length, file), (int) length);
    json[length] = '\0';
    fclose(file);

    XrJsonValue *doc = xjson_parse(json, (size_t) length);
    ASSERT_NOT_NULL(doc);
    ASSERT_EQ_INT((int) xjson_get_int(doc, "schema"), 1);
    XrJsonValue *selection = xjson_get_object(doc, "selection");
    ASSERT_NOT_NULL(selection);
    ASSERT_STR_EQ(xjson_get_string(selection, "provider"), "llvm-clang");
    ASSERT_STR_EQ(xjson_get_string(selection, "version"), "clang \"test\"\nline");
    ASSERT_TRUE(xjson_get_bool(selection, "ready"));
    XrJsonValue *capabilities = xjson_get_object(doc, "capabilities");
    ASSERT_NOT_NULL(capabilities);
    ASSERT_STR_EQ(xjson_get_string(capabilities, "runtimeLink"), "ok");
    xjson_free(doc);
    free(json);
}

TEST(probe_cache_list_reports_recent_success) {
#ifndef _WIN32
    char root_template[] = "/tmp/xray_xtc_cache_XXXXXX";
    char err[256];
    XrToolchainProbeOptions options = {0};
    XrToolchainProbeResult result = {0};
    XrToolchainProbeCacheEntry entries[4];
    size_t count = 0;
    char *root = mkdtemp(root_template);
    ASSERT_NOT_NULL(root);
    ASSERT_TRUE(setenv("XDG_CACHE_HOME", root, 1) == 0);
    ASSERT_TRUE(xtc_target_parse("native", &options.request.target, err, sizeof(err)));
    options.request.selector = XR_TOOLCHAIN_SELECTOR_HOST;
    options.profile = XR_TOOLCHAIN_PROFILE_HOSTED;
    result.selection.readiness = XR_TOOLCHAIN_READY;
    result.selection.provider = XR_TOOLCHAIN_PROVIDER_LLVM_CLANG;
    snprintf(result.selection.probe_fingerprint, sizeof(result.selection.probe_fingerprint), "%s",
             "sha256:probe");
    snprintf(result.selection.compiler_fingerprint, sizeof(result.selection.compiler_fingerprint),
             "%s", "sha256:compiler");
    snprintf(result.selection.runtime_artifact, sizeof(result.selection.runtime_artifact), "%s",
             "xray-rt-coro-test-v1");
    result.c_compile = XR_TOOLCHAIN_CAPABILITY_OK;
    result.sdk_compile = XR_TOOLCHAIN_CAPABILITY_OK;
    result.runtime_link = XR_TOOLCHAIN_CAPABILITY_OK;
    result.native_run = XR_TOOLCHAIN_CAPABILITY_OK;
    result.cross = XR_TOOLCHAIN_CAPABILITY_UNSUPPORTED;
    result.lto = XR_TOOLCHAIN_CAPABILITY_OK;

    ASSERT_TRUE(xtc_probe_cache_store(&options, &result, err, sizeof(err)));
    ASSERT_TRUE(
        xtc_probe_cache_list(options.request.target.name, entries, 4, &count, err, sizeof(err)));
    ASSERT_EQ_INT((int) count, 1);
    ASSERT_STR_EQ(entries[0].provider, "llvm-clang");
    ASSERT_STR_EQ(entries[0].fingerprint, "sha256:probe");
    ASSERT_TRUE(entries[0].ready);
    ASSERT_TRUE(xtc_probe_cache_reset(NULL, err, sizeof(err)));
    ASSERT_TRUE(unsetenv("XDG_CACHE_HOME") == 0);
    char path[512];
    snprintf(path, sizeof(path), "%s/xray/toolchain/probes", root);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/xray/toolchain", root);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/xray", root);
    rmdir(path);
    rmdir(root);
#endif
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Toolchain model");
RUN_TEST(parse_native_target_normalizes_abi);
RUN_TEST(list_uses_canonical_targets_only);
RUN_TEST(parse_linux_musl_target);
RUN_TEST(parse_freestanding_targets);
RUN_TEST(reject_retired_target_aliases);
RUN_TEST(selector_and_provider_names_are_stable);
RUN_TEST(semantic_command_plan_maps_gnu_and_msvc_dialects);
RUN_TEST(msvc_command_plan_fails_closed_for_gnu_only_intent);

RUN_TEST_SUITE("Toolchain discovery");
RUN_TEST(find_missing_executable);
RUN_TEST(cross_target_rejects_explicit_host_without_fallback);
RUN_TEST(explicit_provider_has_no_fallback);
RUN_TEST(explicit_clang_is_classified_from_banner);

RUN_TEST_SUITE("Toolchain config");
RUN_TEST(config_update_is_atomic_and_round_trips_escaped_paths);
RUN_TEST(config_corruption_is_preserved);

RUN_TEST_SUITE("Toolchain process runner");
RUN_TEST(process_capture_and_output_limit);
RUN_TEST(process_timeout_is_bounded);

RUN_TEST_SUITE("Toolchain JSON");
RUN_TEST(probe_json_schema_v1_is_valid_and_escaped);
RUN_TEST(probe_cache_list_reports_recent_success);
TEST_MAIN_END()
