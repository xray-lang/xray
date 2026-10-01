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
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)

static void reject(const XrXirSourceRequest *request, const char *message) {
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    CHECK(xr_xir_source_check(request, &result, &diagnostic) != XR_XIR_OK);
    CHECK(!result.checked && strstr(diagnostic.message, message));
    xr_xir_source_result_free(&result);
}

static bool unexpected_output(void *context, const XrXirOutputGroup *group) {
    (void)context; (void)group; CHECK(false); return false;
}
static void execute(XrXirArtifact *checked, const char *output) {
    XrXirArtifact *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_specialize(checked, NULL, &closed, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(closed);
    const XrXirModule *module = xr_xir_artifact_module(lowered);
    uint32_t entry = UINT32_MAX, inactive = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        if (module->functions[f].name_length == 6 && !memcmp(module->functions[f].name, "answer", 6)) entry = f;
        if (module->functions[f].name_length == 11 && !memcmp(module->functions[f].name, "unreachable", 11)) inactive = f;
    }
    CHECK(entry != UINT32_MAX && inactive != UINT32_MAX);
    if (output) {
        XrXirCSource source = {0};
        CHECK(xr_xir_emit_c(lowered, "locked_package", 1048576, &source) == XR_XIR_OK);
        CHECK(!strstr(source.text, "({") && !strstr(source.text, "xr_xir_vm"));
        FILE *file = fopen(output, "wb"); CHECK(file);
        CHECK(fwrite(source.text, 1, source.length, file) == source.length);
        CHECK(fprintf(file, "\nconst uint32_t locked_package_entry = %uu;\n", entry) > 0);
        CHECK(fprintf(file, "const uint32_t locked_package_inactive = %uu;\n", inactive) > 0);
        CHECK(fclose(file) == 0); xr_xir_c_source_free(&source);
    }
    XrXirProgram *program = NULL;
    CHECK(xr_xir_vm_program_take(&lowered, (XrXirProgramBudget){33554432, 64000000}, &program) == XR_XIR_OK);
    XrXirInstanceConfig config = xr_xir_instance_defaults(); XrXirInstance *instance = NULL;
    config.output = (XrXirOutputProvider){unexpected_output, NULL};
    CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance, inactive, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_start(instance, entry, NULL, 0) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll(instance).outcome.status == XR_XIR_CALL_RETURNED);
    XrXirValue value = {0};
    CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_start(instance, inactive, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY); xr_xir_program_drop(program);
    CHECK(value.type == XR_XIR_I64 && value.payload == 41); xr_xir_value_drop(&value);
}

int main(int argc, char **argv) {
    CHECK(argc == 4 || argc == 5);
    XrCompilerSession *session = xr_compiler_session_new(NULL); CHECK(session);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_PROJECT, "consumer", argv[1]};
    XrXirSourceRequest request = {session, argv[2], &authority, NULL, NULL, NULL, XR_XIR_PROGRAM, NULL};
    reject(&request, "requires an exact checksummed");
    XrLockfile *lock = xr_lockfile_new(); CHECK(lock); request.lockfile = lock;
    CHECK(xr_lockfile_add_package(lock, "fixture/math", "1.0.0", "", ""));
    reject(&request, "requires an exact checksummed");
    CHECK(xr_lockfile_add_package(lock, "fixture/math", "1.0.0", "",
        "sha256:0000000000000000000000000000000000000000000000000000000000000000"));
    reject(&request, "checksum does not match");
    CHECK(xr_lockfile_add_package(lock, "fixture/math", "1.0.0", "", argv[3]));
    XrXirBudget budget = xr_xir_default_budget();
    const char *work_limit = getenv("XR_TEST_PACKAGE_WORK");
    if (work_limit) {
        char *end = NULL;
        budget.work = strtoull(work_limit, &end, 10);
        CHECK(end && *end == 0 && budget.work > 0);
        request.budget = &budget;
    }
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_source_check(&request, &result, &diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr, "%s\n", diagnostic.message);
    CHECK(status == XR_XIR_OK && result.checked);
    xr_lockfile_free(lock); xr_compiler_session_delete(session);
    const XrXirSourceView *view = xr_xir_source_snapshot_view(result.snapshot);
    CHECK(view && view->complete && view->module_count == 4);
    unsigned package_modules = 0;
    for (uint32_t m = 0; m < view->module_count; ++m)
        if (strstr(view->modules[m].identity, "fixture/math@1.0.0")) ++package_modules;
    CHECK(package_modules == 2);
    XrXirArtifact *checked = result.checked; result.checked = NULL;
    xr_xir_source_result_free(&result); execute(checked, argc == 5 ? argv[4] : NULL);
    puts("Locked package and package-relative imports execute with independent expected value 41");
    return 0;
}
