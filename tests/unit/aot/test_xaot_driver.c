#include "../../../src/aot/xaot_driver.h"
#include "../../../src/analysis/xglobal_producer.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/module/xmodule_graph.h"
#include "../../../src/module/xmodule_resolver.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static bool write_file_text(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (!f)
        return false;
    if (fputs(text, f) < 0) {
        fclose(f);
        return false;
    }
    return fclose(f) == 0;
}

static bool mkdir_one(const char *path) {
    if (mkdir(path, 0700) == 0)
        return true;
    return errno == EEXIST;
}

static bool mkdir_p(const char *path) {
    char buf[PATH_MAX];
    size_t len;
    if (!path)
        return false;
    len = strlen(path);
    if (len == 0 || len >= sizeof(buf))
        return false;
    memcpy(buf, path, len + 1);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (!mkdir_one(buf))
                return false;
            *p = '/';
        }
    }
    return mkdir_one(buf);
}

static char *make_package_payload_for_source(const char *canonical, const char *source_path) {
    XrModuleSpec spec = {0};
    XgBuildKey key;
    XgModuleSummary module;
    XgGlobalEvidence package = {0};
    char *payload;
    spec.canonical = (char *) canonical;
    spec.source_path = (char *) source_path;
    spec.kind = XR_MOD_PACKAGE;
    if (!xg_standalone_build_key_from_module_spec(&key, &spec, XG_BUILD_NATIVE_RELEASE, 0))
        return NULL;
    if (!xg_module_summary_from_module_spec(&module, 1, &spec))
        return NULL;
    xg_global_evidence_init(&package, key);
    if (!xg_global_evidence_add_module(&package, &module)) {
        xg_global_evidence_free(&package);
        return NULL;
    }
    payload = xg_global_evidence_cache_payload_dump(&package, XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE);
    xg_global_evidence_free(&package);
    return payload;
}

static bool write_global_payload_to_cache(const char *cache_dir, const char *payload) {
    XgEvidenceCachePayloadInfo info;
    char phase_dir[PATH_MAX];
    char payload_path[PATH_MAX];
    int n;
    if (!cache_dir || !payload || !xg_evidence_cache_payload_parse(payload, &info))
        return false;
    n = snprintf(phase_dir, sizeof(phase_dir), "%s/evidence/%s", cache_dir,
                 xg_evidence_cache_phase_name(XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE));
    if (n < 0 || (size_t) n >= sizeof(phase_dir) || !mkdir_p(phase_dir))
        return false;
    n = snprintf(payload_path, sizeof(payload_path), "%s/%016" PRIx64 ".xgpayload", phase_dir,
                 info.key_hash);
    if (n < 0 || (size_t) n >= sizeof(payload_path))
        return false;
    return write_file_text(payload_path, payload);
}

static char *dup_env_value(const char *name) {
    const char *value = getenv(name);
    return value ? xr_strdup(value) : NULL;
}

static void restore_env_value(const char *name, char *value) {
    if (value) {
        setenv(name, value, 1);
        xr_free(value);
    } else {
        unsetenv(name);
    }
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

static void test_driver_auto_discovers_package_summary_payloads(void) {
    char root[PATH_MAX];
    char home_dir[PATH_MAX];
    char pkg_dir[PATH_MAX];
    char pkg_source[PATH_MAX];
    char real_pkg_source[PATH_MAX];
    char entry_source[PATH_MAX];
    char cache_dir[PATH_MAX];
    XaotTarget target = {0};
    XaotBuildOptions options = {0};
    XaotBuildResult result;
    char *payload = NULL;
    const char *payloads[1];
    uint64_t imported_hash = 0;
    char *old_home;

    memset(&result, 0, sizeof(result));
    snprintf(root, sizeof(root), "/tmp/xray-xaot-driver-auto-%ld", (long) getpid());
    snprintf(home_dir, sizeof(home_dir), "%s/home", root);
    snprintf(pkg_dir, sizeof(pkg_dir), "%s/.xray/packages/codex/pkg/1.0.0/src", home_dir);
    snprintf(pkg_source, sizeof(pkg_source), "%s/main.xr", pkg_dir);
    snprintf(entry_source, sizeof(entry_source), "%s/entry.xr", root);
    snprintf(cache_dir, sizeof(cache_dir), "%s/cache/aot/native", root);
    ASSERT_TRUE(mkdir_p(pkg_dir));
    ASSERT_TRUE(write_file_text(pkg_source, "fn package_value() -> int {\n"
                                            "    return 5\n"
                                            "}\n"));
    ASSERT_TRUE(write_file_text(entry_source, "import \"codex/pkg\" as pkg\n"
                                              "fn value() -> int {\n"
                                              "    return 7\n"
                                              "}\n"));
    ASSERT_TRUE(realpath(pkg_source, real_pkg_source) != NULL);
    payload = make_package_payload_for_source("codex/pkg", real_pkg_source);
    ASSERT_TRUE(payload != NULL);
    payloads[0] = payload;
    ASSERT_TRUE(xg_imported_summary_hash_from_package_payloads(0, payloads, 1, &imported_hash));
    ASSERT_TRUE(write_global_payload_to_cache(cache_dir, payload));
    old_home = dup_env_value("HOME");
    setenv("HOME", home_dir, 1);

    ASSERT_TRUE(xaot_target_init(&target, NULL));
    options.target = &target;
    options.profile = XAOT_BUILD_PROFILE_HOSTED;
    options.emit_global_evidence_dump = true;
    options.evidence_cache_dir = cache_dir;

    ASSERT_TRUE(xaot_build(entry_source, &options, &result) == 0);
    ASSERT_TRUE(dump_contains_import_hash(result.global_evidence_dump, imported_hash));

    xaot_build_result_free(&result);
    xaot_target_free(&target);
    restore_env_value("HOME", old_home);
    xr_free(payload);
    unlink(entry_source);
    unlink(pkg_source);
    passed++;
}

int main(void) {
    test_driver_consumes_imported_summary_payload_set();
    test_driver_rejects_invalid_imported_summary_payload_set();
    test_driver_auto_discovers_package_summary_payloads();
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
