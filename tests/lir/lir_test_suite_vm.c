/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_test_suite_vm.c - Execute and emit one source-owned test suite
 */
#include "lir_test_suite_catalog.h"
#include "lir/xlir_c.h"
#include "lir/xlir_test_plan.h"
#include "runtime/native/xnative_execution.h"
#include "vm/register/xrbc_lir.h"
#include <stdlib.h>

_Static_assert(XR_XIR_TEST_ROLE_TEST == (XrXirTestRole)SUITE_TEST &&
    XR_XIR_TEST_ROLE_SKIP == (XrXirTestRole)SUITE_SKIP, "suite report preserves Source roles");
_Static_assert(XR_FINGERPRINT_BYTES == SUITE_DIGEST_SIZE, "suite report carries the full source digest");

static bool catalog_roots(const XrLirTestPlan *plan, SuiteRow *rows, XrLirFunction *roots, uint32_t count,
    const XrLirTestCatalogView *metadata, uint32_t *actions) {
    for (uint32_t i = 0; i < count; ++i) {
        XrLirTestPlanRole role;
        if (xr_lir_test_plan_role_at(plan, i, &role) != XR_LIR_OK) return false;
        uint32_t action = UINT32_MAX;
        if (role.role == XR_XIR_TEST_ROLE_TEST) {
            if (xr_lir_test_plan_check_role(plan, i, role.function) != XR_LIR_OK) return false;
            action = (*actions)++;
            roots[action] = role.function;
        } else if (role.role != XR_XIR_TEST_ROLE_SKIP || role.function.id) {
            return false;
        }
        rows[i] = (SuiteRow){role.name, metadata->entries[i].closed_function, (uint32_t)role.role,
            metadata->entries[i].timeout_milliseconds, action};
    }
    return true;
}

static bool write_c_string(FILE *output, const char *text) {
    if (fputc('"', output) == EOF) return false;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
        if (fprintf(output, "\\x%02x", (unsigned)*p) < 0) return false;
    return fputc('"', output) != EOF;
}

static bool write_metadata(FILE *output, const SuiteCatalog *catalog) {
    if (fputs("\n#include \"lir_test_suite_catalog.h\"\nstatic const SuiteRow xrl_suite_rows[] = {\n", output) < 0)
        return false;
    for (uint32_t i = 0; i < catalog->count; ++i) {
        const SuiteRow *row = &catalog->rows[i];
        if (fputs("    {", output) < 0 || !write_c_string(output, row->name)) return false;
        if (fprintf(output, ", UINT32_C(%" PRIu32 "), UINT32_C(%" PRIu32 "), "
                "UINT32_C(%" PRIu32 "), UINT32_C(%" PRIu32 ")},\n",
                row->function, row->role, row->timeout, row->action) < 0) return false;
    }
    if (fputs("};\nconst SuiteCatalog xrl_suite_catalog = {{", output) < 0) return false;
    for (size_t i = 0; i < SUITE_DIGEST_SIZE; ++i)
        if (fprintf(output, "0x%02x,", (unsigned)catalog->source_sha256[i]) < 0) return false;
    return fprintf(output, "}, xrl_suite_rows, UINT32_C(%" PRIu32 "), ", catalog->count) >= 0 &&
        write_c_string(output, catalog->source) && fputs("};\n", output) >= 0;
}

static bool write_products(const char *c_path, const char *report_path, const XrLirCOutput *code,
    const SuiteCatalog *catalog, SuiteResults results) {
    FILE *output = fopen(c_path, "wbx");
    if (!output) return false;
    bool written = fwrite(code->bytes, 1, code->length, output) == code->length &&
        write_metadata(output, catalog);
    bool closed = fclose(output) == 0;
    if (written && closed && suite_write_report(report_path, catalog, results)) return true;
    (void)remove(c_path);
    return false;
}

static bool run_tests(XrRbcTestSession *session, const SuiteCatalog *catalog, SuiteResults *results) {
    for (uint32_t i = 0; i < catalog->count; ++i) {
        const SuiteRow *row = &catalog->rows[i];
        if (row->role == SUITE_SKIP) {
            suite_record(results, "VM", row, false);
            continue;
        }
        XrRbcOutcome *outcome = NULL;
        XrRbcTestActionStage stage;
        XrRbcStatus status = xr_rbc_test_session_run_detailed(session, row->action, &outcome, &stage);
        if (stage != XR_RBC_TEST_ACTION_INVOKE || status == XR_RBC_INVALID || !outcome) {
            fprintf(stderr, "VM test action could not start (stage %u, status %u)\n",
                (unsigned)stage, (unsigned)status);
            xr_rbc_outcome_free(&outcome);
            return false;
        }
        if (status != XR_RBC_OK) {
            uint32_t code, source;
            const char *message;
            const XrtLocationOwner *locations;
            size_t length;
            if (xr_rbc_outcome_message_panic(outcome, &code, &message, &length, &source, &locations))
                (void)xrt_message_panic_report(code, message, length, xrt_locations_get(locations, source));
        }
        suite_record(results, "VM", row, status == XR_RBC_OK);
        if (!xr_rbc_outcome_free(&outcome)) return false;
    }
    return true;
}

int main(int argc, char **argv) {
    if (argc != 6 || !argv[3][0]) {
        fputs("usage: lir_test_suite_vm ROOT STDLIB RELATIVE-SOURCE GENERATED-C REPORT\n", stderr);
        return 2;
    }
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
    XrCompileResources *resources = NULL;
    if (xr_compile_resources_new(&limits, &resources) != XR_COMPILE_RESOURCE_OK) return 1;

    int result = 1;
    char *path = NULL;
    const char *source = argv[3];
    size_t root_length = strlen(argv[1]), source_length = strlen(source);
    XrLirTestPlan *plan = NULL;
    XrLirSourceFailure failure = {0};
    XrLirCOutput output = {0};
    XrRbcTestSession *session = NULL;
    SuiteRow *rows = NULL;
    XrLirFunction *roots = NULL;
    if (root_length > SIZE_MAX - 2 || source_length > SIZE_MAX - root_length - 2 ||
        xr_compile_resources_alloc(resources, root_length + source_length + 2,
            (void **)&path) != XR_COMPILE_RESOURCE_OK) goto done;
    memcpy(path, argv[1], root_length);
    path[root_length] = '/';
    memcpy(path + root_length + 1, source, source_length + 1);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, argv[1]};
    XrLirSourceRequest request = {resources, path, argv[2], false, &authority, NULL, XR_XIR_HOST_TARGET};
    if (xr_lir_compile_test_catalog(&request, &plan, &failure) != XR_LIR_SOURCE_OK) {
        const char *message = failure.stage == XR_LIR_SOURCE_LOWER_FAILED ?
            failure.lower.diagnostic.message : failure.source.message;
        fprintf(stderr, "%s: suite compilation failed at stage %u: %s\n",
            source, (unsigned)failure.stage, message);
        goto done;
    }

    XrLirTestCatalogView metadata;
    if (!xr_lir_test_plan_catalog(plan, &metadata) || !metadata.count ||
        xr_compile_resources_calloc(resources, metadata.count, sizeof(*rows), (void **)&rows) !=
            XR_COMPILE_RESOURCE_OK ||
        xr_compile_resources_calloc(resources, metadata.count, sizeof(*roots), (void **)&roots) !=
            XR_COMPILE_RESOURCE_OK) goto done;

    uint32_t actions = 0;
    SuiteCatalog catalog = {.rows = rows, .count = metadata.count, .source = source};
    memcpy(catalog.source_sha256, metadata.source_sha256.bytes, sizeof(catalog.source_sha256));
    if (!catalog_roots(plan, rows, roots, metadata.count, &metadata, &actions) ||
        !suite_catalog_valid(&catalog, actions)) {
        fputs("suite requires a runnable root catalog of test and skip rows\n", stderr);
        goto done;
    }

    XrLirFunction initializer = xr_lir_test_plan_initializer(plan);
    XrRbcCompileDiagnostic vm_diagnostic = {0};
    if (xr_rbc_test_session_compile(initializer.module, initializer, roots, actions,
            &session, &vm_diagnostic) != XR_RBC_COMPILE_OK) {
        fprintf(stderr, "cannot compile suite bytecode: %s\n", vm_diagnostic.message);
        goto done;
    }
    XrLirCRequest emit = {initializer.module, initializer, resources, path, NULL};
    XrLirCDiagnostic c_diagnostic = {0};
    if (xr_lir_emit_c_test_session(&emit, roots, actions, &output, &c_diagnostic) != XR_LIR_C_OK) {
        fprintf(stderr, "cannot emit suite C: %s\n", c_diagnostic.message);
        goto done;
    }

    SuiteResults results = {0};
    bool executed = run_tests(session, &catalog, &results);
    if (xr_rbc_test_session_free(&session) != XR_OS_IO_OK) {
        fputs("cannot retire suite execution\n", stderr);
        _Exit(1);
    }
    if (!executed) goto done;
    if (!write_products(argv[4], argv[5], &output, &catalog, results)) {
        fputs("cannot publish suite products\n", stderr);
        goto done;
    }
    suite_summary(results);
    result = results.failed ? 1 : 0;

done:
    if (xr_rbc_test_session_free(&session) != XR_OS_IO_OK) _Exit(1);
    xr_lir_c_output_free(&output);
    xr_compile_resources_free(roots);
    xr_compile_resources_free(rows);
    xr_lir_test_plan_free(plan);
    xr_lir_source_failure_free(&failure);
    xr_compile_resources_free(path);
    xr_compile_resources_release(resources);
    return result;
}
