#include "../../../src/aot/xaot_driver.h"
#include "../../../src/analysis/xglobal_producer.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/module/xmodule_graph.h"
#include "../../../src/module/xmodule_resolver.h"
#include "../../../src/incremental/xr_target_plan_tasks.h"
#include "../../../src/app/toolchain/xtc_target_profile.h"
#include "../../../src/os/os_dir.h"
#include "../../../src/os/os_temp.h"
#include "../test_win_compat.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif

static int passed;
static int failed;

static bool install_native_target_profile(XaotBuildOptions *options,
                                          const XaotTarget *target) {
    XrTargetCodegenFacts codegen;
    char error[256];
    return options && xaot_target_profile_codegen_facts(target, &codegen) &&
           xtc_target_profile_build_current_native_hosted(
               &codegen, &options->target_profile, error, sizeof(error));
}

static void release_target_profile(XaotBuildOptions *options) {
    if (!options)
        return;
    xr_target_profile_free(options->target_profile);
    options->target_profile = NULL;
}

static int xaot_build_script(const char *source_path, XaotBuildOptions *options,
                             XaotBuildResult *result) {
    XrModuleIdentityAuthority authority = {0};
    char *authority_root = NULL;
    if (!options ||
        !xr_module_identity_script_authority_from_source(
            source_path, &authority, &authority_root))
        return 1;
    options->entry_module_authority = authority;
    int rc = xaot_build(source_path, options, result);
    options->entry_module_authority = (XrModuleIdentityAuthority) {0};
    xr_free(authority_root);
    return rc;
}

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
    int n = snprintf(path, path_sz, "/tmp/xray-xaot-driver-%ld.xr", (long) xr_test_getpid());
    if (n < 0 || (size_t) n >= path_sz)
        return false;
    f = fopen(path, "w");
    if (!f)
        return false;
    if (fputs("fn value() -> i64 {\n    return 7\n}\n", f) < 0) {
        fclose(f);
        xr_test_unlink(path);
        return false;
    }
    if (fclose(f) != 0) {
        xr_test_unlink(path);
        return false;
    }
    return true;
}

static bool corrupt_first_xtp_cache_object(const char *root) {
    char directory[XR_TEST_PATH_MAX];
    if (snprintf(directory, sizeof(directory), "%s/xtp", root) < 0 ||
        strlen(directory) >= sizeof(directory))
        return false;
    XrDirIter *iterator = xr_dir_open(directory);
    if (!iterator)
        return false;
    XrDirEntry entry;
    bool corrupted = false;
    while (xr_dir_next(iterator, &entry)) {
        if (entry.is_dir || entry.name[0] == '.')
            continue;
        char path[XR_TEST_PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", directory, entry.name) < 0 ||
            strlen(path) >= sizeof(path))
            break;
        FILE *file = fopen(path, "r+b");
        if (!file)
            break;
        if (fseek(file, -1L, SEEK_END) == 0) {
            int byte = fgetc(file);
            if (byte != EOF && fseek(file, -1L, SEEK_END) == 0 &&
                fputc(byte ^ 1, file) != EOF && fclose(file) == 0) {
                corrupted = true;
                break;
            }
        }
        fclose(file);
        break;
    }
    xr_dir_close(iterator);
    return corrupted;
}

static bool add_package_link_dependency(XgGlobalEvidence *package, XgModuleId module_id) {
    XgLinkDependencySummary dep;
    memset(&dep, 0, sizeof(dep));
    dep.link_id = 1;
    dep.module_id = module_id;
    dep.kind = XG_LINK_DEP_STDLIB_SYMBOL;
    dep.name_id = xg_name_id("math.abs");
    snprintf(dep.name, sizeof(dep.name), "%s", "math.abs");
    return xg_global_evidence_add_link_dependency(package, &dep) != NULL;
}

static bool add_package_function_storage_decls(XgGlobalEvidence *package, XgModuleId module_id,
                                               const char *source_path) {
    FILE *f;
    long size;
    char *source;
    char *cursor;
    if (!package || !source_path || !(f = fopen(source_path, "rb")))
        return false;
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    source = (char *) xr_malloc((size_t) size + 1);
    if (!source || fread(source, 1, (size_t) size, f) != (size_t) size) {
        xr_free(source);
        fclose(f);
        return false;
    }
    fclose(f);
    source[size] = '\0';
    cursor = source;
    while ((cursor = strstr(cursor, "fn ")) != NULL) {
        char name[128];
        size_t len = 0;
        XgDeclSummary decl;
        cursor += 3;
        while ((cursor[len] == '_' || (cursor[len] >= 'a' && cursor[len] <= 'z') ||
                (cursor[len] >= 'A' && cursor[len] <= 'Z') ||
                (len > 0 && cursor[len] >= '0' && cursor[len] <= '9')) &&
               len + 1 < sizeof(name))
            len++;
        if (len == 0)
            continue;
        memcpy(name, cursor, len);
        name[len] = '\0';
        memset(&decl, 0, sizeof(decl));
        decl.module_id = module_id;
        decl.decl_id = package->ndecls + 1;
        decl.source_node_id = xg_stable_source_node_id(module_id, 1, decl.decl_id, 1);
        decl.kind = XG_DECL_FUNC;
        decl.name_id = xg_name_id(name);
        decl.source_span_id = decl.decl_id;
        decl.storage_domain = XR_STORAGE_MODULE_STATIC;
        decl.storage_mutability = XR_STORAGE_READONLY;
        decl.address_identity = XR_ADDRESS_MODULE_STABLE;
        decl.materialization_kind = XR_MATERIALIZE_STATIC_DATA;
        if (!xg_global_evidence_add_decl(package, &decl)) {
            xr_free(source);
            return false;
        }
        {
            XgBodySummary body;
            memset(&body, 0, sizeof(body));
            body.func_id = package->nbodies + 1;
            body.module_id = module_id;
            body.source_node_id = decl.source_node_id;
            body.owner_decl_id = decl.decl_id;
            body.name_id = decl.name_id;
            body.source_span_id = decl.source_span_id;
            body.kind = XG_BODY_FUNCTION;
            body.body_hash = (uint64_t) decl.name_id + 1;
            if (!xg_global_evidence_add_body(package, &body)) {
                xr_free(source);
                return false;
            }
        }
        cursor += len;
    }
    cursor = source;
    while ((cursor = strstr(cursor, "import \"")) != NULL) {
        char *line_end = strchr(cursor, '\n');
        char *as = strstr(cursor, "\" as ");
        char name[128];
        size_t len = 0;
        XgDeclSummary decl;
        if (!as || (line_end && as >= line_end)) {
            cursor += 8;
            continue;
        }
        as += 5;
        while ((as[len] == '_' || (as[len] >= 'a' && as[len] <= 'z') ||
                (as[len] >= 'A' && as[len] <= 'Z') ||
                (len > 0 && as[len] >= '0' && as[len] <= '9')) &&
               len + 1 < sizeof(name))
            len++;
        if (len == 0) {
            cursor = as;
            continue;
        }
        memcpy(name, as, len);
        name[len] = '\0';
        memset(&decl, 0, sizeof(decl));
        decl.module_id = module_id;
        decl.decl_id = package->ndecls + 1;
        decl.source_node_id = xg_stable_source_node_id(module_id, 2, decl.decl_id, 1);
        decl.kind = XG_DECL_GLOBAL;
        decl.name_id = xg_name_id(name);
        decl.source_span_id = decl.decl_id;
        decl.storage_domain = XR_STORAGE_MODULE_STATIC;
        decl.storage_mutability = XR_STORAGE_READONLY;
        decl.address_identity = XR_ADDRESS_MODULE_STABLE;
        decl.materialization_kind = XR_MATERIALIZE_STATIC_DATA;
        if (!xg_global_evidence_add_decl(package, &decl)) {
            xr_free(source);
            return false;
        }
        cursor = as + len;
    }
    xr_free(source);
    return true;
}

static bool write_file_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    if (fputs(text, f) < 0) {
        fclose(f);
        return false;
    }
    return fclose(f) == 0;
}

static bool mkdir_one(const char *path) {
    if (xr_test_mkdir(path) == 0)
        return true;
    return errno == EEXIST;
}

static bool mkdir_p(const char *path) {
    char buf[XR_TEST_PATH_MAX];
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
    if (!xg_global_evidence_add_module(&package, &module) ||
        !add_package_function_storage_decls(&package, module.module_id, source_path) ||
        !add_package_link_dependency(&package, module.module_id)) {
        xg_global_evidence_free(&package);
        return NULL;
    }
    payload = xg_global_evidence_cache_payload_dump(&package, XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE);
    xg_global_evidence_free(&package);
    return payload;
}

static char *make_package_payload_for_ordered_sources(const char *const *canonicals,
                                                      const char *const *source_paths,
                                                      uint32_t count) {
    XrModuleSpec *specs = NULL;
    const XrModuleSpec **ordered_specs = NULL;
    XgBuildKey key;
    XgGlobalEvidence package = {0};
    char *payload = NULL;
    if (!canonicals || !source_paths || count == 0)
        return NULL;
    specs = (XrModuleSpec *) xr_calloc(count, sizeof(*specs));
    ordered_specs = (const XrModuleSpec **) xr_calloc(count, sizeof(*ordered_specs));
    if (!specs || !ordered_specs)
        goto done;
    for (uint32_t i = 0; i < count; i++) {
        if (!canonicals[i] || !source_paths[i])
            goto done;
        specs[i].canonical = (char *) canonicals[i];
        specs[i].source_path = (char *) source_paths[i];
        specs[i].kind = XR_MOD_PACKAGE;
        ordered_specs[i] = &specs[i];
    }
    if (!xg_build_key_from_ordered_module_specs(&key, ordered_specs, count, XG_BUILD_NATIVE_RELEASE,
                                                0))
        goto done;
    xg_global_evidence_init(&package, key);
    for (uint32_t i = 0; i < count; i++) {
        XgModuleSummary module;
        if (!xg_module_summary_from_module_spec(&module, i + 1, &specs[i]) ||
            !xg_global_evidence_add_module(&package, &module) ||
            !add_package_function_storage_decls(&package, module.module_id, source_paths[i]))
            goto done;
    }
    if (!add_package_link_dependency(&package, 1))
        goto done;
    payload = xg_global_evidence_cache_payload_dump(&package, XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE);

done:
    xg_global_evidence_free(&package);
    xr_free(ordered_specs);
    xr_free(specs);
    return payload;
}

static bool write_global_payload_to_cache(const char *cache_dir, const char *payload) {
    XgEvidenceCachePayloadInfo info;
    char phase_dir[XR_TEST_PATH_MAX];
    char payload_path[XR_TEST_PATH_MAX];
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

static bool write_package_source(const char *home_dir, const char *canonical,
                                 const char *source_text, char *out_real_path,
                                 size_t out_real_path_size) {
    const char *slash;
    size_t owner_len;
    char pkg_dir[XR_TEST_PATH_MAX];
    char pkg_source[XR_TEST_PATH_MAX];
    char real_pkg_source[XR_TEST_PATH_MAX];
    int n;
    if (!home_dir || !canonical || !source_text || !out_real_path || out_real_path_size == 0)
        return false;
    slash = strchr(canonical, '/');
    if (!slash || slash == canonical || !slash[1])
        return false;
    owner_len = (size_t) (slash - canonical);
    if (owner_len > 120 || strlen(slash + 1) > 120)
        return false;
    n = snprintf(pkg_dir, sizeof(pkg_dir), "%s/.xray/packages/%.*s/%s/1.0.0/src", home_dir,
                 (int) owner_len, canonical, slash + 1);
    if (n < 0 || (size_t) n >= sizeof(pkg_dir) || !mkdir_p(pkg_dir))
        return false;
    n = snprintf(pkg_source, sizeof(pkg_source), "%s/main.xr", pkg_dir);
    if (n < 0 || (size_t) n >= sizeof(pkg_source) || !write_file_text(pkg_source, source_text))
        return false;
    if (!xr_test_realpath_buf(pkg_source, real_pkg_source, sizeof(real_pkg_source)))
        return false;
    n = snprintf(out_real_path, out_real_path_size, "%s", real_pkg_source);
    return n >= 0 && (size_t) n < out_real_path_size;
}

static char *install_package_payload(const char *home_dir, const char *cache_dir,
                                     const char *canonical, const char *source_text) {
    char real_pkg_source[XR_TEST_PATH_MAX];
    char *payload;
    if (!home_dir || !cache_dir ||
        !write_package_source(home_dir, canonical, source_text, real_pkg_source,
                              sizeof(real_pkg_source)))
        return NULL;
    payload = make_package_payload_for_source(canonical, real_pkg_source);
    if (!payload)
        return NULL;
    if (!write_global_payload_to_cache(cache_dir, payload)) {
        xr_free(payload);
        return NULL;
    }
    return payload;
}

static char *dup_env_value(const char *name) {
    const char *value = getenv(name);
    return value ? xr_strdup(value) : NULL;
}

static void restore_env_value(const char *name, char *value) {
    if (value) {
        xr_test_setenv(name, value, 1);
        xr_free(value);
    } else {
        xr_test_unsetenv(name);
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
    if (!xg_global_evidence_add_module(&package, &module) ||
        !add_package_link_dependency(&package, module.module_id)) {
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

static bool dump_contains_imported_package_link_dep(const char *dump) {
    return dump && strstr(dump, "link-dep") && strstr(dump, "name=math.abs");
}

static void test_target_simd_plan_is_explicit_and_fail_closed(void) {
    XaotTarget x86 = {0};
    XaotTarget arm = {0};
    XaotTarget arm32 = {0};
    XaotTarget x86_32 = {0};
    XaotTarget powerpc64 = {0};
    XaotTarget powerpc64le = {0};
    XaotTarget loongarch64 = {0};
    XaotLinkManifest manifest = {0};
    XaotSimdMode parsed = XAOT_SIMD_AUTO;
    char err[192];

    ASSERT_TRUE(xaot_simd_mode_parse("scalar", &parsed));
    ASSERT_TRUE(parsed == XAOT_SIMD_SCALAR);
    ASSERT_TRUE(!xaot_simd_mode_parse("host-macros", &parsed));

    ASSERT_TRUE(xaot_target_init(&x86, "x86_64-linux-musl"));
    ASSERT_TRUE(x86.simd_mode == XAOT_SIMD_AUTO);
    ASSERT_TRUE(x86.simd_features == XAOT_SIMD_FEATURE_SSE2);
    ASSERT_TRUE(xaot_target_configure_simd(&x86, XAOT_SIMD_AVX2, "haswell", err, sizeof(err)));
    ASSERT_TRUE(x86.simd_features == (XAOT_SIMD_FEATURE_SSE2 | XAOT_SIMD_FEATURE_AVX2));
    ASSERT_TRUE(strcmp(x86.cpu, "haswell") == 0);
    ASSERT_TRUE(xaot_simd_mode_parse("avx512", &parsed));
    ASSERT_TRUE(parsed == XAOT_SIMD_AVX512);
    ASSERT_TRUE(
        xaot_target_configure_simd(&x86, XAOT_SIMD_AVX512, "skylake-avx512", err, sizeof(err)));
    ASSERT_TRUE(x86.simd_features ==
                (XAOT_SIMD_FEATURE_SSE2 | XAOT_SIMD_FEATURE_AVX2 | XAOT_SIMD_FEATURE_AVX512));
    ASSERT_TRUE(xaot_simd_mode_parse("dispatch", &parsed));
    ASSERT_TRUE(parsed == XAOT_SIMD_DISPATCH);
    ASSERT_TRUE(xaot_target_configure_simd(&x86, XAOT_SIMD_DISPATCH, NULL, err, sizeof(err)));
    ASSERT_TRUE(x86.simd_features ==
                (XAOT_SIMD_FEATURE_SSE2 | XAOT_SIMD_FEATURE_AVX2 | XAOT_SIMD_FEATURE_AVX512));
    ASSERT_TRUE(!xaot_target_configure_simd(&x86, XAOT_SIMD_NEON, NULL, err, sizeof(err)));
    ASSERT_TRUE(strstr(err, "AArch64") != NULL);

    ASSERT_TRUE(xaot_target_init(&powerpc64le, "powerpc64le-linux-musl"));
    ASSERT_TRUE(powerpc64le.pointer_bits == 64);
    ASSERT_TRUE(strcmp(powerpc64le.endian, "little") == 0);
    ASSERT_TRUE(powerpc64le.data_layout.endian == XR_TARGET_ENDIAN_LITTLE);
    ASSERT_TRUE(powerpc64le.simd_features == 0);
    ASSERT_TRUE(xaot_target_configure_simd(&powerpc64le, XAOT_SIMD_VSX, NULL, err, sizeof(err)));
    ASSERT_TRUE(powerpc64le.simd_features == XAOT_SIMD_FEATURE_VSX);

    ASSERT_TRUE(xaot_target_init(&loongarch64, "loongarch64-linux-musl"));
    ASSERT_TRUE(loongarch64.pointer_bits == 64);
    ASSERT_TRUE(strcmp(loongarch64.endian, "little") == 0);
    ASSERT_TRUE(loongarch64.data_layout.endian == XR_TARGET_ENDIAN_LITTLE);
    ASSERT_TRUE(loongarch64.simd_features == 0);
    ASSERT_TRUE(xaot_simd_mode_parse("lsx", &parsed));
    ASSERT_TRUE(parsed == XAOT_SIMD_LSX);
    ASSERT_TRUE(xaot_target_configure_simd(&loongarch64, XAOT_SIMD_LSX, NULL, err, sizeof(err)));
    ASSERT_TRUE(loongarch64.simd_features == XAOT_SIMD_FEATURE_LSX);
    ASSERT_TRUE(xaot_target_configure_simd(&loongarch64, XAOT_SIMD_NATIVE, NULL, err, sizeof(err)));
    ASSERT_TRUE(loongarch64.simd_features == XAOT_SIMD_FEATURE_LSX);
    ASSERT_TRUE(!xaot_target_configure_simd(&loongarch64, XAOT_SIMD_VSX, NULL, err, sizeof(err)));
    ASSERT_TRUE(strstr(err, "PowerPC64") != NULL);

    ASSERT_TRUE(xaot_target_init(&arm, "aarch64-linux-musl"));
    ASSERT_TRUE(arm.simd_features == XAOT_SIMD_FEATURE_NEON);
    ASSERT_TRUE(xaot_simd_mode_parse("sve", &parsed));
    ASSERT_TRUE(parsed == XAOT_SIMD_SVE);
    ASSERT_TRUE(xaot_target_configure_simd(&arm, XAOT_SIMD_SVE, NULL, err, sizeof(err)));
    ASSERT_TRUE(arm.simd_features == XAOT_SIMD_FEATURE_SVE);
    ASSERT_TRUE(!xaot_target_configure_simd(&x86, XAOT_SIMD_SVE, NULL, err, sizeof(err)));
    ASSERT_TRUE(strstr(err, "AArch64") != NULL);
    ASSERT_TRUE(!xaot_target_configure_simd(&arm, XAOT_SIMD_AVX2, NULL, err, sizeof(err)));
    ASSERT_TRUE(strstr(err, "x86_64") != NULL);
    ASSERT_TRUE(!xaot_target_configure_simd(&arm, XAOT_SIMD_DISPATCH, NULL, err, sizeof(err)));
    ASSERT_TRUE(strstr(err, "x86_64") != NULL);
    ASSERT_TRUE(xaot_target_configure_simd(&arm, XAOT_SIMD_SCALAR, NULL, err, sizeof(err)));
    ASSERT_TRUE(arm.simd_features == 0);

    ASSERT_TRUE(xaot_target_init(&arm32, "arm-linux-gnueabi"));
    ASSERT_TRUE(strcmp(arm32.arch, "arm") == 0);
    ASSERT_TRUE(strcmp(arm32.abi, "gnu") == 0);
    ASSERT_TRUE(arm32.pointer_bits == 32);
    ASSERT_TRUE(strcmp(arm32.endian, "little") == 0);
    ASSERT_TRUE(arm32.data_layout.pointer.size == 4);
    ASSERT_TRUE(arm32.data_layout.endian == XR_TARGET_ENDIAN_LITTLE);
    ASSERT_TRUE(arm32.simd_features == 0);

    ASSERT_TRUE(xaot_target_init(&x86_32, "i386-linux-musl"));
    ASSERT_TRUE(x86_32.pointer_bits == 32);
    ASSERT_TRUE(strcmp(x86_32.endian, "little") == 0);
    ASSERT_TRUE(x86_32.data_layout.pointer.size == 4);
    ASSERT_TRUE(x86_32.simd_features == 0);

    ASSERT_TRUE(xaot_target_init(&powerpc64, "powerpc64-linux-musl"));
    ASSERT_TRUE(powerpc64.pointer_bits == 64);
    ASSERT_TRUE(strcmp(powerpc64.endian, "big") == 0);
    ASSERT_TRUE(powerpc64.data_layout.pointer.size == 8);
    ASSERT_TRUE(powerpc64.data_layout.endian == XR_TARGET_ENDIAN_BIG);
    ASSERT_TRUE(powerpc64.simd_features == 0);
    ASSERT_TRUE(xaot_simd_mode_parse("vsx", &parsed));
    ASSERT_TRUE(parsed == XAOT_SIMD_VSX);
    ASSERT_TRUE(xaot_target_configure_simd(&powerpc64, XAOT_SIMD_VSX, NULL, err, sizeof(err)));
    ASSERT_TRUE(powerpc64.simd_features == XAOT_SIMD_FEATURE_VSX);
    ASSERT_TRUE(xaot_target_configure_simd(&powerpc64, XAOT_SIMD_NATIVE, NULL, err, sizeof(err)));
    ASSERT_TRUE(powerpc64.simd_features == XAOT_SIMD_FEATURE_VSX);
    ASSERT_TRUE(!xaot_target_configure_simd(&powerpc64, XAOT_SIMD_NEON, NULL, err, sizeof(err)));
    ASSERT_TRUE(strstr(err, "AArch64") != NULL);

    ASSERT_TRUE(xaot_link_manifest_init(&manifest, &x86));
    char *json = xaot_link_manifest_dump_json(&manifest);
    ASSERT_TRUE(json != NULL);
    ASSERT_TRUE(strstr(json, "\"simd_mode\": \"dispatch\"") != NULL);
    ASSERT_TRUE(strstr(json, "\"simd_features\": 22") != NULL);
    xr_free(json);
    xaot_link_manifest_free(&manifest);
    xaot_target_free(&powerpc64le);
    xaot_target_free(&loongarch64);
    xaot_target_free(&powerpc64);
    xaot_target_free(&x86_32);
    xaot_target_free(&arm32);
    xaot_target_free(&arm);
    xaot_target_free(&x86);
    passed++;
}

static void test_driver_consumes_imported_summary_payload_set(void) {
    char source_path[256];
    char cache_dir[256];
    XaotTarget target = {0};
    XaotBuildOptions options = {0};
    XaotBuildResult result;
    char *payload = NULL;
    const char *payloads[1];
    uint64_t imported_hash = 0;

    memset(&result, 0, sizeof(result));
    ASSERT_TRUE(write_temp_source(source_path, sizeof(source_path)));
    ASSERT_TRUE(xr_temp_dir_create("xray-xaot-target-plan-cache", cache_dir,
                                   sizeof(cache_dir)) == 0);
    ASSERT_TRUE(xaot_target_init(&target, NULL));
    payload = make_package_payload();
    ASSERT_TRUE(payload != NULL);
    payloads[0] = payload;
    ASSERT_TRUE(xg_imported_summary_hash_from_package_payloads(0, payloads, 1, &imported_hash));

    options.target = &target;
    options.profile = XAOT_BUILD_PROFILE_HOSTED;
    ASSERT_TRUE(install_native_target_profile(&options, &target));
    options.emit_global_evidence_dump = true;
    options.imported_summary_payloads = payloads;
    options.imported_summary_payload_count = 1;
    options.incremental_cache_dir = cache_dir;
    options.target_plan_workers = 8;

    ASSERT_TRUE(xaot_build_script(source_path, &options, &result) == 0);
    ASSERT_TRUE(dump_contains_import_hash(result.global_evidence_dump, imported_hash));
    ASSERT_TRUE(dump_contains_imported_package_link_dep(result.global_evidence_dump));
    ASSERT_TRUE(result.target_plan_cache.misses == 1);
    ASSERT_TRUE(result.target_plan_cache.published == 1);
    ASSERT_TRUE(result.target_plan_cache.workers == 1);

    xaot_build_result_free(&result);
    ASSERT_TRUE(xaot_build_script(source_path, &options, &result) == 0);
    ASSERT_TRUE(result.target_plan_cache.hits == 1);
    ASSERT_TRUE(result.target_plan_cache.misses == 0);
    ASSERT_TRUE(result.target_plan_cache.workers == 1);
    xaot_build_result_free(&result);
    ASSERT_TRUE(corrupt_first_xtp_cache_object(cache_dir));
    ASSERT_TRUE(xaot_build_script(source_path, &options, &result) == 0);
    ASSERT_TRUE(result.target_plan_cache.hits == 0);
    ASSERT_TRUE(result.target_plan_cache.misses == 1);
    ASSERT_TRUE(result.target_plan_cache.rejected == 1);
    ASSERT_TRUE(result.target_plan_cache.published == 1);
    xaot_build_result_free(&result);
    release_target_profile(&options);
    xaot_target_free(&target);
    xr_free(payload);
    xr_test_unlink(source_path);
    passed++;
}

static void test_driver_dumps_subject_bound_local_evidence(void) {
    char source_path[256];
    XaotTarget target = {0};
    XaotBuildOptions options = {0};
    XaotBuildResult result;

    memset(&result, 0, sizeof(result));
    ASSERT_TRUE(write_temp_source(source_path, sizeof(source_path)));
    ASSERT_TRUE(write_file_text(source_path, "fn first(input: Slice<u8>) -> u8 {\n"
                                             "    return input[0]\n"
                                             "}\n"));
    ASSERT_TRUE(xaot_target_init(&target, NULL));
    options.target = &target;
    options.profile = XAOT_BUILD_PROFILE_HOSTED;
    ASSERT_TRUE(install_native_target_profile(&options, &target));
    options.emit_local_evidence_dump = true;

    ASSERT_TRUE(xaot_build_script(source_path, &options, &result) == 0);
    ASSERT_TRUE(result.local_evidence_dump != NULL);
    ASSERT_TRUE(strstr(result.local_evidence_dump, "xi-evidence function=") != NULL);
    ASSERT_TRUE(strstr(result.local_evidence_dump, "revision=") != NULL);
    ASSERT_TRUE(strstr(result.local_evidence_dump, "subject=") != NULL);
    ASSERT_TRUE(strstr(result.local_evidence_dump, "producer=") != NULL);
    ASSERT_TRUE(strstr(result.local_evidence_dump, "source=") != NULL);
    ASSERT_TRUE(strstr(result.local_evidence_dump, "view-param function=first param=0") != NULL);
    ASSERT_TRUE(
        strstr(result.local_evidence_dump, "capability=read lifetime=caller complete=yes") != NULL);

    xaot_build_result_free(&result);
    release_target_profile(&options);
    xaot_target_free(&target);
    xr_test_unlink(source_path);
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
    ASSERT_TRUE(install_native_target_profile(&options, &target));
    options.imported_summary_payloads = payloads;
    options.imported_summary_payload_count = 1;

    ASSERT_TRUE(xaot_build_script("/tmp/xray-xaot-driver-missing.xr", &options, &result) != 0);
    ASSERT_TRUE(result.n_sources == 0);

    xaot_build_result_free(&result);
    release_target_profile(&options);
    xaot_target_free(&target);
    passed++;
}

static void test_driver_analyzes_aggregate_layout_with_selected_target(void) {
    char source_path[256];
    XaotTarget target = {0};
    XaotBuildOptions options = {0};
    XaotBuildResult result;

    snprintf(source_path, sizeof(source_path), "/tmp/xray-xaot-target-layout-%ld.xr",
             (long) xr_test_getpid());
    ASSERT_TRUE(write_file_text(
        source_path, "import mem\n"
                     "struct TargetPair {\n"
                     "    ptr: Ptr<u8>\n"
                     "    size: usize\n"
                     "}\n"
                     "comptime {\n"
                     "    compile_assert(mem.sizeOf<TargetPair>() == 8)\n"
                     "    compile_assert(mem.alignOf<TargetPair>() == 4)\n"
                     "    compile_assert(mem.offsetOf<TargetPair>(\"size\") == 4)\n"
                     "}\n"
                     "fn target_pair_size() -> i64 { return mem.sizeOf<TargetPair>() }\n"));
    ASSERT_TRUE(xaot_target_init(&target, "riscv32-freestanding-none"));
    options.target = &target;
    options.profile = XAOT_BUILD_PROFILE_FREESTANDING;
    options.emit_plan_dump = true;
    memset(&result, 0, sizeof(result));

    ASSERT_TRUE(xaot_build_script(source_path, &options, &result) != 0);
    ASSERT_TRUE(result.plan_dump == NULL);
    ASSERT_TRUE(result.n_sources == 0);

    xaot_build_result_free(&result);
    xaot_target_free(&target);
    xr_test_unlink(source_path);
    passed++;
}

static void test_driver_analyzes_riscv64_layout_with_selected_target(void) {
    char source_path[256];
    XaotTarget target = {0};
    XaotBuildOptions options = {0};
    XaotBuildResult result;

    snprintf(source_path, sizeof(source_path), "/tmp/xray-xaot-riscv64-layout-%ld.xr",
             (long) xr_test_getpid());
    ASSERT_TRUE(write_file_text(
        source_path, "import mem\n"
                     "struct TargetPair {\n"
                     "    ptr: Ptr<u8>\n"
                     "    size: usize\n"
                     "}\n"
                     "comptime {\n"
                     "    compile_assert(mem.sizeOf<TargetPair>() == 16)\n"
                     "    compile_assert(mem.alignOf<TargetPair>() == 8)\n"
                     "    compile_assert(mem.offsetOf<TargetPair>(\"size\") == 8)\n"
                     "}\n"
                     "fn target_pair_size() -> i64 { return mem.sizeOf<TargetPair>() }\n"));
    ASSERT_TRUE(xaot_target_init(&target, "riscv64-freestanding-none"));
    options.target = &target;
    options.profile = XAOT_BUILD_PROFILE_FREESTANDING;
    options.emit_plan_dump = true;
    memset(&result, 0, sizeof(result));

    ASSERT_TRUE(xaot_build_script(source_path, &options, &result) != 0);
    ASSERT_TRUE(result.plan_dump == NULL);
    ASSERT_TRUE(result.n_sources == 0);

    xaot_build_result_free(&result);
    xaot_target_free(&target);
    xr_test_unlink(source_path);
    passed++;
}

static bool manifest_has_define(const XaotLinkManifest *manifest, const char *needle) {
    if (!manifest || !needle)
        return false;
    for (uint32_t i = 0; i < manifest->n_defines; i++) {
        if (manifest->defines[i] && strcmp(manifest->defines[i], needle) == 0)
            return true;
    }
    return false;
}

static bool manifest_has_runtime_cap(const XaotLinkManifest *manifest, const char *needle) {
    if (!manifest || !needle)
        return false;
    for (uint32_t i = 0; i < manifest->n_runtime_caps; i++) {
        if (manifest->runtime_caps[i] && strcmp(manifest->runtime_caps[i], needle) == 0)
            return true;
    }
    return false;
}

static bool dump_line_contains(const char *dump, const char *anchor, const char *needle) {
    const char *line;
    const char *end;
    const char *match;
    if (!dump || !anchor || !needle || !(line = strstr(dump, anchor)))
        return false;
    end = strchr(line, '\n');
    match = strstr(line, needle);
    return match && (!end || match < end);
}

static void test_spawn_target_contributes_artifact_runtime_capabilities(void) {
    char source_path[256];
    XaotTarget target = {0};
    XaotBuildOptions options = {0};
    XaotBuildResult result;

    snprintf(source_path, sizeof(source_path), "/tmp/xray-xaot-spawn-caps-%ld.xr",
             (long) xr_test_getpid());
    ASSERT_TRUE(write_file_text(source_path, "import time\n"
                                             "fn worker() {\n"
                                             "    time.sleep(1)\n"
                                             "}\n"
                                             "await go worker()\n"));
    ASSERT_TRUE(xaot_target_init(&target, NULL));
    options.target = &target;
    options.profile = XAOT_BUILD_PROFILE_HOSTED;
    ASSERT_TRUE(install_native_target_profile(&options, &target));
    options.emit_plan_dump = true;
    memset(&result, 0, sizeof(result));

    ASSERT_TRUE(xaot_build_script(source_path, &options, &result) == 0);
    ASSERT_TRUE(manifest_has_runtime_cap(&result.link_manifest, "timer"));
    ASSERT_TRUE(result.plan_dump != NULL);
    ASSERT_TRUE(dump_line_contains(result.plan_dump, "name=worker", "reachable=1"));

    xaot_build_result_free(&result);
    release_target_profile(&options);
    xaot_target_free(&target);
    xr_test_unlink(source_path);
    passed++;
}

static void test_driver_hosted_fragment_borrows_runtime_ownership(void) {
    char source_path[256];
    XaotTarget target = {0};
    XaotBuildOptions options = {0};
    XaotBuildResult result;

    memset(&result, 0, sizeof(result));
    ASSERT_TRUE(write_temp_source(source_path, sizeof(source_path)));
    ASSERT_TRUE(xaot_target_init(&target, NULL));
    options.target = &target;
    options.profile = XAOT_BUILD_PROFILE_HOSTED;
    ASSERT_TRUE(install_native_target_profile(&options, &target));
    options.artifact_kind = XAOT_ARTIFACT_HOSTED_FRAGMENT;

    ASSERT_TRUE(xaot_build_script(source_path, &options, &result) == 0);
    ASSERT_TRUE(result.n_sources > 0);
    ASSERT_TRUE(!manifest_has_define(&result.link_manifest, "XRT_IMPL"));
    for (int i = 0; i < result.n_sources; i++) {
        const char *source = result.sources[i].c_source;
        ASSERT_TRUE(source != NULL);
        ASSERT_TRUE(strstr(source, "#define XRT_IMPL") == NULL);
        ASSERT_TRUE(strstr(source, "i64 main(") == NULL);
        ASSERT_TRUE(strstr(source, "xrt_shared_lib_ctor") == NULL);
    }

    xaot_build_result_free(&result);
    release_target_profile(&options);
    xaot_target_free(&target);
    xr_test_unlink(source_path);
    passed++;
}

static void test_driver_validates_freestanding_runtime_provider(void) {
    char source_path[256];
    XaotTarget target = {0};
    XaotBuildOptions options = {0};
    XaotBuildResult result;
    XaotTargetCapabilityProvider provider = {
        .abi_version = XAOT_PROVIDER_ABI_VERSION,
        .provided_capability_bits = XG_CAP_COROUTINE | XG_CAP_TASK,
        .hook_bits = XAOT_PROVIDER_HOOK_TASK_ALLOC | XAOT_PROVIDER_HOOK_SUBMIT |
                     XAOT_PROVIDER_HOOK_PARK_WAKE | XAOT_PROVIDER_HOOK_EXECUTOR_PUMP,
        .target_metadata_hash = 0x197195,
    };

    snprintf(source_path, sizeof(source_path), "/tmp/xray-xaot-provider-%ld.xr",
             (long) xr_test_getpid());
    ASSERT_TRUE(write_file_text(source_path, "fn worker() -> i64 {\n"
                                             "    return 1\n"
                                             "}\n"
                                             "await go worker()\n"));
    ASSERT_TRUE(xaot_target_init(&target, NULL));
    options.target = &target;
    options.profile = XAOT_BUILD_PROFILE_FREESTANDING;

    memset(&result, 0, sizeof(result));
    ASSERT_TRUE(xaot_build_script(source_path, &options, &result) != 0);
    xaot_build_result_free(&result);

    provider.hook_bits &= ~XAOT_PROVIDER_HOOK_EXECUTOR_PUMP;
    options.capability_provider = &provider;
    memset(&result, 0, sizeof(result));
    ASSERT_TRUE(xaot_build_script(source_path, &options, &result) != 0);
    xaot_build_result_free(&result);

    provider.hook_bits |= XAOT_PROVIDER_HOOK_EXECUTOR_PUMP;
    memset(&result, 0, sizeof(result));
    ASSERT_TRUE(xaot_build_script(source_path, &options, &result) != 0);
    xaot_build_result_free(&result);

    xaot_target_free(&target);
    xr_test_unlink(source_path);
    passed++;
}

static void test_driver_auto_discovers_package_summary_payloads(void) {
    char root[XR_TEST_PATH_MAX];
    char home_dir[XR_TEST_PATH_MAX];
    char entry_source[XR_TEST_PATH_MAX];
    char cache_dir[XR_TEST_PATH_MAX];
    XaotTarget target = {0};
    XaotBuildOptions options = {0};
    XaotBuildResult result;
    char *payload = NULL;
    const char *payloads[1];
    uint64_t imported_hash = 0;
    char *old_home;

    memset(&result, 0, sizeof(result));
    snprintf(root, sizeof(root), "/tmp/xray-xaot-driver-auto-%ld", (long) xr_test_getpid());
    snprintf(home_dir, sizeof(home_dir), "%s/home", root);
    snprintf(entry_source, sizeof(entry_source), "%s/entry.xr", root);
    snprintf(cache_dir, sizeof(cache_dir), "%s/cache/aot/native", root);
    payload = install_package_payload(home_dir, cache_dir, "codex/pkg",
                                      "fn package_value() -> i64 {\n"
                                      "    return 5\n"
                                      "}\n");
    ASSERT_TRUE(payload != NULL);
    ASSERT_TRUE(write_file_text(entry_source, "import \"codex/pkg\" as pkg\n"
                                              "fn value() -> i64 {\n"
                                              "    return 7\n"
                                              "}\n"));
    payloads[0] = payload;
    ASSERT_TRUE(xg_imported_summary_hash_from_package_payloads(0, payloads, 1, &imported_hash));
    old_home = dup_env_value("HOME");
    xr_test_setenv("HOME", home_dir, 1);

    ASSERT_TRUE(xaot_target_init(&target, NULL));
    options.target = &target;
    options.profile = XAOT_BUILD_PROFILE_HOSTED;
    ASSERT_TRUE(install_native_target_profile(&options, &target));
    options.emit_global_evidence_dump = true;
    options.incremental_cache_dir = cache_dir;

    ASSERT_TRUE(xaot_build_script(entry_source, &options, &result) == 0);
    ASSERT_TRUE(dump_contains_import_hash(result.global_evidence_dump, imported_hash));
    ASSERT_TRUE(dump_contains_imported_package_link_dep(result.global_evidence_dump));
    xaot_build_result_free(&result);
    release_target_profile(&options);
    xaot_target_free(&target);
    restore_env_value("HOME", old_home);
    xr_free(payload);
    xr_test_unlink(entry_source);
    passed++;
}

static void run_driver_auto_discovers_multiple_package_summary_payloads(
    bool parallel_probe) {
    char root[XR_TEST_PATH_MAX];
    char home_dir[XR_TEST_PATH_MAX];
    char entry_source[XR_TEST_PATH_MAX];
    char cache_dir[XR_TEST_PATH_MAX];
    char cache_dir_dual[XR_TEST_PATH_MAX];
    char cache_dir_wide[XR_TEST_PATH_MAX];
    char edited_source[XR_TEST_PATH_MAX];
    XaotTarget target = {0};
    XaotBuildOptions options = {0};
    XaotBuildResult result;
    char *payload_a = NULL;
    char *payload_b = NULL;
    const char *payloads[2];
    uint64_t imported_hash = 0;
    char *old_home;

    memset(&result, 0, sizeof(result));
    snprintf(root, sizeof(root), "/tmp/xray-xaot-driver-auto-multi-%ld", (long) xr_test_getpid());
    snprintf(home_dir, sizeof(home_dir), "%s/home", root);
    snprintf(entry_source, sizeof(entry_source), "%s/entry.xr", root);
    snprintf(cache_dir, sizeof(cache_dir), "%s/cache/aot/native", root);
    snprintf(cache_dir_dual, sizeof(cache_dir_dual),
             "%s/cache-dual/aot/native", root);
    snprintf(cache_dir_wide, sizeof(cache_dir_wide),
             "%s/cache-wide/aot/native", root);
    payload_a = install_package_payload(home_dir, cache_dir, "codex/pkga",
                                        "fn package_a() -> i64 {\n"
                                        "    return 11\n"
                                        "}\n");
    payload_b = install_package_payload(home_dir, cache_dir, "codex/pkgb",
                                        "fn package_b() -> i64 {\n"
                                        "    return 13\n"
                                        "}\n");
    ASSERT_TRUE(payload_a != NULL);
    ASSERT_TRUE(payload_b != NULL);
    ASSERT_TRUE(write_global_payload_to_cache(cache_dir_dual, payload_a));
    ASSERT_TRUE(write_global_payload_to_cache(cache_dir_dual, payload_b));
    ASSERT_TRUE(write_global_payload_to_cache(cache_dir_wide, payload_a));
    ASSERT_TRUE(write_global_payload_to_cache(cache_dir_wide, payload_b));
    ASSERT_TRUE(write_file_text(entry_source, "import \"codex/pkga\" as a\n"
                                              "import \"codex/pkgb\" as b\n"
                                              "fn value() -> i64 {\n"
                                              "    return 17\n"
                                              "}\n"));
    payloads[0] = payload_a;
    payloads[1] = payload_b;
    ASSERT_TRUE(xg_imported_summary_hash_from_package_payloads(0, payloads, 2, &imported_hash));
    old_home = dup_env_value("HOME");
    xr_test_setenv("HOME", home_dir, 1);

    ASSERT_TRUE(xaot_target_init(&target, NULL));
    options.target = &target;
    options.profile = XAOT_BUILD_PROFILE_HOSTED;
    ASSERT_TRUE(install_native_target_profile(&options, &target));
    options.emit_global_evidence_dump = true;
    options.incremental_cache_dir = cache_dir;
    if (!parallel_probe) {
        ASSERT_TRUE(xaot_build_script(entry_source, &options, &result) == 0);
        ASSERT_TRUE(dump_contains_import_hash(result.global_evidence_dump,
                                              imported_hash));
    } else {
        XaotModuleSummaryCacheStats cold[3];
        XaotModuleSummaryCacheStats warm[3];
        XaotModuleSummaryCacheStats edited[3];
        const char *cache_roots[3] = {
            cache_dir, cache_dir_dual, cache_dir_wide,
        };
        const uint32_t worker_limits[3] = {1u, 2u, 8u};
        const uint32_t expected_workers[3] = {1u, 2u, 2u};

        for (uint32_t run = 0; run < 3u; run++) {
            options.incremental_cache_dir = cache_roots[run];
            options.target_plan_workers = worker_limits[run];
            ASSERT_TRUE(xaot_build_script(entry_source, &options, &result) == 0);
            cold[run] = result.module_summary_cache;
            ASSERT_TRUE(cold[run].workers == expected_workers[run]);
            ASSERT_TRUE(cold[run].tasks == 3);
            ASSERT_TRUE(cold[run].hits == 0);
            ASSERT_TRUE(cold[run].misses == 3);
            ASSERT_TRUE(cold[run].recomputed_modules == 3);
            ASSERT_TRUE(cold[run].published == 3);
            ASSERT_TRUE(cold[run].merged_modules == 3);
            if (run > 0) {
                ASSERT_TRUE(xr_fingerprint_equal(
                    cold[run].artifact_order_fingerprint,
                    cold[0].artifact_order_fingerprint));
                ASSERT_TRUE(xr_fingerprint_equal(
                    cold[run].publish_order_fingerprint,
                    cold[0].publish_order_fingerprint));
            }
            ASSERT_TRUE(result.target_plan_cache.workers ==
                        (run == 0 ? 1u : (run == 1 ? 2u : 3u)));
            xaot_build_result_free(&result);
        }

        for (uint32_t run = 0; run < 3u; run++) {
            options.incremental_cache_dir = cache_roots[run];
            options.target_plan_workers = worker_limits[run];
            ASSERT_TRUE(xaot_build_script(entry_source, &options, &result) == 0);
            warm[run] = result.module_summary_cache;
            ASSERT_TRUE(warm[run].workers == expected_workers[run]);
            ASSERT_TRUE(warm[run].tasks == 3);
            ASSERT_TRUE(warm[run].hits == 3);
            ASSERT_TRUE(warm[run].misses == 0);
            ASSERT_TRUE(warm[run].recomputed_modules == 0);
            ASSERT_TRUE(warm[run].published == 0);
            ASSERT_TRUE(warm[run].merged_modules == 3);
            ASSERT_TRUE(xr_fingerprint_equal(
                warm[run].artifact_order_fingerprint,
                cold[0].artifact_order_fingerprint));
            if (run > 0)
                ASSERT_TRUE(xr_fingerprint_equal(
                    warm[run].publish_order_fingerprint,
                    warm[0].publish_order_fingerprint));
            xaot_build_result_free(&result);
        }

        ASSERT_TRUE(write_package_source(
            home_dir, "codex/pkga",
            "fn package_a() -> i64 {\n"
            "    return 31\n"
            "}\n",
            edited_source, sizeof(edited_source)));
        for (uint32_t run = 0; run < 3u; run++) {
            options.incremental_cache_dir = cache_roots[run];
            options.target_plan_workers = worker_limits[run];
            ASSERT_TRUE(xaot_build_script(entry_source, &options, &result) == 0);
            edited[run] = result.module_summary_cache;
            ASSERT_TRUE(edited[run].workers == expected_workers[run]);
            ASSERT_TRUE(edited[run].tasks == 3);
            ASSERT_TRUE(edited[run].hits < 3);
            ASSERT_TRUE(edited[run].recomputed_modules > 0);
            ASSERT_TRUE(edited[run].recomputed_modules < 3);
            ASSERT_TRUE(edited[run].published ==
                        edited[run].recomputed_modules);
            ASSERT_TRUE(edited[run].merged_modules == 3);
            ASSERT_TRUE(!xr_fingerprint_equal(
                edited[run].artifact_order_fingerprint,
                cold[0].artifact_order_fingerprint));
            if (run > 0) {
                ASSERT_TRUE(edited[run].hits == edited[0].hits);
                ASSERT_TRUE(edited[run].recomputed_modules ==
                            edited[0].recomputed_modules);
                ASSERT_TRUE(xr_fingerprint_equal(
                    edited[run].artifact_order_fingerprint,
                    edited[0].artifact_order_fingerprint));
                ASSERT_TRUE(xr_fingerprint_equal(
                    edited[run].publish_order_fingerprint,
                    edited[0].publish_order_fingerprint));
            }
            xaot_build_result_free(&result);
        }

        options.incremental_cache_dir = cache_dir;
        options.target_plan_workers = 1;
        XrTargetPlanCancellationToken cancellation;
        xr_target_plan_cancellation_token_init(&cancellation);
        xr_target_plan_cancellation_token_request(&cancellation);
        options.target_plan_cancellation = &cancellation;
        options.incremental_cache_rebuild = false;
        ASSERT_TRUE(xaot_build_script(entry_source, &options, &result) != 0);
        ASSERT_TRUE(result.target_plan_cache.cancelled == 3);
        ASSERT_TRUE(result.n_sources == 0);
        options.target_plan_cancellation = NULL;
    }

    xaot_build_result_free(&result);
    release_target_profile(&options);
    xaot_target_free(&target);
    restore_env_value("HOME", old_home);
    xr_free(payload_a);
    xr_free(payload_b);
    xr_test_unlink(entry_source);
    passed++;
}

static void test_driver_auto_discovers_multiple_package_summary_payloads(void) {
    run_driver_auto_discovers_multiple_package_summary_payloads(false);
}

static void test_driver_parallel_target_plans_are_canonical(void) {
    run_driver_auto_discovers_multiple_package_summary_payloads(true);
}

static void test_driver_auto_discovers_package_dependency_summary_payload(void) {
    char root[XR_TEST_PATH_MAX];
    char home_dir[XR_TEST_PATH_MAX];
    char entry_source[XR_TEST_PATH_MAX];
    char cache_dir[XR_TEST_PATH_MAX];
    char pkg_a_source[XR_TEST_PATH_MAX];
    char pkg_b_source[XR_TEST_PATH_MAX];
    XaotTarget target = {0};
    XaotBuildOptions options = {0};
    XaotBuildResult result;
    char *payload_ab = NULL;
    char *payload_b = NULL;
    const char *ordered_canonicals[2] = {"codex/pkgb", "codex/pkga"};
    const char *ordered_sources[2];
    const char *payloads[1];
    uint64_t imported_hash = 0;
    char *old_home;

    memset(&result, 0, sizeof(result));
    snprintf(root, sizeof(root), "/tmp/xray-xaot-driver-auto-graph-%ld", (long) xr_test_getpid());
    snprintf(home_dir, sizeof(home_dir), "%s/home", root);
    snprintf(entry_source, sizeof(entry_source), "%s/entry.xr", root);
    snprintf(cache_dir, sizeof(cache_dir), "%s/cache/aot/native", root);
    ASSERT_TRUE(write_package_source(home_dir, "codex/pkgb",
                                     "fn package_b() -> i64 {\n"
                                     "    return 23\n"
                                     "}\n",
                                     pkg_b_source, sizeof(pkg_b_source)));
    ASSERT_TRUE(write_package_source(home_dir, "codex/pkga",
                                     "import \"codex/pkgb\" as b\n"
                                     "fn package_a() -> i64 {\n"
                                     "    return 19\n"
                                     "}\n",
                                     pkg_a_source, sizeof(pkg_a_source)));
    ordered_sources[0] = pkg_b_source;
    ordered_sources[1] = pkg_a_source;
    payload_ab = make_package_payload_for_ordered_sources(ordered_canonicals, ordered_sources, 2);
    payload_b = make_package_payload_for_source("codex/pkgb", pkg_b_source);
    ASSERT_TRUE(payload_ab != NULL);
    ASSERT_TRUE(payload_b != NULL);
    ASSERT_TRUE(write_global_payload_to_cache(cache_dir, payload_ab));
    ASSERT_TRUE(write_global_payload_to_cache(cache_dir, payload_b));
    ASSERT_TRUE(write_file_text(entry_source, "import \"codex/pkga\" as a\n"
                                              "fn value() -> i64 {\n"
                                              "    return 29\n"
                                              "}\n"));
    payloads[0] = payload_ab;
    ASSERT_TRUE(xg_imported_summary_hash_from_package_payloads(0, payloads, 1, &imported_hash));
    old_home = dup_env_value("HOME");
    xr_test_setenv("HOME", home_dir, 1);

    ASSERT_TRUE(xaot_target_init(&target, NULL));
    options.target = &target;
    options.profile = XAOT_BUILD_PROFILE_HOSTED;
    ASSERT_TRUE(install_native_target_profile(&options, &target));
    options.emit_global_evidence_dump = true;
    options.incremental_cache_dir = cache_dir;

    ASSERT_TRUE(xaot_build_script(entry_source, &options, &result) == 0);
    ASSERT_TRUE(dump_contains_import_hash(result.global_evidence_dump, imported_hash));

    xaot_build_result_free(&result);
    release_target_profile(&options);
    xaot_target_free(&target);
    restore_env_value("HOME", old_home);
    xr_free(payload_ab);
    xr_free(payload_b);
    xr_test_unlink(entry_source);
    passed++;
}

static void test_driver_requires_exact_typed_entry_authority(void) {
    char source_path[256];
    char *physical_root = NULL;
    XaotTarget target = {0};
    XaotBuildOptions options = {0};
    XaotBuildResult result = {0};
    XrModuleIdentityAuthority script_authority = {0};

    ASSERT_TRUE(write_temp_source(source_path, sizeof(source_path)));
    ASSERT_TRUE(xaot_target_init(&target, NULL));
    options.target = &target;
    options.profile = XAOT_BUILD_PROFILE_HOSTED;
    ASSERT_TRUE(install_native_target_profile(&options, &target));
    ASSERT_TRUE(xr_module_identity_script_authority_from_source(
        source_path, &script_authority, &physical_root));

    options.entry_module_authority = script_authority;
    ASSERT_TRUE(xaot_build(source_path, &options, &result) == 0);
    xaot_build_result_free(&result);

    options.entry_module_authority = (XrModuleIdentityAuthority) {
        .kind = XR_MODULE_IDENTITY_PROJECT,
        .namespace_id = "identity-fixture",
        .physical_root = physical_root,
    };
    ASSERT_TRUE(xaot_build(source_path, &options, &result) == 0);
    xaot_build_result_free(&result);

    options.entry_module_authority = (XrModuleIdentityAuthority) {
        .kind = XR_MODULE_IDENTITY_PACKAGE,
        .namespace_id = "xray/identity-fixture@1.0.0",
        .physical_root = physical_root,
    };
    ASSERT_TRUE(xaot_build(source_path, &options, &result) == 0);
    xaot_build_result_free(&result);

    const XrModuleIdentityAuthority rejected[] = {
        {0},
        {
            .kind = XR_MODULE_IDENTITY_PROJECT,
            .namespace_id = "xray/identity-fixture",
            .physical_root = physical_root,
        },
        {
            .kind = XR_MODULE_IDENTITY_PACKAGE,
            .namespace_id = "identity-fixture@1.0.0",
            .physical_root = physical_root,
        },
        {
            .kind = XR_MODULE_IDENTITY_PACKAGE,
            .namespace_id = "xray/identity-fixture@^1.0.0",
            .physical_root = physical_root,
        },
        {
            .kind = XR_MODULE_IDENTITY_SCRIPT,
            .namespace_id = "identity-fixture",
            .physical_root = physical_root,
        },
        {
            .kind = XR_MODULE_IDENTITY_PROJECT,
            .namespace_id = "identity-fixture",
            .physical_root = NULL,
        },
    };
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        options.entry_module_authority = rejected[i];
        ASSERT_TRUE(xaot_build(source_path, &options, &result) != 0);
        ASSERT_TRUE(result.n_sources == 0);
        xaot_build_result_free(&result);
    }

    release_target_profile(&options);
    xaot_target_free(&target);
    xr_free(physical_root);
    xr_test_unlink(source_path);
    passed++;
}

static bool span_contains(const char *begin, const char *end, const char *needle) {
    const char *match = begin && end && needle ? strstr(begin, needle) : NULL;
    return match && match < end;
}

static const char *find_function_definition(const char *source, const char *name_fragment) {
    const char *cursor = source;
    while (cursor && (cursor = strstr(cursor, name_fragment)) != NULL) {
        const char *line_end = strchr(cursor, '\n');
        const char *body = strstr(cursor, ") {");
        if (body && (!line_end || body < line_end))
            return cursor;
        cursor++;
    }
    return NULL;
}

static void test_driver_direct_i64_call_consumes_target_plan(void) {
    char source_path[256];
    XaotTarget target = {0};
    XaotBuildOptions options = {0};
    XaotBuildResult result = {0};
    snprintf(source_path, sizeof(source_path), "/tmp/xray-xaot-direct-i64-%ld.xr",
             (long) xr_test_getpid());
    ASSERT_TRUE(write_file_text(source_path,
                                "fn add1(value: i64) -> i64 { return value + 1 }\n"
                                "fn root() -> i64 { return add1(41) }\n"
                                "print(root())\n"));
    ASSERT_TRUE(xaot_target_init(&target, NULL));
    options.target = &target;
    options.profile = XAOT_BUILD_PROFILE_HOSTED;
    ASSERT_TRUE(install_native_target_profile(&options, &target));
    options.emit_plan_dump = true;

    ASSERT_TRUE(xaot_build_script(source_path, &options, &result) == 0);
    ASSERT_TRUE(result.n_sources == 1 && result.sources && result.sources[0].c_source);
    ASSERT_TRUE(result.plan_dump != NULL);
    ASSERT_TRUE(dump_line_contains(result.plan_dump, "name=add1", "abi_owner=target-plan"));
    ASSERT_TRUE(dump_line_contains(result.plan_dump, "name=root", "abi_owner=legacy"));

    const char *source = result.sources[0].c_source;
    const char *root = find_function_definition(source, "_root_");
    const char *root_end = root ? strstr(root, "\n}\n") : NULL;
    ASSERT_TRUE(root != NULL && root_end != NULL);
    const char *call_site = root ? strstr(root, "_add1_") : NULL;
    const char *call_line_end = call_site ? strchr(call_site, '\n') : NULL;
    ASSERT_TRUE(call_site != NULL && call_site < root_end && call_line_end != NULL &&
                call_line_end <= root_end &&
                span_contains(call_site, call_line_end, "(NULL, INT64_C(41))"));
    ASSERT_TRUE(!span_contains(call_site, call_line_end, "XrValue") &&
                !span_contains(call_site, call_line_end, "_boxed") &&
                !span_contains(call_site, call_line_end, "XR_FROM_INT") &&
                !span_contains(call_site, call_line_end, "XR_TO_INT") &&
                !span_contains(call_site, call_line_end, "xrt_call"));
    ASSERT_TRUE(!span_contains(root, root_end, "_boxed") &&
                !span_contains(root, root_end, "XR_FROM_INT") &&
                !span_contains(root, root_end, "XR_TO_INT") &&
                !span_contains(root, root_end, "xrt_call"));

    xaot_build_result_free(&result);
    release_target_profile(&options);
    xaot_target_free(&target);
    xr_test_unlink(source_path);
    passed++;
}

int main(void) {
    const char *filter = getenv("XRAY_TEST_FILTER");
    if (filter && strcmp(filter, "target_plan_cache") == 0) {
        test_driver_consumes_imported_summary_payload_set();
        printf("%d passed, %d failed\n", passed, failed);
        return failed ? 1 : 0;
    }
    if (filter && strcmp(filter, "parallel_target_plans") == 0) {
        test_driver_parallel_target_plans_are_canonical();
        printf("%d passed, %d failed\n", passed, failed);
        return failed ? 1 : 0;
    }
    if (filter && strcmp(filter, "identity_authority") == 0) {
        test_driver_requires_exact_typed_entry_authority();
        printf("%d passed, %d failed\n", passed, failed);
        return failed ? 1 : 0;
    }
    if (filter && strcmp(filter, "direct_i64_target_plan") == 0) {
        test_driver_direct_i64_call_consumes_target_plan();
        printf("%d passed, %d failed\n", passed, failed);
        return failed ? 1 : 0;
    }
    test_target_simd_plan_is_explicit_and_fail_closed();
    test_driver_consumes_imported_summary_payload_set();
    test_driver_dumps_subject_bound_local_evidence();
    test_driver_rejects_invalid_imported_summary_payload_set();
    test_driver_analyzes_aggregate_layout_with_selected_target();
    test_driver_analyzes_riscv64_layout_with_selected_target();
    test_spawn_target_contributes_artifact_runtime_capabilities();
    test_driver_hosted_fragment_borrows_runtime_ownership();
    test_driver_validates_freestanding_runtime_provider();
    test_driver_auto_discovers_package_summary_payloads();
    test_driver_auto_discovers_multiple_package_summary_payloads();
    test_driver_auto_discovers_package_dependency_summary_payload();
    test_driver_requires_exact_typed_entry_authority();
    test_driver_direct_i64_call_consumes_target_plan();
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
