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
#include "module/xmodule_graph.h"
#include "module/xmodule_identity.h"
#include "module/xmodule_resolver.h"
#include "module/xlockfile.h"

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
    ASSERT_EQ_INT(xr_module_graph_build_source(&graph, "<eval>", "print(42)\n", &error), -1);
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
    XrHashMap *factories = xr_hashmap_new();
    ASSERT_NOT_NULL(factories);
    ASSERT_TRUE(xr_hashmap_set(factories, "time", (void *) 1));
    XrModuleResolverConfig config = {.native_factories = factories};
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
    xr_hashmap_free(factories);
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
    uint64_t content_hash = xr_hash_bytes64(source, strlen(source));
    XrModuleSpec embedded = {
        .canonical = "stdlib-module-v1:module=5:probe:path=14:probe/probe.xr",
        .source_path = "<embedded stdlib>/probe/probe.xr",
        .kind = XR_MOD_STDLIB,
        .source_content_hash = content_hash,
    };
    XrModuleSpec dev = embedded;
    dev.source_path = "D:/relocated/stdlib/probe/probe.xr";
    XgModuleSummary embedded_summary;
    XgModuleSummary dev_summary;
    ASSERT_TRUE(xg_module_summary_from_module_spec(&embedded_summary, 1, &embedded));
    ASSERT_TRUE(xg_module_summary_from_module_spec(&dev_summary, 1, &dev));
    ASSERT_EQ_UINT(embedded_summary.canonical_hash, dev_summary.canonical_hash);
    ASSERT_EQ_UINT(embedded_summary.source_hash, dev_summary.source_hash);
    dev.source_content_hash++;
    ASSERT_TRUE(xg_module_summary_from_module_spec(&dev_summary, 1, &dev));
    ASSERT_TRUE(embedded_summary.source_hash != dev_summary.source_hash);
    dev.canonical = "probe";
    ASSERT_FALSE(xg_module_summary_from_module_spec(&dev_summary, 1, &dev));

    root->module = NULL;
    xi_module_free(module);
    xi_func_free(root);
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
RUN_TEST(package_without_exact_lock_fails_closed);
RUN_TEST(package_with_malformed_checksum_fails_closed);
RUN_TEST(absolute_import_fails_closed);

TEST_MAIN_END()
