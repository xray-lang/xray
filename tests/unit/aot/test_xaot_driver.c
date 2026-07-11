#include "../../../src/aot/xaot_driver.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/module/xmodule_resolver.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int passed;
static int failed;

#define ASSERT_TRUE(cond)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                 \
            failed++;                                                                              \
            return;                                                                                \
        }                                                                                          \
    } while (0)

static bool write_temp_source(char *path, size_t path_sz) {
    FILE *f;
    int n = snprintf(path, path_sz, "/tmp/xray-xaot-driver-%ld.xr", (long) getpid());
    if (n < 0 || (size_t) n >= path_sz)
        return false;
    f = fopen(path, "w");
    if (!f)
        return false;
    if (fputs("fn value() -> int {\n    return 7\n}\n", f) < 0) {
        fclose(f);
        unlink(path);
        return false;
    }
    if (fclose(f) != 0) {
        unlink(path);
        return false;
    }
    return true;
}

static char *make_package_payload(void) {
    XgGlobalEvidence package = {0};
    XgBuildKey key = {
        .module_id = 7,
        .profile = XG_BUILD_NATIVE_RELEASE,
        .source_hash = 0x7100,
        .compiler_semver_hash = 0x7200,
        .profile_hash = 0x7300,
        .imported_summary_hash = 0,
    };
    XgModuleSummary module = {
        .module_id = 7,
        .name_id = xg_name_id("pkg.driver"),
        .canonical_hash = 0x7101,
        .source_hash = 0x7102,
        .kind = XR_MOD_PACKAGE,
        .flags = 0,
    };
    char *payload;
    xg_global_evidence_init(&package, key);
    if (!xg_global_evidence_add_module(&package, &module)) {
        xg_global_evidence_free(&package);
        return NULL;
    }
    payload = xg_global_evidence_cache_payload_dump(&package, XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE);
    xg_global_evidence_free(&package);
    return payload;
}

static bool dump_contains_import_hash(const char *dump, uint64_t imported_hash) {
    char needle[64];
    snprintf(needle, sizeof(needle), "imports=%016" PRIx64, imported_hash);
    return dump && strstr(dump, needle) != NULL;
}

static void test_driver_consumes_imported_summary_payload_set(void) {
    char source_path[256];
    XaotTarget target = {0};
    XaotBuildOptions options = {0};
    XaotBuildResult result;
    char *payload = NULL;
    const char *payloads[1];
    uint64_t imported_hash = 0;

    memset(&result, 0, sizeof(result));
    ASSERT_TRUE(write_temp_source(source_path, sizeof(source_path)));
    ASSERT_TRUE(xaot_target_init(&target, NULL));
    payload = make_package_payload();
    ASSERT_TRUE(payload != NULL);
    payloads[0] = payload;
    ASSERT_TRUE(xg_imported_summary_hash_from_package_payloads(0, payloads, 1, &imported_hash));

    options.target = &target;
    options.profile = XAOT_BUILD_PROFILE_HOSTED;
    options.emit_global_evidence_dump = true;
    options.imported_summary_payloads = payloads;
    options.imported_summary_payload_count = 1;

    ASSERT_TRUE(xaot_build(source_path, &options, &result) == 0);
    ASSERT_TRUE(dump_contains_import_hash(result.global_evidence_dump, imported_hash));

    xaot_build_result_free(&result);
    xaot_target_free(&target);
    xr_free(payload);
    unlink(source_path);
    passed++;
}

static void test_driver_rejects_invalid_imported_summary_payload_set(void) {
    XaotTarget target = {0};
    XaotBuildOptions options = {0};
    XaotBuildResult result;
    const char *payloads[1] = {"not-a-valid-payload"};

    memset(&result, 0, sizeof(result));
    ASSERT_TRUE(xaot_target_init(&target, NULL));
    options.target = &target;
    options.profile = XAOT_BUILD_PROFILE_HOSTED;
    options.imported_summary_payloads = payloads;
    options.imported_summary_payload_count = 1;

    ASSERT_TRUE(xaot_build("/tmp/xray-xaot-driver-missing.xr", &options, &result) != 0);
    ASSERT_TRUE(result.n_sources == 0);

    xaot_build_result_free(&result);
    xaot_target_free(&target);
    passed++;
}

int main(void) {
    test_driver_consumes_imported_summary_payload_set();
    test_driver_rejects_invalid_imported_summary_payload_set();
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
