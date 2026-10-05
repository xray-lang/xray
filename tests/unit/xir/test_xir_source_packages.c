/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_packages.c - Locked package inputs and detached artifact execution
 */
#include "xir/xxir_source.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "module/xlockfile.h"
#include "module/xdeclaration_load.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"

static XrCompileResourceStats manifest_load_case(const char *root, uint32_t expected_records,
    uint64_t work, bool fail) {
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(64)*1024*1024, work);
    XrDeclarationInputLimits limits = {{SIZE_MAX - 1, 128}, {context->limits.functions, context->limits.parameters}};
    XrDeclarationManifest *manifest = NULL;
    XrDeclarationStatus status = xr_compile_declaration_manifest_load(context->resources, root, &limits, &manifest);
    CHECK(status == (fail ? XR_DECLARATION_LIMIT : XR_DECLARATION_OK));
    if (fail) CHECK(!manifest);
    else {
        CHECK(manifest && manifest->count == expected_records);
        CHECK(!strcmp(manifest->records[0].module, expected_records == 3 ? "root.xr" : "main.xr"));
        CHECK(!strcmp(manifest->records[0].name, expected_records == 3 ? "answer" : "value"));
        CHECK(manifest->records[0].no_suspend);
        if (expected_records == 3) {
            CHECK(!strcmp(manifest->records[1].name, "unreachable"));
            CHECK(!strcmp(manifest->records[2].name, "apply") && manifest->records[2].parameter_count == 1);
            CHECK(!strcmp(manifest->records[2].parameters[0], "callback"));
        }
    }
    XrCompileResourceStats stats = {0};
    CHECK(xr_compile_resources_stats(context->resources, &stats) == XR_COMPILE_RESOURCE_OK);
    xr_compile_declaration_manifest_free(manifest);
    return stats;
}
static void manifest_budget_cases(const char *root) {
    char package[4096]; int length = snprintf(package, sizeof(package), "%s/.xray/packages/fixture/math/1.0.0", root);
    CHECK(length > 0 && (size_t)length < sizeof(package));
    const XrCompileResourceStats project_fee = manifest_load_case(root, 3, UINT64_C(128000000), false);
    const XrCompileResourceStats package_fee = manifest_load_case(package, 1, UINT64_C(128000000), false);
    CHECK(project_fee.work > 196608 && package_fee.work > 196608);
    CHECK(manifest_load_case(root, 3, project_fee.work, false).work == project_fee.work);
    manifest_load_case(root, 3, project_fee.work - 1, true);
    CHECK(manifest_load_case(package, 1, package_fee.work, false).work == package_fee.work);
    manifest_load_case(package, 1, package_fee.work - 1, true);
    uint64_t allowance = project_fee.work > package_fee.work ? project_fee.work : package_fee.work;
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(64)*1024*1024, allowance);
    XrDeclarationInputLimits limits = {{SIZE_MAX - 1, 128}, {context->limits.functions, context->limits.parameters}};
    XrCompileResourceStats baseline = {0};
    CHECK(xr_compile_resources_stats(context->resources, &baseline) == XR_COMPILE_RESOURCE_OK);
    XrDeclarationManifest *first = NULL, *second = NULL;
    CHECK(xr_compile_declaration_manifest_load(context->resources, root, &limits, &first) == XR_DECLARATION_OK);
    CHECK(first && first->count == 3);
    xr_compile_declaration_manifest_free(first);
    CHECK(xr_compile_declaration_manifest_load(context->resources, package, &limits, &second) == XR_DECLARATION_LIMIT);
    CHECK(!second);
    XrCompileResourceStats stats = {0};
    CHECK(xr_compile_resources_stats(context->resources, &stats) == XR_COMPILE_RESOURCE_OK);
    CHECK(stats.work <= allowance && stats.live_bytes == baseline.live_bytes);
    fprintf(stderr, "Package manifests independent work=%llu/%llu exact/minus1; same-ledger two loads LIMIT cap=%llu used=%llu\n",
        (unsigned long long)project_fee.work, (unsigned long long)package_fee.work,
        (unsigned long long)allowance, (unsigned long long)stats.work);
}

static void reject(const XrXirSourceRequest *request, const char *message) {
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_check(request, &result, &diagnostic, NULL);
    fprintf(stderr, "Package rejection expected=%s status=%u checked=%p snapshot=%p message=%s\n", message, status, (void *)result.checked, (void *)result.snapshot, diagnostic.message);
    CHECK(status == XR_XIR_BAD_STRUCTURE);
    CHECK(!result.checked && !result.snapshot && !strcmp(diagnostic.message, "module graph build failed"));
    xr_xir_compile_source_result_free(&result);
}

static XrXirOutputStatus unexpected_output(void *context, const XrXirOutputGroup *group) {
    (void)context; (void)group; CHECK(false); return XR_XIR_OUTPUT_ERROR;
}
static void execute(XrXirArtifact *checked, const char *output) {
    XrXirArtifact *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    uint32_t entry = UINT32_MAX, inactive = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        if (module->functions[f].name_length == 6 && !memcmp(module->functions[f].name, "answer", 6)) entry = f;
        if (module->functions[f].name_length == 11 && !memcmp(module->functions[f].name, "unreachable", 11)) inactive = f;
    }
    CHECK(entry != UINT32_MAX && inactive != UINT32_MAX);
    if (output) {
        XrXirCSource source = {0};
        CHECK(xr_xir_compile_emit_c(lowered, "locked_package", 1048576, &source) == XR_XIR_OK);
        CHECK(!strstr(source.text, "({") && !strstr(source.text, "xr_xir_vm"));
        FILE *file = fopen(output, "wb"); CHECK(file);
        CHECK(fwrite(source.text, 1, source.length, file) == source.length);
        CHECK(fprintf(file, "\nconst uint32_t locked_package_entry = %uu;\n", entry) > 0);
        CHECK(fprintf(file, "const uint32_t locked_package_inactive = %uu;\n", inactive) > 0);
        CHECK(fclose(file) == 0); xr_xir_compile_c_source_free(&source);
    }
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered, &program) == XR_XIR_OK);
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY); XrXirInstance *instance = NULL;
    config.output = (XrXirOutputProvider) {XR_XIR_CALL_ABI_VERSION, 0, unexpected_output, NULL};
    CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance, inactive, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_start(instance, entry, NULL, 0) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
    XrXirValue value = {0};
    CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_start(instance, inactive, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY); xr_xir_compile_program_drop(program);
    CHECK(value.type == XR_XIR_I64 && value.payload == 41); xr_xir_value_drop(&value);
}

int main(int argc, char **argv) {
    CHECK(argc == 4 || argc == 5);
    uint64_t work = UINT64_C(128000000);
    const char *work_limit = getenv("XR_TEST_PACKAGE_WORK");
    if (work_limit) {
        char *end = NULL;
        work = strtoull(work_limit, &end, 10);
        CHECK(end && *end == 0 && work > 0);
    }
    const XrXirCompileContext context = *effects_source_owner(UINT64_C(64)*1024*1024, work);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context.resources, &session) == XR_COMPILER_SESSION_OK && session);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_PROJECT, "consumer", argv[1]};
    XrXirSourceRequest request = {session, argv[2], &authority, &context, NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrOsIoPolicy policy = xr_compile_io_policy(context.resources);
    XrLockfile *lock = NULL;
    if (!work_limit) {
        CHECK(!request.lockfile);
        reject(&request, "requires an exact checksummed");
        CHECK(xr_lockfile_new_owned(&policy, &lock) == XR_OS_IO_OK && lock); request.lockfile = lock;
        CHECK(xr_lockfile_add_package_owned(lock, "fixture/math", "1.0.0", "", "") == XR_OS_IO_OK);
        const XrLockedPackage *locked = NULL;
        CHECK(xr_lockfile_find_owned(&policy, lock, "fixture/math", &locked) == XR_OS_IO_OK && locked && !*locked->checksum);
        reject(&request, "requires an exact checksummed");
        CHECK(xr_lockfile_add_package_owned(lock, "fixture/math", "1.0.0", "",
            "sha256:0000000000000000000000000000000000000000000000000000000000000000") == XR_OS_IO_OK);
        char archive[4096];
        int written = snprintf(archive, sizeof(archive), "%s/.xray/cache/fixture-math-1.0.0.tar.gz", argv[1]);
        CHECK(written > 0 && (size_t)written < sizeof(archive));
        bool matches = true;
        CHECK(xr_lockfile_find_owned(&policy, lock, "fixture/math", &locked) == XR_OS_IO_OK && locked);
        CHECK(xr_lockfile_verify_checksum_owned(&policy, archive, locked->checksum, &matches) == XR_OS_IO_OK && !matches);
        reject(&request, "checksum does not match");
    } else {
        CHECK(xr_lockfile_new_owned(&policy, &lock) == XR_OS_IO_OK && lock); request.lockfile = lock;
    }
    CHECK(xr_lockfile_add_package_owned(lock, "fixture/math", "1.0.0", "", argv[3]) == XR_OS_IO_OK);
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_check(&request, &result, &diagnostic, NULL);
    if (work_limit) {
        CHECK(status == XR_XIR_BUDGET && !result.checked && !result.snapshot && !*diagnostic.message);
        xr_xir_compile_source_result_free(&result); xr_lockfile_free_owned(lock); xr_compile_session_free(session);
        XrCompileResourceStats stats = {0};
        CHECK(xr_compile_resources_stats(context.resources, &stats) == XR_COMPILE_RESOURCE_OK && stats.work <= work);
        fprintf(stderr, "Package work budget status=6 checked=NULL snapshot=NULL cap=%llu used=%llu\n",
            (unsigned long long)work, (unsigned long long)stats.work);
        CHECK(!runtime_live && !runtime_bytes); effects_source_owners_free(); return 1;
    }
    if (status != XR_XIR_OK) {
        CHECK((status == XR_XIR_BAD_STRUCTURE || status == XR_XIR_BAD_TYPE) && !result.checked && !result.snapshot);
        fprintf(stderr, "Package manifest failure status=%u checked=NULL snapshot=NULL message=%s\n", status, diagnostic.message);
        xr_xir_compile_source_result_free(&result); xr_lockfile_free_owned(lock); xr_compile_session_free(session);
        CHECK(!runtime_live && !runtime_bytes); effects_source_owners_free(); return 1;
    }
    CHECK(status == XR_XIR_OK && result.checked && result.snapshot);
    xr_lockfile_free_owned(lock); xr_compile_session_free(session);
    const XrXirSourceView *view = xr_xir_compile_source_snapshot_view(result.snapshot);
    CHECK(view && view->complete && view->module_count == 4);
    unsigned package_modules = 0;
    for (uint32_t m = 0; m < view->module_count; ++m)
        if (strstr(view->modules[m].identity, "fixture/math@1.0.0")) ++package_modules;
    CHECK(package_modules == 2);
    XrXirArtifact *checked = result.checked; result.checked = NULL;
    xr_xir_compile_source_result_free(&result); execute(checked, argc == 5 ? argv[4] : NULL);
    CHECK(!runtime_live && !runtime_bytes); manifest_budget_cases(argv[1]); effects_source_owners_free();
    puts("Locked package and package-relative imports execute with independent expected value 41");
    return 0;
}
