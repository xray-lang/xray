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
#include "../test_win_compat.h"
#include "base/xmalloc.h"
#include "module/xproject.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

static char g_tmpdir[512];

static void setup_tmpdir(void) {
#ifdef _WIN32
    char base[MAX_PATH];
    DWORD length = GetTempPathA((DWORD) sizeof(base), base);
    ASSERT_TRUE(length > 0 && length < sizeof(base));
    snprintf(g_tmpdir, sizeof(g_tmpdir), "%sxray_test_project_XXXXXX", base);
#else
    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/xray_test_project_XXXXXX");
#endif
    ASSERT_NOT_NULL(xr_test_mkdtemp(g_tmpdir));
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
    f = fopen(path, "wb");
    ASSERT_NOT_NULL(f);
    fputs(content, f);
    fclose(f);
}

static void write_fixture_file(const char *name, const char *content) {
    char path[640];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", g_tmpdir, name);
    f = fopen(path, "wb");
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

TEST(load_verified_native_package_plan) {
    setup_tmpdir();
    write_fixture_file("oracle.c", "int x(void) { return 1; }\n");
    write_project_file(
        "[package]\n"
        "name = \"fixture\"\n"
        "version = \"1.0.0\"\n"
        "license = \"MIT\"\n"
        "\n"
        "[native]\n"
        "name = \"fixture-native\"\n"
        "version = \"1.0.0\"\n"
        "license = \"MIT\"\n"
        "source = \"vendored test fixture\"\n"
        "audit_mode = \"shipping\"\n"
        "vm = \"verified-dynamic\"\n"
        "\n"
        "[native.target.aarch64-apple-darwin]\n"
        "profile = \"release\"\n"
        "visibility = \"hidden\"\n"
        "system_links = [\"pthread\"]\n"
        "vm = \"verified-dynamic\"\n"
        "\n"
        "[[native.unit]]\n"
        "name = \"oracle\"\n"
        "kind = \"c\"\n"
        "sources = [\"oracle.c\"]\n"
        "source_hashes = [\"1a4799e20d310b20da93ba33a23ec2cef063f5d0dd46e0d6c650f8c1b0499c16\"]\n"
        "include_dirs = []\n"
        "defines = []\n"
        "system_links = []\n"
        "c_standard = \"c11\"\n"
        "optimization = \"release\"\n"
        "visibility = \"hidden\"\n"
        "warnings = \"strict\"\n"
        "output = \"liboracle.dylib\"\n"
        "purpose = \"test oracle\"\n"
        "\n"
        "[[native.symbol]]\n"
        "xray = \"raw.x\"\n"
        "native = \"x\"\n"
        "kind = \"function\"\n"
        "calling_convention = \"c\"\n"
        "unit = \"oracle\"\n"
        "\n"
        "[native.symbol.contract]\n"
        "params = []\n"
        "return = { ownership = \"value\", nullable = false, validity = \"i32\" }\n"
        "effects = [\"foreign\"]\n"
        "callbacks = []\n"
        "failure = \"none\"\n"
        "allocation = \"none\"\n"
        "blocking = \"never\"\n"
        "suspend = \"never\"\n"
        "io = \"none\"\n"
        "sync = \"none\"\n"
        "panic = \"never\"\n"
        "error = \"none\"\n");

    XrProject *project = xr_project_load(NULL, g_tmpdir);
    ASSERT_NOT_NULL(project);
    ASSERT_TRUE(project->initialized);
    ASSERT_NOT_NULL(project->native_plan);
    ASSERT_TRUE(project->native_plan->valid);
    ASSERT_EQ_INT(project->native_plan->unit_count, 1);
    ASSERT_EQ_INT(project->native_plan->symbol_count, 1);
    ASSERT_EQ_INT(project->native_plan->target_count, 1);
    ASSERT_STR_EQ(project->native_plan->targets[0].triple, "aarch64-apple-darwin");
    ASSERT_STR_EQ(project->native_plan->targets[0].profile, "release");
    ASSERT_EQ_INT(project->native_plan->targets[0].system_link_count, 1);
    const XrNativeSymbol *symbol = xr_native_package_find_symbol(project->native_plan, "x");
    ASSERT_NOT_NULL(symbol);
    ASSERT_STR_EQ(symbol->native_name, "x");
    ASSERT_TRUE(symbol->contract.complete);
    ASSERT_TRUE(strstr(xr_native_symbol_library(symbol), "liboracle.dylib") != NULL);
    xr_project_free(project);
    teardown_tmpdir();
}

TEST(native_package_unknown_target_policy_fails_closed) {
    setup_tmpdir();
    write_project_file("[project]\nname = \"bad-target\"\n"
                       "[native]\nname = \"bad-native\"\nversion = \"1\"\nlicense = \"MIT\"\n"
                       "source = \"fixture\"\naudit_mode = \"exploratory\"\nvm = \"unsupported\"\n"
                       "[native.target.aarch64-apple-darwin]\nprofile = \"release\"\n"
                       "shell = \"cc surprise.c\"\n");
    XrProject *project = xr_project_load(NULL, g_tmpdir);
    ASSERT_NOT_NULL(project);
    ASSERT_FALSE(project->initialized);
    ASSERT_NOT_NULL(project->native_plan);
    ASSERT_FALSE(project->native_plan->valid);
    ASSERT_TRUE(strstr(project->native_plan->error, "unsupported field") != NULL);
    xr_project_free(project);
    teardown_tmpdir();
}

TEST(native_package_hash_mismatch_fails_closed) {
    setup_tmpdir();
    write_fixture_file("oracle.c", "int x(void) { return 1; }\n");
    write_project_file(
        "[project]\nname = \"bad\"\n"
        "[native]\nname = \"bad-native\"\nversion = \"1\"\nlicense = \"MIT\"\n"
        "source = \"fixture\"\naudit_mode = \"exploratory\"\nvm = \"unsupported\"\n"
        "[[native.unit]]\nname = \"bad\"\nkind = \"c\"\nsources = [\"oracle.c\"]\n"
        "source_hashes = [\"0000000000000000000000000000000000000000000000000000000000000000\"]\n"
        "c_standard = \"c11\"\noptimization = \"none\"\nvisibility = \"hidden\"\n"
        "warnings = \"strict\"\npurpose = \"negative test\"\n");
    XrProject *project = xr_project_load(NULL, g_tmpdir);
    ASSERT_NOT_NULL(project);
    ASSERT_FALSE(project->initialized);
    ASSERT_NOT_NULL(project->native_plan);
    ASSERT_FALSE(project->native_plan->valid);
    ASSERT_TRUE(strstr(project->native_plan->error, "E-NATIVE-HASH-MISMATCH") != NULL);
    xr_project_free(project);
    teardown_tmpdir();
}

TEST(c_export_prefix_and_exclude_shape_manifest_roots) {
    setup_tmpdir();
    write_project_file("[project]\nname = \"exports\"\n"
                       "[[export.c]]\nxray = \"add\"\nsymbol = \"xr_add\"\n"
                       "visibility = \"default\"\nheader = true\n"
                       "[[export.c]]\nxray = \"sub\"\nsymbol = \"xr_sub\"\n"
                       "visibility = \"default\"\nheader = true\n"
                       "[[export.c]]\nxray = \"helper\"\nsymbol = \"xr_helper\"\n"
                       "visibility = \"hidden\"\n");
    XrProject *project = xr_project_load(NULL, g_tmpdir);
    ASSERT_NOT_NULL(project);
    ASSERT_TRUE(project->initialized);
    ASSERT_NOT_NULL(project->native_plan);
    ASSERT_EQ_INT(project->native_plan->export_count, 3);
    uint64_t original_fingerprint = project->native_plan->fingerprint;
    char error[256];
    ASSERT_FALSE(xr_native_package_configure_c_exports(project->native_plan, "NS_", "xr_missing",
                                                       error, sizeof(error)));
    ASSERT_TRUE(strstr(error, "unknown C export symbol") != NULL);
    ASSERT_EQ_INT(project->native_plan->export_count, 3);

    ASSERT_TRUE(xr_native_package_configure_c_exports(project->native_plan, "NS_", "xr_sub", error,
                                                      sizeof(error)));
    ASSERT_EQ_INT(project->native_plan->export_count, 2);
    ASSERT_STR_EQ(project->native_plan->exports[0].symbol, "NS_xr_add");
    ASSERT_STR_EQ(project->native_plan->exports[1].symbol, "xr_helper");
    ASSERT_TRUE(project->native_plan->fingerprint != original_fingerprint);
    ASSERT_NULL(xr_native_package_find_export(project->native_plan, "sub"));
    ASSERT_NOT_NULL(xr_native_package_find_export(project->native_plan, "add"));

    xr_project_free(project);
    teardown_tmpdir();
}

TEST(module_identity_authority_is_manifest_typed) {
    setup_tmpdir();
    XrModuleIdentityAuthority authority = {0};
    char *namespace_id = NULL;
    char *physical_root = NULL;
    XrProject project = {
        .root = g_tmpdir,
        .name = "identity-project",
    };

    ASSERT_TRUE(xr_project_module_identity_authority(
        &project, &authority, &namespace_id, &physical_root));
    ASSERT_EQ_INT(authority.kind, XR_MODULE_IDENTITY_PROJECT);
    ASSERT_STR_EQ(authority.namespace_id, "identity-project");
    ASSERT_TRUE(authority.physical_root[0] != '\0');
    xr_free(namespace_id);
    xr_free(physical_root);

    project.is_package = true;
    project.name = "xray/identity-package";
    project.version = "1.2.3-beta.1+build.7";
    ASSERT_TRUE(xr_project_module_identity_authority(
        &project, &authority, &namespace_id, &physical_root));
    ASSERT_EQ_INT(authority.kind, XR_MODULE_IDENTITY_PACKAGE);
    ASSERT_STR_EQ(authority.namespace_id,
                  "xray/identity-package@1.2.3-beta.1+build.7");
    ASSERT_TRUE(authority.physical_root[0] != '\0');
    xr_free(namespace_id);
    xr_free(physical_root);

    const struct {
        bool is_package;
        const char *name;
        const char *version;
    } rejected[] = {
        {false, NULL, NULL},
        {false, "xray/project", NULL},
        {true, "xray/package", NULL},
        {true, "package", "1.0.0"},
        {true, "xray/package", "^1.0.0"},
        {true, "xray/package", "not-a-version"},
    };
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        project.is_package = rejected[i].is_package;
        project.name = (char *) rejected[i].name;
        project.version = (char *) rejected[i].version;
        ASSERT_FALSE(xr_project_module_identity_authority(
            &project, &authority, &namespace_id, &physical_root));
        ASSERT_NULL(namespace_id);
        ASSERT_NULL(physical_root);
    }

    teardown_tmpdir();
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Project Configuration");
RUN_TEST(load_target_config);
RUN_TEST(load_verified_native_package_plan);
RUN_TEST(native_package_hash_mismatch_fails_closed);
RUN_TEST(native_package_unknown_target_policy_fails_closed);
RUN_TEST(c_export_prefix_and_exclude_shape_manifest_roots);
RUN_TEST(module_identity_authority_is_manifest_typed);
TEST_MAIN_END()
