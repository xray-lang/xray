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
RUN_TEST(package_without_exact_lock_fails_closed);
RUN_TEST(package_with_malformed_checksum_fails_closed);
RUN_TEST(absolute_import_fails_closed);

TEST_MAIN_END()
