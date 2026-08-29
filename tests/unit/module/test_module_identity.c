/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_module_identity.c - Relocatable module identity regression gates
 */

#include "../test_framework.h"
#include "base/xmalloc.h"
#include "base/xhash.h"
#include "analysis/xglobal_producer.h"
#include "ir/xi.h"
#include "ir/xi_module.h"
#include "module/xmodule.h"
#include "module/xmodule_graph.h"
#include "module/xmodule_identity.h"
#include "module/xmodule_resolver.h"
#include "module/xlockfile.h"
#include "toolchain/xcompiler_session.h"
#include "xray_vm.h"
#include <string.h>

static void require_identity(const XrModuleIdentityAuthority *authority, const char *source,
                             const char *expected_identity, const char *expected_logical) {
    char *identity = NULL;
    char *logical = NULL;
    ASSERT_TRUE(xr_module_identity_from_source(authority, source, &identity, &logical));
    ASSERT_STR_EQ(identity, expected_identity);
    ASSERT_STR_EQ(logical, expected_logical);
    xr_free(identity);
    xr_free(logical);
}

static char *require_logical_identity(const XrModuleIdentityAuthority *authority,
                                      const char *logical_path) {
    char *identity = NULL;
    if (!xr_module_identity_from_logical(authority, logical_path, &identity)) {
        xr_free(identity);
        return NULL;
    }
    return identity;
}

TEST(project_identity_survives_checkout_relocation) {
    XrModuleIdentityAuthority first = {
        .kind = XR_MODULE_IDENTITY_PROJECT,
        .namespace_id = "acme-app",
        .physical_root = "C:/work/first/acme",
    };
    XrModuleIdentityAuthority second = {
        .kind = XR_MODULE_IDENTITY_PROJECT,
        .namespace_id = "acme-app",
        .physical_root = "D:/relocated/acme",
    };
    require_identity(&first, "C:/work/first/acme/src/net/client.xr",
                     "module-id-v1:kind=7:project:namespace=8:acme-app:path=17:src/net/client.xr",
                     "src/net/client.xr");
    require_identity(&second, "D:\\relocated\\acme\\src\\net\\client.xr",
                     "module-id-v1:kind=7:project:namespace=8:acme-app:path=17:src/net/client.xr",
                     "src/net/client.xr");
}

TEST(project_namespace_is_part_of_identity) {
    XrModuleIdentityAuthority authority = {
        .kind = XR_MODULE_IDENTITY_PROJECT,
        .namespace_id = "other-app",
        .physical_root = "/checkout/app",
    };
    require_identity(&authority, "/checkout/app/src/main.xr",
                     "module-id-v1:kind=7:project:namespace=9:other-app:path=11:src/main.xr",
                     "src/main.xr");
}

TEST(package_identity_includes_locked_version) {
    XrModuleIdentityAuthority authority = {
        .kind = XR_MODULE_IDENTITY_PACKAGE,
        .namespace_id = "acme/json@2.4.1",
        .physical_root = "/cache/acme/json/2.4.1",
    };
    require_identity(&authority, "/cache/acme/json/2.4.1/src/main.xr",
                     "module-id-v1:kind=7:package:namespace=15:acme/json@2.4.1:path=11:src/main.xr",
                     "src/main.xr");
}

TEST(source_outside_authority_fails_closed) {
    XrModuleIdentityAuthority authority = {
        .kind = XR_MODULE_IDENTITY_PROJECT,
        .namespace_id = "acme-app",
        .physical_root = "/checkout/app",
    };
    char *identity = (char *) 1;
    char *logical = (char *) 1;
    ASSERT_FALSE(xr_module_identity_from_source(&authority, "/checkout/app-copy/main.xr",
                                                &identity, &logical));
    ASSERT_NULL(identity);
    ASSERT_NULL(logical);
}

static void require_identity_rejected(const XrModuleIdentityAuthority *authority,
                                      const char *source) {
    char *identity = (char *) 1;
    char *logical = (char *) 1;
    ASSERT_FALSE(xr_module_identity_from_source(authority, source, &identity, &logical));
    ASSERT_NULL(identity);
    ASSERT_NULL(logical);
}

TEST(relative_physical_locator_fails_closed) {
    XrModuleIdentityAuthority relative_root = {
        .kind = XR_MODULE_IDENTITY_PROJECT,
        .namespace_id = "acme-app",
        .physical_root = "checkout/app",
    };
    require_identity_rejected(&relative_root, "checkout/app/src/main.xr");
    relative_root.physical_root = "/checkout/app";
    require_identity_rejected(&relative_root, "checkout/app/src/main.xr");
}

TEST(posix_drive_and_unc_authorities_are_absolute) {
    XrModuleIdentityAuthority posix = {
        .kind = XR_MODULE_IDENTITY_SCRIPT,
        .physical_root = "/checkout/app",
    };
    require_identity(&posix, "/checkout/app/main.xr",
                     "module-id-v1:kind=6:script:namespace=0::path=7:main.xr", "main.xr");
    XrModuleIdentityAuthority drive = {
        .kind = XR_MODULE_IDENTITY_PROJECT,
        .namespace_id = "acme-app",
        .physical_root = "C:\\checkout\\app",
    };
    require_identity(&drive, "C:\\checkout\\app\\main.xr",
                     "module-id-v1:kind=7:project:namespace=8:acme-app:path=7:main.xr", "main.xr");
    XrModuleIdentityAuthority unc = {
        .kind = XR_MODULE_IDENTITY_PROJECT,
        .namespace_id = "acme-app",
        .physical_root = "\\\\server\\share\\app",
    };
    require_identity(&unc, "\\\\server\\share\\app\\main.xr",
                     "module-id-v1:kind=7:project:namespace=8:acme-app:path=7:main.xr", "main.xr");
}

TEST(namespace_injection_fails_closed) {
    XrModuleIdentityAuthority authority = {
        .kind = XR_MODULE_IDENTITY_PROJECT,
        .namespace_id = "C:\\checkout\\app",
        .physical_root = "/checkout/app",
    };
    require_identity_rejected(&authority, "/checkout/app/main.xr");
    authority.namespace_id = "..";
    require_identity_rejected(&authority, "/checkout/app/main.xr");
    authority.kind = XR_MODULE_IDENTITY_PACKAGE;
    authority.namespace_id = "acme//json@1.0.0";
    require_identity_rejected(&authority, "/checkout/app/main.xr");
    authority.namespace_id = "acme/json@../1.0.0";
    require_identity_rejected(&authority, "/checkout/app/main.xr");
    authority.namespace_id = "acme/json";
    require_identity_rejected(&authority, "/checkout/app/main.xr");
}

TEST(namespace_kinds_cannot_collide) {
    XrModuleIdentityAuthority project = {
        .kind = XR_MODULE_IDENTITY_PROJECT,
        .namespace_id = "acme",
        .physical_root = "/checkout/app",
    };
    XrModuleIdentityAuthority script = {
        .kind = XR_MODULE_IDENTITY_SCRIPT,
        .physical_root = "/checkout/app",
    };
    XrModuleIdentityAuthority package = {
        .kind = XR_MODULE_IDENTITY_PACKAGE,
        .namespace_id = "acme/app@1.0.0",
        .physical_root = "/checkout/app",
    };
    char *project_id = NULL;
    char *script_id = NULL;
    char *package_id = NULL;
    char *logical = NULL;
    ASSERT_TRUE(xr_module_identity_from_source(&project, "/checkout/app/main.xr", &project_id,
                                               &logical));
    xr_free(logical);
    ASSERT_TRUE(xr_module_identity_from_source(&script, "/checkout/app/main.xr", &script_id,
                                               &logical));
    xr_free(logical);
    ASSERT_TRUE(xr_module_identity_from_source(&package, "/checkout/app/main.xr", &package_id,
                                               &logical));
    xr_free(logical);
    ASSERT_TRUE(strcmp(project_id, script_id) != 0);
    ASSERT_TRUE(strcmp(project_id, package_id) != 0);
    ASSERT_TRUE(strcmp(script_id, package_id) != 0);
    xr_free(project_id);
    xr_free(script_id);
    xr_free(package_id);
}

TEST(stdlib_identity_is_equal_for_equal_embedded_and_dev_bytes) {
    static const char embedded_source[] = "export fn answer() -> int { return 42 }\n";
    static const char dev_source[] = "export fn answer() -> int { return 42 }\n";
    ASSERT_EQ_UINT(sizeof(embedded_source), sizeof(dev_source));
    ASSERT_EQ_INT(memcmp(embedded_source, dev_source, sizeof(embedded_source)), 0);
    XrModuleIdentityAuthority embedded = {
        .kind = XR_MODULE_IDENTITY_STDLIB,
        .namespace_id = "probe",
    };
    XrModuleIdentityAuthority dev = {
        .kind = XR_MODULE_IDENTITY_STDLIB,
        .namespace_id = "probe",
        .physical_root = "C:/checkout/stdlib",
    };
    char *embedded_id = require_logical_identity(&embedded, "probe/probe.xr");
    char *dev_id = require_logical_identity(&dev, "probe/probe.xr");
    ASSERT_NOT_NULL(embedded_id);
    ASSERT_NOT_NULL(dev_id);
    ASSERT_STR_EQ(embedded_id,
                  "stdlib-module-v1:module=5:probe:path=14:probe/probe.xr");
    ASSERT_STR_EQ(embedded_id, dev_id);
    ASSERT_TRUE(xr_module_identity_valid(embedded_id, NULL));
    xr_free(embedded_id);
    xr_free(dev_id);
}

TEST(memory_identity_requires_an_explicit_valid_id) {
    XrModuleIdentityAuthority authority = {
        .kind = XR_MODULE_IDENTITY_MEMORY,
        .namespace_id = "eval-unit",
    };
    char *identity = require_logical_identity(&authority, NULL);
    ASSERT_NOT_NULL(identity);
    ASSERT_STR_EQ(identity, "memory-module-v1:id=9:eval-unit");
    XrModuleIdentityKind kind = 0;
    ASSERT_TRUE(xr_module_identity_valid(identity, &kind));
    ASSERT_EQ_INT(kind, XR_MODULE_IDENTITY_MEMORY);
    xr_free(identity);

    authority.namespace_id = NULL;
    identity = (char *) 1;
    ASSERT_FALSE(xr_module_identity_from_logical(&authority, NULL, &identity));
    ASSERT_NULL(identity);
    authority.namespace_id = "<eval>";
    ASSERT_FALSE(xr_module_identity_from_logical(&authority, NULL, &identity));
    authority.namespace_id = "eval-unit";
    ASSERT_FALSE(xr_module_identity_from_logical(&authority, "eval.xr", &identity));
}

TEST(memory_graph_rejects_missing_or_raw_eval_identity_before_publication) {
    XrModuleGraph graph = {0};
    char *error = NULL;
    ASSERT_EQ_INT(xr_module_graph_build_source(&graph, NULL, "print(42)\n", &error), -1);
    ASSERT_NOT_NULL(error);
    ASSERT_NOT_NULL(strstr(error, "explicit valid identity"));
    xr_free(error);

    error = NULL;
    XrModuleIdentityAuthority invalid = {
        .kind = XR_MODULE_IDENTITY_MEMORY,
        .namespace_id = "<eval>",
    };
    ASSERT_EQ_INT(xr_module_graph_build_source(&graph, &invalid, "print(42)\n", &error), -1);
    ASSERT_NOT_NULL(error);
    ASSERT_NOT_NULL(strstr(error, "explicit valid identity"));
    xr_free(error);
}

TEST(durable_identity_parser_rejects_raw_and_ambiguous_text) {
    ASSERT_FALSE(xr_module_identity_valid("time", NULL));
    ASSERT_FALSE(xr_module_identity_valid("<eval>", NULL));
    ASSERT_FALSE(xr_module_identity_valid("memory-module-v1:eval-unit", NULL));
    ASSERT_FALSE(xr_module_identity_valid("memory-module-v1:id=09:eval-unit", NULL));
    ASSERT_FALSE(xr_module_identity_valid(
        "stdlib-module-v1:module=5:probe:path=2:..", NULL));
    ASSERT_FALSE(xr_module_identity_valid(
        "stdlib-module-v1:module=5:probe:path=15:probe/probe.xr", NULL));
}

TEST(resolver_publishes_typed_stdlib_identity) {
    /* `time` is in the generated descriptor table, which is the resolver's
     * only source of stdlib module names. */
    XrModuleResolverConfig config = {0};
    XrModuleResolver *resolver = xr_module_resolver_new(&config);
    ASSERT_NOT_NULL(resolver);
    XrModuleId module_id;
    char *error = NULL;
    ASSERT_EQ_INT(xr_module_resolver_resolve(resolver, "time", NULL, NULL, &module_id, &error),
                  0);
    ASSERT_NULL(error);
    ASSERT_EQ_INT(module_id.kind, XR_MOD_STDLIB);
    ASSERT_STR_EQ(module_id.canonical,
                  "stdlib-module-v1:module=4:time:path=12:time/time.xr");
    ASSERT_STR_EQ(module_id.authority.namespace_id, "time");
    ASSERT_TRUE(xr_module_identity_valid(module_id.canonical, NULL));
    xr_module_id_cleanup(&module_id);
    xr_module_resolver_free(resolver);
}

TEST(xi_and_global_evidence_reject_untyped_identity_mutations) {
    XiFunc *root = xi_func_new("identity_probe", NULL);
    ASSERT_NOT_NULL(root);
    XiModule *module = xi_module_new("C:/checkout/probe.xr", "probe", root);
    ASSERT_NOT_NULL(module);
    ASSERT_FALSE(xi_module_set_identity(module, "probe"));
    ASSERT_FALSE(xi_module_set_identity(module, "<eval>"));
    ASSERT_TRUE(xi_module_set_identity(module, "memory-module-v1:id=5:probe"));

    static const char source[] = "export const answer = 42\n";
    XrFingerprint content_fingerprint;
    xr_module_source_fingerprint(source, &content_fingerprint);
    XrModuleSpec embedded = {
        .canonical = "stdlib-module-v1:module=5:probe:path=14:probe/probe.xr",
        .source_path = "<embedded stdlib>/probe/probe.xr",
        .kind = XR_MOD_STDLIB,
        .source_content_fingerprint = content_fingerprint,
    };
    XrModuleSpec dev = embedded;
    dev.source_path = "D:/relocated/stdlib/probe/probe.xr";
    XgModuleSummary embedded_summary;
    XgModuleSummary dev_summary;
    ASSERT_TRUE(xg_module_summary_from_module_spec(&embedded_summary, 1, &embedded));
    ASSERT_TRUE(xg_module_summary_from_module_spec(&dev_summary, 1, &dev));
    ASSERT_EQ_UINT(embedded_summary.canonical_hash, dev_summary.canonical_hash);
    ASSERT_EQ_UINT(embedded_summary.source_hash, dev_summary.source_hash);
    dev.source_content_fingerprint.bytes[0] ^= UINT8_C(0x80);
    ASSERT_TRUE(xg_module_summary_from_module_spec(&dev_summary, 1, &dev));
    ASSERT_TRUE(embedded_summary.source_hash != dev_summary.source_hash);
    dev.canonical = "probe";
    ASSERT_FALSE(xg_module_summary_from_module_spec(&dev_summary, 1, &dev));

    root->module = NULL;
    xi_module_free(module);
    xi_func_free(root);
}

TEST(source_content_fingerprint_is_domain_and_length_framed) {
    static const uint8_t expected[XR_FINGERPRINT_BYTES] = {
        0x3b, 0x83, 0xd6, 0x74, 0xc4, 0x51, 0xd6, 0x0d,
        0x16, 0xeb, 0xb0, 0x7f, 0xcc, 0x87, 0xe1, 0x23,
        0x54, 0x36, 0x8f, 0x6a, 0xd2, 0x68, 0x2b, 0x6d,
        0x1a, 0x6b, 0x8a, 0xb7, 0x38, 0x7e, 0x66, 0x3c,
    };
    XrFingerprint fingerprint;
    xr_module_source_fingerprint("export const answer = 42\n", &fingerprint);
    ASSERT_EQ_INT(memcmp(fingerprint.bytes, expected, sizeof(expected)), 0);

    XrFingerprint different_length;
    xr_module_source_fingerprint("export const answer = 42", &different_length);
    ASSERT_TRUE(memcmp(fingerprint.bytes, different_length.bytes,
                       sizeof(fingerprint.bytes)) != 0);
}

TEST(package_without_exact_lock_fails_closed) {
    XrModuleResolverConfig config = {0};
    XrModuleResolver *resolver = xr_module_resolver_new(&config);
    ASSERT_NOT_NULL(resolver);
    XrModuleId module_id;
    char *error = NULL;
    ASSERT_EQ_INT(
        xr_module_resolver_resolve(resolver, "acme/json", NULL, NULL, &module_id, &error), -1);
    ASSERT_NOT_NULL(error);
    ASSERT_NOT_NULL(strstr(error, "checksummed xray.lock"));
    xr_free(error);
    xr_module_resolver_free(resolver);
}

TEST(package_with_malformed_checksum_fails_closed) {
    XrLockedPackage package = {
        .name = "acme/json",
        .version = "1.0.0",
        .checksum = "sha256:not-a-complete-digest",
    };
    XrLockfile lockfile = {
        .packages = &package,
        .package_count = 1,
    };
    XrModuleResolverConfig config = {.lockfile = &lockfile};
    XrModuleResolver *resolver = xr_module_resolver_new(&config);
    ASSERT_NOT_NULL(resolver);
    XrModuleId module_id;
    char *error = NULL;
    ASSERT_EQ_INT(
        xr_module_resolver_resolve(resolver, "acme/json", NULL, NULL, &module_id, &error), -1);
    ASSERT_NOT_NULL(error);
    ASSERT_NOT_NULL(strstr(error, "checksummed xray.lock"));
    xr_free(error);
    xr_module_resolver_free(resolver);
}

TEST(named_package_edge_requires_exact_unambiguous_identity) {
    int dependencies[2] = {1, 2};
    XrModuleSpec specs[3] = {0};
    specs[0].source_path = "C:/checkout/app/main.xr";
    specs[0].dep_indices = dependencies;
    specs[0].dep_count = 1;
    specs[1].canonical =
        "module-id-v1:kind=7:package:namespace=15:acme/json@2.4.1:path=11:src/main.xr";
    specs[1].logical_path = "src/main.xr";
    specs[1].source_path = "C:/cache/acme/json/2.4.1/src/main.xr";
    specs[1].kind = XR_MOD_PACKAGE;
    specs[1].authority = (XrModuleIdentityAuthority) {
        .kind = XR_MODULE_IDENTITY_PACKAGE,
        .namespace_id = "acme/json@2.4.1",
        .physical_root = "C:/cache/acme/json/2.4.1",
    };
    XrModuleGraph graph = {.specs = specs, .spec_count = 2};

    ASSERT_EQ_INT(xr_module_graph_find_named_dependency(&graph, specs[0].source_path, "acme/json"),
                  1);
    specs[1].authority.namespace_id = "acme/json@2.4.2";
    ASSERT_EQ_INT(xr_module_graph_find_named_dependency(&graph, specs[0].source_path, "acme/json"),
                  -1);
    specs[1].authority.namespace_id = "acme/json@2.4.1";

    specs[2] = specs[1];
    specs[2].canonical =
        "module-id-v1:kind=7:package:namespace=15:acme/json@2.5.0:path=11:src/main.xr";
    specs[2].authority.namespace_id = "acme/json@2.5.0";
    specs[2].authority.physical_root = "C:/cache/acme/json/2.5.0";
    specs[2].source_path = "C:/cache/acme/json/2.5.0/src/main.xr";
    specs[0].dep_count = 2;
    graph.spec_count = 3;
    ASSERT_EQ_INT(xr_module_graph_find_named_dependency(&graph, specs[0].source_path, "acme/json"),
                  -1);
}

TEST(active_graph_named_import_never_falls_back_to_shared_resolver) {
    char entry_path[4096];
    char stdlib_root[4096];
    char base64_path[4096];
    ASSERT_TRUE(snprintf(entry_path, sizeof(entry_path),
                         "%s/tests/unit/module/test_module_identity.c",
                         XRAY_TEST_SOURCE_DIR) > 0);
    ASSERT_TRUE(snprintf(stdlib_root, sizeof(stdlib_root), "%s/stdlib",
                         XRAY_TEST_SOURCE_DIR) > 0);
    ASSERT_TRUE(snprintf(base64_path, sizeof(base64_path), "%s/base64/base64.xr",
                         stdlib_root) > 0);

    XrModuleResolverConfig resolver_config = {
        .stdlib_path = stdlib_root,
    };
    XrModuleResolver *shared_fixture = xr_module_resolver_new(&resolver_config);
    ASSERT_NOT_NULL(shared_fixture);
    XrModuleId shared_id;
    char *error = NULL;
    ASSERT_EQ_INT(xr_module_resolver_resolve(shared_fixture, "base64", entry_path, NULL,
                                             &shared_id, &error),
                  0);
    ASSERT_NULL(error);
    ASSERT_NOT_NULL(shared_id.source_path);

    const char *old_stdlib_path = getenv("XRAY_STDLIB_PATH");
    char *saved_stdlib_path = old_stdlib_path ? xr_strdup(old_stdlib_path) : NULL;
    ASSERT_EQ_INT(xr_test_setenv("XRAY_STDLIB_PATH", stdlib_root, 1), 0);
    XrVMConfig config = {.script_file = entry_path};
    XrVMRuntime *isolate = xray_vm_new_full(&config);
    ASSERT_NOT_NULL(isolate);
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(isolate);
    ASSERT_NOT_NULL(session);

    XrModuleIdentityAuthority entry_authority = {
        .kind = XR_MODULE_IDENTITY_SCRIPT,
        .physical_root = XRAY_TEST_SOURCE_DIR,
    };
    XrModuleIdentityAuthority base64_authority = {
        .kind = XR_MODULE_IDENTITY_STDLIB,
        .namespace_id = "base64",
        .physical_root = stdlib_root,
    };
    char *entry_canonical = NULL;
    char *entry_logical = NULL;
    char *base64_canonical = NULL;
    char *base64_logical = NULL;
    ASSERT_TRUE(xr_module_identity_from_source(&entry_authority, entry_path, &entry_canonical,
                                               &entry_logical));
    ASSERT_TRUE(xr_module_identity_from_source(&base64_authority, base64_path,
                                               &base64_canonical, &base64_logical));

    int dependencies[2] = {1, 2};
    XrModuleSpec specs[3] = {0};
    specs[0].canonical = entry_canonical;
    specs[0].logical_path = entry_logical;
    specs[0].source_path = entry_path;
    specs[0].kind = XR_MOD_FILE;
    specs[0].authority = entry_authority;
    specs[0].dep_indices = dependencies;
    specs[0].dep_count = 1;
    specs[1].canonical = base64_canonical;
    specs[1].logical_path = base64_logical;
    specs[1].source_path = base64_path;
    specs[1].kind = XR_MOD_STDLIB;
    specs[1].authority = base64_authority;
    XrModuleGraph graph = {.specs = specs, .spec_count = 2};
    xr_compiler_session_set_module_graph(session, &graph);

    char *resolved = xr_module_resolve_path(isolate, "base64");
    ASSERT_NOT_NULL(resolved);
    ASSERT_STR_EQ(resolved, base64_path);
    xr_free(resolved);

    specs[1].authority.namespace_id = "base64-mutated";
    ASSERT_NULL(xr_module_resolve_path(isolate, "base64"));
    specs[1].authority.namespace_id = "base64";

    specs[2] = specs[1];
    specs[0].dep_count = 2;
    graph.spec_count = 3;
    ASSERT_NULL(xr_module_resolve_path(isolate, "base64"));

    xr_compiler_session_set_module_graph(session, NULL);
    xr_module_id_cleanup(&shared_id);
    xr_module_resolver_free(shared_fixture);
    xr_free(entry_canonical);
    xr_free(entry_logical);
    xr_free(base64_canonical);
    xr_free(base64_logical);
    xray_vm_delete(isolate);
    if (saved_stdlib_path) {
        ASSERT_EQ_INT(xr_test_setenv("XRAY_STDLIB_PATH", saved_stdlib_path, 1), 0);
        xr_free(saved_stdlib_path);
    } else {
        ASSERT_EQ_INT(xr_test_unsetenv("XRAY_STDLIB_PATH"), 0);
    }
}

TEST(absolute_import_fails_closed) {
    XrModuleResolverConfig config = {0};
    XrModuleResolver *resolver = xr_module_resolver_new(&config);
    ASSERT_NOT_NULL(resolver);
    XrModuleId module_id;
    char *error = NULL;
    ASSERT_EQ_INT(xr_module_resolver_resolve(resolver, "/outside/file.xr", NULL, NULL, &module_id,
                                             &error),
                  -1);
    ASSERT_NOT_NULL(error);
    xr_free(error);
    xr_module_resolver_free(resolver);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Relocatable Module Identity");
RUN_TEST(project_identity_survives_checkout_relocation);
RUN_TEST(project_namespace_is_part_of_identity);
RUN_TEST(package_identity_includes_locked_version);
RUN_TEST(source_outside_authority_fails_closed);
RUN_TEST(relative_physical_locator_fails_closed);
RUN_TEST(posix_drive_and_unc_authorities_are_absolute);
RUN_TEST(namespace_injection_fails_closed);
RUN_TEST(namespace_kinds_cannot_collide);
RUN_TEST(stdlib_identity_is_equal_for_equal_embedded_and_dev_bytes);
RUN_TEST(memory_identity_requires_an_explicit_valid_id);
RUN_TEST(memory_graph_rejects_missing_or_raw_eval_identity_before_publication);
RUN_TEST(durable_identity_parser_rejects_raw_and_ambiguous_text);
RUN_TEST(resolver_publishes_typed_stdlib_identity);
RUN_TEST(xi_and_global_evidence_reject_untyped_identity_mutations);
RUN_TEST(source_content_fingerprint_is_domain_and_length_framed);
RUN_TEST(package_without_exact_lock_fails_closed);
RUN_TEST(package_with_malformed_checksum_fails_closed);
RUN_TEST(named_package_edge_requires_exact_unambiguous_identity);
RUN_TEST(active_graph_named_import_never_falls_back_to_shared_resolver);
RUN_TEST(absolute_import_fails_closed);

TEST_MAIN_END()
