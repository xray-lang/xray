/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_program_semantic_closure.c - Closed-world semantic identity tests
 */

#include "../../../src/plan/semantic/xr_program_semantic_closure_internal.h"
#include "../../../src/plan/semantic/xr_semantic_ids.h"
#include "../../../src/plan/semantic/xr_source_semantic_identity.h"
#include "../../../src/frontend/parser/xast_types.h"
#include "../../../src/base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

typedef struct ClosureFixtureIds {
    XrStableId app_module;
    XrStableId library_module;
    XrStableId pair_type;
    XrStableId entry_function;
    XrStableId helper_function;
    XrStableId call;
} ClosureFixtureIds;

typedef struct ClosureFixtureOptions {
    bool reverse;
    bool include_dependency;
    bool include_cycle;
    bool include_orphan;
    bool alternate_locator;
    bool alternate_function_locator;
    bool same_module_functions;
    const char *policy;
} ClosureFixtureOptions;

static XrFingerprint fingerprint(const char *text) {
    XrFingerprint value;
    xr_sha256((const uint8_t *) text, strlen(text), value.bytes);
    return value;
}

static XrStableId stable_id(const char *key) {
    XrStableId value;
    XrFingerprint digest;
    REQUIRE(xr_stable_id_from_key(key, &value, &digest));
    return value;
}

static XrProgramSemanticClosureLimits ordinary_limits(void) {
    return (XrProgramSemanticClosureLimits) {
        .max_modules = 4,
        .max_dependencies = 4,
        .max_types = 4,
        .max_functions = 4,
        .max_calls = 4,
    };
}

static XrProgramSemanticClosure *build_fixture(ClosureFixtureOptions options,
                                               ClosureFixtureIds *ids, char *error,
                                               size_t error_size) {
    XrProgramSemanticClosure *closure = NULL;
    XrProgramSemanticClosureLimits limits = ordinary_limits();
    if (!xr_program_semantic_closure_create(&limits, fingerprint(options.policy), &closure, error,
                                            error_size))
        return NULL;
    XrProgramSemanticModuleInput app;
    XrProgramSemanticModuleInput library;
    if (!xr_source_semantic_module_authority("memory-module-v1:id=7:psc-app",
                                             fingerprint("app-source"), &app, NULL) ||
        !xr_source_semantic_module_authority("memory-module-v1:id=11:psc-library",
                                             fingerprint("library-source"), &library, NULL))
        goto failed;
    ids->app_module = app.module_identity;
    ids->library_module = library.module_identity;
    bool modules_ok =
        options.reverse
            ? xr_program_semantic_closure_add_module(closure, &library, error, error_size) &&
                  xr_program_semantic_closure_add_module(closure, &app, error, error_size)
            : xr_program_semantic_closure_add_module(closure, &app, error, error_size) &&
                  xr_program_semantic_closure_add_module(closure, &library, error, error_size);
    if (!modules_ok)
        goto failed;
    if (options.include_dependency) {
        XrProgramSemanticDependencyInput dependency = {
            .source_module = ids->app_module,
            .dependency_module = ids->library_module,
            .contract_fingerprint = fingerprint("app-to-library-contract"),
        };
        if (!xr_program_semantic_closure_add_dependency(closure, &dependency, error, error_size))
            goto failed;
    }
    if (options.include_cycle) {
        XrProgramSemanticDependencyInput cycle = {
            .source_module = ids->library_module,
            .dependency_module = ids->app_module,
            .contract_fingerprint = fingerprint("library-to-app-contract"),
        };
        if (!xr_program_semantic_closure_add_dependency(closure, &cycle, error, error_size))
            goto failed;
    }
    XrProgramSemanticTypeInput pair = {
        .module_identity = ids->app_module,
        .declaration_identity = stable_id("declaration:psc-app:Pair"),
        .concrete_instance_identity = stable_id("instance:psc-app:Pair<>"),
        .shape_fingerprint = fingerprint("Pair{left:i64,right:i64}"),
        .ownership_fingerprint = fingerprint("Pair:inline-copy-drop-none"),
    };
    if (!xr_program_semantic_closure_add_type(closure, &pair, &ids->pair_type, error, error_size))
        goto failed;
    XrProgramSemanticFunctionInput entry = {
        .module_identity = ids->app_module,
        .declaration_identity = stable_id("declaration:psc-app:main"),
        .concrete_instance_identity = stable_id("instance:psc-app:main<>"),
        .declaration_locator =
            {
                .kind = AST_FUNCTION_DECL,
                .start_line = 1,
                .start_column = options.alternate_function_locator ? 2u : 1u,
                .end_line = 3,
                .end_column = 2,
            },
        .signature_fingerprint = fingerprint("fn():i64"),
        .effect_fingerprint = fingerprint("effect:no-suspend:no-throw"),
        .capability_mask = UINT64_C(1),
        .flags = XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY,
    };
    XrProgramSemanticFunctionInput helper = {
        .module_identity = options.same_module_functions ? ids->app_module : ids->library_module,
        .declaration_identity = stable_id("declaration:psc-library:helper"),
        .concrete_instance_identity = stable_id("instance:psc-library:helper<>"),
        .declaration_locator =
            {
                .kind = AST_FUNCTION_DECL,
                .start_line = 5,
                .start_column = 1,
                .end_line = 7,
                .end_column = 2,
            },
        .signature_fingerprint = fingerprint("fn():i64"),
        .effect_fingerprint = fingerprint("effect:no-suspend:no-throw"),
        .capability_mask = UINT64_C(1),
        .flags = XR_PROGRAM_SEMANTIC_FUNCTION_EXPORTED,
    };
    bool functions_ok =
        options.reverse
            ? xr_program_semantic_closure_add_function(closure, &helper, &ids->helper_function,
                                                       error, error_size) &&
                  xr_program_semantic_closure_add_function(closure, &entry, &ids->entry_function,
                                                           error, error_size)
            : xr_program_semantic_closure_add_function(closure, &entry, &ids->entry_function, error,
                                                       error_size) &&
                  xr_program_semantic_closure_add_function(closure, &helper, &ids->helper_function,
                                                           error, error_size);
    if (!functions_ok)
        goto failed;
    if (options.include_orphan) {
        XrProgramSemanticFunctionInput orphan = helper;
        XrStableId ignored;
        orphan.module_identity = ids->app_module;
        orphan.declaration_identity = stable_id("declaration:psc-app:orphan");
        orphan.concrete_instance_identity = stable_id("instance:psc-app:orphan<>");
        orphan.declaration_locator.start_line = 12;
        orphan.declaration_locator.end_line = 14;
        orphan.flags = 0;
        if (!xr_program_semantic_closure_add_function(closure, &orphan, &ignored, error,
                                                      error_size))
            goto failed;
    }
    XrProgramSemanticSourceLocator locator = {
        .kind = AST_CALL_EXPR,
        .start_line = 2,
        .start_column = options.alternate_locator ? 6u : 5u,
        .end_line = 2,
        .end_column = 20,
    };
    XrStableId callsite;
    if (!xr_source_semantic_callsite_identity(app.source_fingerprint, app.module_identity,
                                              entry.declaration_identity, locator, &callsite))
        goto failed;
    XrProgramSemanticCallInput call = {
        .callsite_identity = callsite,
        .locator = locator,
        .caller_function = ids->entry_function,
        .callee_function = ids->helper_function,
        .contract_fingerprint = fingerprint("direct-call:fn():i64:no-suspend"),
    };
    if (!xr_program_semantic_closure_add_call(closure, &call, &ids->call, error, error_size))
        goto failed;
    return closure;

failed:
    xr_program_semantic_closure_free(closure);
    return NULL;
}

static void test_deterministic_closed_world_identity(void) {
    REQUIRE(XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION == UINT32_C(4));
    char error[256] = {0};
    ClosureFixtureIds first_ids = {0};
    ClosureFixtureIds second_ids = {0};
    XrProgramSemanticClosure *first =
        build_fixture((ClosureFixtureOptions) {.include_dependency = true, .policy = "policy-v1"},
                      &first_ids, error, sizeof(error));
    REQUIRE(first != NULL);
    REQUIRE(xr_program_semantic_closure_freeze(first, error, sizeof(error)));
    XrProgramSemanticClosure *second = build_fixture(
        (ClosureFixtureOptions) {
            .reverse = true, .include_dependency = true, .policy = "policy-v1"},
        &second_ids, error, sizeof(error));
    REQUIRE(second != NULL);
    REQUIRE(xr_program_semantic_closure_freeze(second, error, sizeof(error)));
    REQUIRE(xr_program_semantic_closure_is_frozen(first));
    REQUIRE(xr_program_semantic_closure_is_verified(first));
    REQUIRE(xr_program_semantic_closure_verify(first, error, sizeof(error)));
    REQUIRE(xr_program_semantic_closure_schema(first) ==
            XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION);
    REQUIRE(xr_program_semantic_closure_module_count(first) == 2);
    REQUIRE(xr_program_semantic_closure_dependency_count(first) == 1);
    REQUIRE(xr_program_semantic_closure_type_count(first) == 1);
    REQUIRE(xr_program_semantic_closure_function_count(first) == 2);
    REQUIRE(xr_program_semantic_closure_call_count(first) == 1);
    uint32_t entry_roots = 0;
    uint32_t export_roots = 0;
    for (uint32_t i = 0; i < xr_program_semantic_closure_function_count(first); i++) {
        const XrProgramSemanticFunctionRecord *row = xr_program_semantic_closure_function(first, i);
        REQUIRE(row != NULL);
        entry_roots += (row->flags & XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY) != 0;
        export_roots += (row->flags & XR_PROGRAM_SEMANTIC_FUNCTION_EXPORTED) != 0;
    }
    REQUIRE(entry_roots == 1);
    REQUIRE(export_roots == 1);
    REQUIRE(memcmp(xr_program_semantic_closure_fingerprint(first).bytes,
                   xr_program_semantic_closure_fingerprint(second).bytes,
                   XR_FINGERPRINT_BYTES) == 0);
    REQUIRE(xr_generation_closure_id_equal(xr_program_semantic_closure_generation_id(first),
                                           xr_program_semantic_closure_generation_id(second)));
    REQUIRE(memcmp(first_ids.pair_type.bytes, second_ids.pair_type.bytes, XR_STABLE_ID_BYTES) == 0);
    static const uint8_t canonical_call_identity[XR_STABLE_ID_BYTES] = {
        0x3d, 0xa5, 0x0e, 0x03, 0x7d, 0x21, 0x1b, 0x53,
        0xb3, 0xfb, 0x8e, 0x4f, 0xe9, 0xba, 0x17, 0x81,
    };
    REQUIRE(memcmp(first_ids.call.bytes, canonical_call_identity,
                   sizeof(canonical_call_identity)) == 0);
    static const uint8_t canonical_entry_function_identity[XR_STABLE_ID_BYTES] = {
        0xc6, 0x7e, 0x74, 0x49, 0x7f, 0xd3, 0x6d, 0x81,
        0x75, 0x0c, 0xcc, 0x8f, 0xcb, 0x44, 0x8e, 0xe7,
    };
    static const uint8_t canonical_helper_function_identity[XR_STABLE_ID_BYTES] = {
        0x2e, 0x0e, 0x44, 0xb9, 0x3b, 0x0e, 0x6a, 0xe1,
        0xc7, 0x6d, 0x37, 0x7a, 0x0b, 0x97, 0x3a, 0x1a,
    };
    static const uint8_t v4_closure_fingerprint[XR_FINGERPRINT_BYTES] = {
        0xae, 0xc2, 0x41, 0xba, 0x81, 0xae, 0x96, 0x0e, 0x59, 0x41, 0x7d,
        0x97, 0x28, 0xc3, 0xd7, 0x10, 0x52, 0x72, 0xf8, 0x4f, 0x9e, 0xee,
        0xa0, 0xda, 0x99, 0x24, 0x8d, 0xb3, 0x85, 0xc5, 0xf2, 0x5a,
    };
    static const uint8_t v4_generation_id[XR_STABLE_ID_BYTES] = {
        0xad, 0x80, 0xef, 0x16, 0x44, 0x81, 0x37, 0x4b,
        0x6b, 0xc1, 0xeb, 0xee, 0xc7, 0x5e, 0xe7, 0x35,
    };
    REQUIRE(memcmp(first_ids.entry_function.bytes, canonical_entry_function_identity,
                   sizeof(canonical_entry_function_identity)) == 0);
    REQUIRE(memcmp(first_ids.helper_function.bytes, canonical_helper_function_identity,
                   sizeof(canonical_helper_function_identity)) == 0);
    REQUIRE(memcmp(xr_program_semantic_closure_fingerprint(first).bytes, v4_closure_fingerprint,
                   sizeof(v4_closure_fingerprint)) == 0);
    REQUIRE(memcmp(xr_program_semantic_closure_generation_id(first).bytes, v4_generation_id,
                   sizeof(v4_generation_id)) == 0);

    ClosureFixtureIds alternate_locator_ids = {0};
    XrProgramSemanticClosure *alternate_locator = build_fixture(
        (ClosureFixtureOptions) {
            .include_dependency = true, .alternate_locator = true, .policy = "policy-v1"},
        &alternate_locator_ids, error, sizeof(error));
    REQUIRE(alternate_locator != NULL);
    REQUIRE(xr_program_semantic_closure_freeze(alternate_locator, error, sizeof(error)));
    REQUIRE(memcmp(first_ids.call.bytes, alternate_locator_ids.call.bytes, XR_STABLE_ID_BYTES) !=
            0);
    REQUIRE(memcmp(xr_program_semantic_closure_fingerprint(first).bytes,
                   xr_program_semantic_closure_fingerprint(alternate_locator).bytes,
                   XR_FINGERPRINT_BYTES) != 0);
    REQUIRE(!xr_generation_closure_id_equal(
        xr_program_semantic_closure_generation_id(first),
        xr_program_semantic_closure_generation_id(alternate_locator)));
    xr_program_semantic_closure_free(alternate_locator);

    ClosureFixtureIds alternate_function_locator_ids = {0};
    XrProgramSemanticClosure *alternate_function_locator = build_fixture(
        (ClosureFixtureOptions) {
            .include_dependency = true,
            .alternate_function_locator = true,
            .policy = "policy-v1",
        },
        &alternate_function_locator_ids, error, sizeof(error));
    REQUIRE(alternate_function_locator != NULL);
    REQUIRE(xr_program_semantic_closure_freeze(alternate_function_locator, error, sizeof(error)));
    REQUIRE(memcmp(first_ids.entry_function.bytes,
                   alternate_function_locator_ids.entry_function.bytes, XR_STABLE_ID_BYTES) == 0);
    REQUIRE(memcmp(first_ids.call.bytes, alternate_function_locator_ids.call.bytes,
                   XR_STABLE_ID_BYTES) == 0);
    REQUIRE(memcmp(xr_program_semantic_closure_fingerprint(first).bytes,
                   xr_program_semantic_closure_fingerprint(alternate_function_locator).bytes,
                   XR_FINGERPRINT_BYTES) != 0);
    xr_program_semantic_closure_free(alternate_function_locator);

    ClosureFixtureIds other_policy_ids = {0};
    XrProgramSemanticClosure *other_policy =
        build_fixture((ClosureFixtureOptions) {.include_dependency = true, .policy = "policy-v2"},
                      &other_policy_ids, error, sizeof(error));
    REQUIRE(other_policy != NULL);
    REQUIRE(xr_program_semantic_closure_freeze(other_policy, error, sizeof(error)));
    REQUIRE(memcmp(first_ids.pair_type.bytes, other_policy_ids.pair_type.bytes,
                   XR_STABLE_ID_BYTES) != 0);
    REQUIRE(memcmp(first_ids.entry_function.bytes, other_policy_ids.entry_function.bytes,
                   XR_STABLE_ID_BYTES) != 0);
    REQUIRE(
        !xr_generation_closure_id_equal(xr_program_semantic_closure_generation_id(first),
                                        xr_program_semantic_closure_generation_id(other_policy)));
    xr_program_semantic_closure_free(other_policy);
    xr_program_semantic_closure_free(second);
    xr_program_semantic_closure_free(first);
}

static void test_incomplete_graphs_fail_closed(void) {
    char error[256] = {0};
    ClosureFixtureIds ids = {0};
    XrProgramSemanticClosure *missing_dependency =
        build_fixture((ClosureFixtureOptions) {.policy = "policy-v1"}, &ids, error, sizeof(error));
    REQUIRE(missing_dependency != NULL);
    REQUIRE(!xr_program_semantic_closure_freeze(missing_dependency, error, sizeof(error)));
    REQUIRE(strstr(error, "cross-module call lacks an exact dependency contract") != NULL);
    xr_program_semantic_closure_free(missing_dependency);

    memset(error, 0, sizeof(error));
    XrProgramSemanticClosure *cycle = build_fixture(
        (ClosureFixtureOptions) {
            .include_dependency = true, .include_cycle = true, .policy = "policy-v1"},
        &ids, error, sizeof(error));
    REQUIRE(cycle != NULL);
    REQUIRE(!xr_program_semantic_closure_freeze(cycle, error, sizeof(error)));
    REQUIRE(strstr(error, "dependency graph is cyclic or unreachable") != NULL);
    xr_program_semantic_closure_free(cycle);

    memset(error, 0, sizeof(error));
    XrProgramSemanticClosure *orphan = build_fixture(
        (ClosureFixtureOptions) {
            .include_dependency = true, .include_orphan = true, .policy = "policy-v1"},
        &ids, error, sizeof(error));
    REQUIRE(orphan != NULL);
    REQUIRE(!xr_program_semantic_closure_freeze(orphan, error, sizeof(error)));
    REQUIRE(strstr(error, "function call graph is not closed") != NULL);
    xr_program_semantic_closure_free(orphan);

    memset(error, 0, sizeof(error));
    XrProgramSemanticClosure *rootless =
        build_fixture((ClosureFixtureOptions) {.include_dependency = true, .policy = "policy-v1"},
                      &ids, error, sizeof(error));
    REQUIRE(rootless != NULL);
    for (uint32_t i = 0; i < rootless->function_count; i++)
        rootless->functions[i].flags = 0;
    REQUIRE(!xr_program_semantic_closure_freeze(rootless, error, sizeof(error)));
    REQUIRE(strstr(error, "entry or export root") != NULL);
    xr_program_semantic_closure_free(rootless);
}

static void test_budgets_and_duplicate_coordinates_fail_closed(void) {
    char error[256] = {0};
    XrProgramSemanticClosure *closure = NULL;
    XrProgramSemanticClosureLimits limits = ordinary_limits();
    XrFingerprint zero = {{0}};
    REQUIRE(!xr_program_semantic_closure_create(&limits, zero, &closure, error, sizeof(error)));
    limits.max_modules = 1;
    REQUIRE(xr_program_semantic_closure_create(&limits, fingerprint("policy-v1"), &closure, error,
                                               sizeof(error)));
    XrProgramSemanticModuleInput app;
    XrProgramSemanticModuleInput library;
    REQUIRE(xr_source_semantic_module_authority("memory-module-v1:id=10:budget-app",
                                                fingerprint("budget-app-source"), &app, NULL));
    REQUIRE(xr_source_semantic_module_authority("memory-module-v1:id=14:budget-library",
                                                fingerprint("budget-library-source"), &library,
                                                NULL));
    REQUIRE(xr_program_semantic_closure_add_module(closure, &app, error, sizeof(error)));
    REQUIRE(!xr_program_semantic_closure_add_module(closure, &library, error, sizeof(error)));
    REQUIRE(strstr(error, "XR_EXEC_5003") != NULL);
    memset(error, 0, sizeof(error));
    REQUIRE(!xr_program_semantic_closure_add_module(closure, &app, error, sizeof(error)));
    REQUIRE(strstr(error, "duplicated") != NULL);

    XrProgramSemanticTypeInput generic_i64 = {
        .module_identity = app.module_identity,
        .declaration_identity = stable_id("declaration:budget-app:Box"),
        .concrete_instance_identity = stable_id("instance:budget-app:Box<i64>"),
        .shape_fingerprint = fingerprint("Box<i64>{value:i64}"),
        .ownership_fingerprint = fingerprint("Box<i64>:inline-copy"),
    };
    XrProgramSemanticTypeInput generic_string = generic_i64;
    generic_string.concrete_instance_identity = stable_id("instance:budget-app:Box<String>");
    generic_string.shape_fingerprint = fingerprint("Box<String>{value:String}");
    generic_string.ownership_fingerprint = fingerprint("Box<String>:managed-drop");
    XrStableId type_i64;
    XrStableId type_string;
    REQUIRE(xr_program_semantic_closure_add_type(closure, &generic_i64, &type_i64, error,
                                                 sizeof(error)));
    REQUIRE(xr_program_semantic_closure_add_type(closure, &generic_string, &type_string, error,
                                                 sizeof(error)));
    REQUIRE(memcmp(type_i64.bytes, type_string.bytes, sizeof(type_i64.bytes)) != 0);
    REQUIRE(!xr_program_semantic_closure_add_type(closure, &generic_i64, &type_i64, error,
                                                  sizeof(error)));

    XrProgramSemanticFunctionInput map_i64 = {
        .module_identity = app.module_identity,
        .declaration_identity = stable_id("declaration:budget-app:map"),
        .concrete_instance_identity = stable_id("instance:budget-app:map<i64>"),
        .declaration_locator =
            {
                .kind = AST_FUNCTION_DECL,
                .start_line = 20,
                .start_column = 1,
                .end_line = 22,
                .end_column = 2,
            },
        .signature_fingerprint = fingerprint("fn(i64):i64"),
        .effect_fingerprint = fingerprint("effect:pure"),
        .flags = XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY,
    };
    XrProgramSemanticFunctionInput map_string = map_i64;
    map_string.concrete_instance_identity = stable_id("instance:budget-app:map<String>");
    map_string.signature_fingerprint = fingerprint("fn(String):String");
    map_string.declaration_locator.start_line = 24;
    map_string.declaration_locator.end_line = 26;
    map_string.flags = XR_PROGRAM_SEMANTIC_FUNCTION_EXPORTED;
    XrStableId function_i64;
    XrStableId function_string;
    REQUIRE(xr_program_semantic_closure_add_function(closure, &map_i64, &function_i64, error,
                                                     sizeof(error)));
    REQUIRE(xr_program_semantic_closure_add_function(closure, &map_string, &function_string, error,
                                                     sizeof(error)));
    REQUIRE(memcmp(function_i64.bytes, function_string.bytes, sizeof(function_i64.bytes)) != 0);
    XrProgramSemanticFunctionInput invalid_reserved = map_i64;
    invalid_reserved.concrete_instance_identity = stable_id("instance:budget-app:map<u64>");
    invalid_reserved.reserved[0] = 1u;
    REQUIRE(!xr_program_semantic_closure_add_function(closure, &invalid_reserved, &function_i64,
                                                      error, sizeof(error)));

    XrProgramSemanticFunctionInput invalid_locator = map_i64;
    invalid_locator.concrete_instance_identity = stable_id("instance:budget-app:map<i32>");
    invalid_locator.declaration_locator.kind = 0;
    REQUIRE(!xr_program_semantic_closure_add_function(closure, &invalid_locator, &function_i64,
                                                      error, sizeof(error)));

    XrProgramSemanticFunctionInput duplicate_locator = map_i64;
    duplicate_locator.declaration_identity = stable_id("declaration:budget-app:reduce");
    duplicate_locator.concrete_instance_identity = stable_id("instance:budget-app:reduce<i64>");
    REQUIRE(!xr_program_semantic_closure_add_function(closure, &duplicate_locator, &function_i64,
                                                      error, sizeof(error)));
    REQUIRE(xr_program_semantic_closure_freeze(closure, error, sizeof(error)));
    REQUIRE(xr_program_semantic_closure_type_count(closure) == 2);
    REQUIRE(xr_program_semantic_closure_function_count(closure) == 2);
    xr_program_semantic_closure_free(closure);
}

static XrProgramSemanticClosure *fresh_frozen_fixture(void) {
    char error[256] = {0};
    ClosureFixtureIds ids = {0};
    XrProgramSemanticClosure *closure =
        build_fixture((ClosureFixtureOptions) {.include_dependency = true, .policy = "policy-v1"},
                      &ids, error, sizeof(error));
    REQUIRE(closure != NULL);
    REQUIRE(xr_program_semantic_closure_freeze(closure, error, sizeof(error)));
    return closure;
}

static void test_independent_verifier_rejects_hostile_mutations(void) {
    char error[256] = {0};
    XrProgramSemanticClosure *closure = fresh_frozen_fixture();
    closure->types[0].concrete_instance_identity.bytes[0] ^= 1u;
    REQUIRE(!xr_program_semantic_closure_verify(closure, error, sizeof(error)));
    REQUIRE(strstr(error, "XR_SEM_0013") != NULL);
    xr_program_semantic_closure_free(closure);

    closure = fresh_frozen_fixture();
    closure->functions[0].concrete_instance_identity.bytes[0] ^= 1u;
    REQUIRE(!xr_program_semantic_closure_verify(closure, error, sizeof(error)));
    REQUIRE(strstr(error, "XR_SEM_0013") != NULL);
    xr_program_semantic_closure_free(closure);

    closure = fresh_frozen_fixture();
    closure->functions[0].effect_fingerprint.bytes[0] ^= 1u;
    REQUIRE(!xr_program_semantic_closure_verify(closure, error, sizeof(error)));
    REQUIRE(strstr(error, "XR_SEM_0013") != NULL);
    xr_program_semantic_closure_free(closure);

    closure = fresh_frozen_fixture();
    closure->functions[0].declaration_locator.kind = 0;
    REQUIRE(!xr_program_semantic_closure_verify(closure, error, sizeof(error)));
    REQUIRE(strstr(error, "function identity") != NULL);
    xr_program_semantic_closure_free(closure);

    ClosureFixtureIds same_module_ids = {0};
    closure = build_fixture(
        (ClosureFixtureOptions) {
            .include_dependency = true,
            .same_module_functions = true,
            .policy = "policy-v1",
        },
        &same_module_ids, error, sizeof(error));
    REQUIRE(closure != NULL);
    REQUIRE(xr_program_semantic_closure_freeze(closure, error, sizeof(error)));
    closure->functions[1].declaration_locator = closure->functions[0].declaration_locator;
    REQUIRE(!xr_program_semantic_closure_verify(closure, error, sizeof(error)));
    REQUIRE(strstr(error, "declaration locator is duplicated") != NULL);
    xr_program_semantic_closure_free(closure);

    closure = fresh_frozen_fixture();
    closure->modules[0].export_fingerprint.bytes[0] ^= 1u;
    REQUIRE(!xr_program_semantic_closure_verify(closure, error, sizeof(error)));
    REQUIRE(strstr(error, "fingerprint does not match its rows") != NULL);
    xr_program_semantic_closure_free(closure);

    closure = fresh_frozen_fixture();
    closure->calls[0].locator.kind = 0;
    REQUIRE(!xr_program_semantic_closure_verify(closure, error, sizeof(error)));
    REQUIRE(strstr(error, "resolved call identity") != NULL);
    xr_program_semantic_closure_free(closure);

    closure = fresh_frozen_fixture();
    closure->calls[0].locator.start_line = UINT32_MAX;
    REQUIRE(!xr_program_semantic_closure_verify(closure, error, sizeof(error)));
    REQUIRE(strstr(error, "resolved call identity") != NULL);
    xr_program_semantic_closure_free(closure);

    closure = fresh_frozen_fixture();
    closure->calls[0].locator.end_line = closure->calls[0].locator.start_line;
    closure->calls[0].locator.end_column = closure->calls[0].locator.start_column;
    REQUIRE(!xr_program_semantic_closure_verify(closure, error, sizeof(error)));
    REQUIRE(strstr(error, "resolved call identity") != NULL);
    xr_program_semantic_closure_free(closure);

    closure = fresh_frozen_fixture();
    closure->calls[0].locator.kind++;
    REQUIRE(!xr_program_semantic_closure_verify(closure, error, sizeof(error)));
    REQUIRE(strstr(error, "resolved call identity") != NULL);
    xr_program_semantic_closure_free(closure);

    closure = fresh_frozen_fixture();
    closure->calls[0].locator.start_column++;
    REQUIRE(!xr_program_semantic_closure_verify(closure, error, sizeof(error)));
    REQUIRE(strstr(error, "locator does not match") != NULL);
    xr_program_semantic_closure_free(closure);

    closure = fresh_frozen_fixture();
    REQUIRE(closure->call_capacity >= 2);
    closure->calls[1] = closure->calls[0];
    closure->call_count = 2;
    REQUIRE(!xr_program_semantic_closure_verify(closure, error, sizeof(error)));
    REQUIRE(strstr(error, "source locator is duplicated") != NULL);
    xr_program_semantic_closure_free(closure);

    closure = fresh_frozen_fixture();
    closure->fingerprint.bytes[0] ^= 1u;
    REQUIRE(!xr_program_semantic_closure_verify(closure, error, sizeof(error)));
    REQUIRE(strstr(error, "fingerprint does not match its rows") != NULL);
    xr_program_semantic_closure_free(closure);

    closure = fresh_frozen_fixture();
    closure->generation_id.bytes[0] ^= 1u;
    REQUIRE(!xr_program_semantic_closure_verify(closure, error, sizeof(error)));
    REQUIRE(strstr(error, "GenerationClosureId") != NULL);
    xr_program_semantic_closure_free(closure);

    closure = fresh_frozen_fixture();
    closure->verified = 0;
    REQUIRE(!xr_program_semantic_closure_verify(closure, error, sizeof(error)));
    REQUIRE(strstr(error, "header is incomplete") != NULL);
    xr_program_semantic_closure_free(closure);
}

int main(void) {
    test_deterministic_closed_world_identity();
    test_incomplete_graphs_fail_closed();
    test_budgets_and_duplicate_coordinates_fail_closed();
    test_independent_verifier_rejects_hostile_mutations();
    puts("ProgramSemanticClosure tests passed");
    return 0;
}
