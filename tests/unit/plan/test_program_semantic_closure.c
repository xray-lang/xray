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
#include "../../../src/base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                                 \
    do {                                                                                   \
        if (!(condition)) {                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);          \
            exit(1);                                                                       \
        }                                                                                  \
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

static void hash_u32(XrSHA256Context *context, uint32_t value) {
    uint8_t bytes[4];
    for (uint32_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8u));
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void hash_u64(XrSHA256Context *context, uint64_t value) {
    uint8_t bytes[8];
    for (uint32_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8u));
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void hash_framed(XrSHA256Context *context, const uint8_t *bytes,
                        size_t size) {
    hash_u64(context, (uint64_t) size);
    xr_sha256_update(context, bytes, size);
}

static XrStableId source_callsite_identity(
    const XrProgramSemanticModuleInput *module, XrStableId caller_declaration,
    XrProgramSemanticSourceLocator locator) {
    static const uint8_t domain[] = "xray-source-scalar-callsite-v1";
    XrSHA256Context context;
    uint8_t digest[XR_FINGERPRINT_BYTES];
    XrStableId result;
    xr_sha256_init(&context);
    hash_framed(&context, domain, sizeof(domain) - 1u);
    hash_u32(&context, UINT32_C(1));
    hash_framed(&context, module->source_fingerprint.bytes,
                sizeof(module->source_fingerprint.bytes));
    hash_framed(&context, module->module_identity.bytes,
                sizeof(module->module_identity.bytes));
    hash_framed(&context, caller_declaration.bytes,
                sizeof(caller_declaration.bytes));
    hash_u32(&context, locator.kind);
    hash_u32(&context, locator.start_line);
    hash_u32(&context, locator.start_column);
    hash_u32(&context, locator.end_line);
    hash_u32(&context, locator.end_column);
    xr_sha256_final(&context, digest);
    memcpy(result.bytes, digest, sizeof(result.bytes));
    return result;
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
                                               ClosureFixtureIds *ids,
                                               char *error,
                                               size_t error_size) {
    XrProgramSemanticClosure *closure = NULL;
    XrProgramSemanticClosureLimits limits = ordinary_limits();
    if (!xr_program_semantic_closure_create(&limits, fingerprint(options.policy), &closure,
                                            error, error_size))
        return NULL;
    ids->app_module = stable_id("module:memory:psc-app");
    ids->library_module = stable_id("module:memory:psc-library");
    XrProgramSemanticModuleInput app = {
        .module_identity = ids->app_module,
        .source_fingerprint = fingerprint("app-source"),
        .export_fingerprint = fingerprint("app-exports"),
    };
    XrProgramSemanticModuleInput library = {
        .module_identity = ids->library_module,
        .source_fingerprint = fingerprint("library-source"),
        .export_fingerprint = fingerprint("library-exports"),
    };
    bool modules_ok = options.reverse
                          ? xr_program_semantic_closure_add_module(
                                closure, &library, error, error_size) &&
                                xr_program_semantic_closure_add_module(
                                    closure, &app, error, error_size)
                          : xr_program_semantic_closure_add_module(
                                closure, &app, error, error_size) &&
                                xr_program_semantic_closure_add_module(
                                    closure, &library, error, error_size);
    if (!modules_ok)
        goto failed;
    if (options.include_dependency) {
        XrProgramSemanticDependencyInput dependency = {
            .source_module = ids->app_module,
            .dependency_module = ids->library_module,
            .contract_fingerprint = fingerprint("app-to-library-contract"),
        };
        if (!xr_program_semantic_closure_add_dependency(closure, &dependency, error,
                                                        error_size))
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
    if (!xr_program_semantic_closure_add_type(closure, &pair, &ids->pair_type,
                                              error, error_size))
        goto failed;
    XrProgramSemanticFunctionInput entry = {
        .module_identity = ids->app_module,
        .declaration_identity = stable_id("declaration:psc-app:main"),
        .concrete_instance_identity = stable_id("instance:psc-app:main<>"),
        .signature_fingerprint = fingerprint("fn():i64"),
        .effect_fingerprint = fingerprint("effect:no-suspend:no-throw"),
        .capability_mask = UINT64_C(1),
        .flags = XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY,
    };
    XrProgramSemanticFunctionInput helper = {
        .module_identity = ids->library_module,
        .declaration_identity = stable_id("declaration:psc-library:helper"),
        .concrete_instance_identity = stable_id("instance:psc-library:helper<>"),
        .signature_fingerprint = fingerprint("fn():i64"),
        .effect_fingerprint = fingerprint("effect:no-suspend:no-throw"),
        .capability_mask = UINT64_C(1),
        .flags = XR_PROGRAM_SEMANTIC_FUNCTION_EXPORTED,
    };
    bool functions_ok = options.reverse
                            ? xr_program_semantic_closure_add_function(
                                  closure, &helper, &ids->helper_function, error, error_size) &&
                                  xr_program_semantic_closure_add_function(
                                      closure, &entry, &ids->entry_function, error, error_size)
                            : xr_program_semantic_closure_add_function(
                                  closure, &entry, &ids->entry_function, error, error_size) &&
                                  xr_program_semantic_closure_add_function(
                                      closure, &helper, &ids->helper_function, error, error_size);
    if (!functions_ok)
        goto failed;
    if (options.include_orphan) {
        XrProgramSemanticFunctionInput orphan = helper;
        XrStableId ignored;
        orphan.module_identity = ids->app_module;
        orphan.declaration_identity = stable_id("declaration:psc-app:orphan");
        orphan.concrete_instance_identity = stable_id("instance:psc-app:orphan<>");
        orphan.flags = 0;
        if (!xr_program_semantic_closure_add_function(closure, &orphan, &ignored,
                                                      error, error_size))
            goto failed;
    }
    XrProgramSemanticSourceLocator locator = {
        .kind = 41,
        .start_line = 10,
        .start_column = options.alternate_locator ? 6u : 5u,
        .end_line = 10,
        .end_column = 20,
    };
    XrProgramSemanticCallInput call = {
        .callsite_identity = source_callsite_identity(
            &app, entry.declaration_identity, locator),
        .locator = locator,
        .caller_function = ids->entry_function,
        .callee_function = ids->helper_function,
        .contract_fingerprint = fingerprint("direct-call:fn():i64:no-suspend"),
    };
    if (!xr_program_semantic_closure_add_call(closure, &call, &ids->call,
                                              error, error_size))
        goto failed;
    return closure;

failed:
    xr_program_semantic_closure_free(closure);
    return NULL;
}

static void test_deterministic_closed_world_identity(void) {
    REQUIRE(XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION == UINT32_C(2));
    char error[256] = {0};
    ClosureFixtureIds first_ids = {0};
    ClosureFixtureIds second_ids = {0};
    XrProgramSemanticClosure *first =
        build_fixture((ClosureFixtureOptions) {.include_dependency = true,
                                               .policy = "policy-v1"},
                      &first_ids, error, sizeof(error));
    REQUIRE(first != NULL);
    REQUIRE(xr_program_semantic_closure_freeze(first, error, sizeof(error)));
    XrProgramSemanticClosure *second =
        build_fixture((ClosureFixtureOptions) {.reverse = true,
                                               .include_dependency = true,
                                               .policy = "policy-v1"},
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
        const XrProgramSemanticFunctionRecord *row =
            xr_program_semantic_closure_function(first, i);
        REQUIRE(row != NULL);
        entry_roots += (row->flags & XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY) != 0;
        export_roots += (row->flags & XR_PROGRAM_SEMANTIC_FUNCTION_EXPORTED) != 0;
    }
    REQUIRE(entry_roots == 1);
    REQUIRE(export_roots == 1);
    REQUIRE(memcmp(xr_program_semantic_closure_fingerprint(first).bytes,
                   xr_program_semantic_closure_fingerprint(second).bytes,
                   XR_FINGERPRINT_BYTES) == 0);
    REQUIRE(xr_generation_closure_id_equal(
        xr_program_semantic_closure_generation_id(first),
        xr_program_semantic_closure_generation_id(second)));
    REQUIRE(memcmp(first_ids.pair_type.bytes, second_ids.pair_type.bytes,
                   XR_STABLE_ID_BYTES) == 0);
    static const uint8_t v1_call_identity[XR_STABLE_ID_BYTES] = {
        0xc0, 0x4a, 0xda, 0x85, 0x5d, 0xc3, 0x3c, 0x0f,
        0xe6, 0xb8, 0x92, 0xe4, 0x83, 0x96, 0x29, 0xb0,
    };
    REQUIRE(memcmp(first_ids.call.bytes, v1_call_identity,
                   sizeof(v1_call_identity)) == 0);
    static const uint8_t v2_closure_fingerprint[XR_FINGERPRINT_BYTES] = {
        0x13, 0x6d, 0x11, 0x1f, 0x18, 0xed, 0xa9, 0x42,
        0x1d, 0x09, 0xf6, 0x23, 0x92, 0x31, 0x5b, 0x6e,
        0x38, 0xfe, 0x5d, 0x6c, 0x57, 0x45, 0x62, 0xae,
        0x31, 0xa4, 0x95, 0xea, 0x2b, 0x38, 0x1a, 0x4d,
    };
    static const uint8_t v2_generation_id[XR_STABLE_ID_BYTES] = {
        0xbd, 0xec, 0x3e, 0xcf, 0x96, 0x97, 0x58, 0xe1,
        0x34, 0x8c, 0xf4, 0x8b, 0xef, 0xe6, 0x99, 0x3f,
    };
    REQUIRE(memcmp(xr_program_semantic_closure_fingerprint(first).bytes,
                   v2_closure_fingerprint, sizeof(v2_closure_fingerprint)) == 0);
    REQUIRE(memcmp(xr_program_semantic_closure_generation_id(first).bytes,
                   v2_generation_id, sizeof(v2_generation_id)) == 0);

    ClosureFixtureIds alternate_locator_ids = {0};
    XrProgramSemanticClosure *alternate_locator =
        build_fixture((ClosureFixtureOptions) {.include_dependency = true,
                                               .alternate_locator = true,
                                               .policy = "policy-v1"},
                      &alternate_locator_ids, error, sizeof(error));
    REQUIRE(alternate_locator != NULL);
    REQUIRE(xr_program_semantic_closure_freeze(alternate_locator, error,
                                               sizeof(error)));
    REQUIRE(memcmp(first_ids.call.bytes, alternate_locator_ids.call.bytes,
                   XR_STABLE_ID_BYTES) != 0);
    REQUIRE(memcmp(xr_program_semantic_closure_fingerprint(first).bytes,
                   xr_program_semantic_closure_fingerprint(alternate_locator).bytes,
                   XR_FINGERPRINT_BYTES) != 0);
    REQUIRE(!xr_generation_closure_id_equal(
        xr_program_semantic_closure_generation_id(first),
        xr_program_semantic_closure_generation_id(alternate_locator)));
    xr_program_semantic_closure_free(alternate_locator);

    ClosureFixtureIds other_policy_ids = {0};
    XrProgramSemanticClosure *other_policy =
        build_fixture((ClosureFixtureOptions) {.include_dependency = true,
                                               .policy = "policy-v2"},
                      &other_policy_ids, error, sizeof(error));
    REQUIRE(other_policy != NULL);
    REQUIRE(xr_program_semantic_closure_freeze(other_policy, error, sizeof(error)));
    REQUIRE(memcmp(first_ids.pair_type.bytes, other_policy_ids.pair_type.bytes,
                   XR_STABLE_ID_BYTES) != 0);
    REQUIRE(memcmp(first_ids.entry_function.bytes, other_policy_ids.entry_function.bytes,
                   XR_STABLE_ID_BYTES) != 0);
    REQUIRE(!xr_generation_closure_id_equal(
        xr_program_semantic_closure_generation_id(first),
        xr_program_semantic_closure_generation_id(other_policy)));
    xr_program_semantic_closure_free(other_policy);
    xr_program_semantic_closure_free(second);
    xr_program_semantic_closure_free(first);
}

static void test_incomplete_graphs_fail_closed(void) {
    char error[256] = {0};
    ClosureFixtureIds ids = {0};
    XrProgramSemanticClosure *missing_dependency =
        build_fixture((ClosureFixtureOptions) {.policy = "policy-v1"},
                      &ids, error, sizeof(error));
    REQUIRE(missing_dependency != NULL);
    REQUIRE(!xr_program_semantic_closure_freeze(missing_dependency, error, sizeof(error)));
    REQUIRE(strstr(error, "cross-module call lacks an exact dependency contract") != NULL);
    xr_program_semantic_closure_free(missing_dependency);

    memset(error, 0, sizeof(error));
    XrProgramSemanticClosure *cycle =
        build_fixture((ClosureFixtureOptions) {.include_dependency = true,
                                               .include_cycle = true,
                                               .policy = "policy-v1"},
                      &ids, error, sizeof(error));
    REQUIRE(cycle != NULL);
    REQUIRE(!xr_program_semantic_closure_freeze(cycle, error, sizeof(error)));
    REQUIRE(strstr(error, "dependency graph is cyclic or unreachable") != NULL);
    xr_program_semantic_closure_free(cycle);

    memset(error, 0, sizeof(error));
    XrProgramSemanticClosure *orphan =
        build_fixture((ClosureFixtureOptions) {.include_dependency = true,
                                               .include_orphan = true,
                                               .policy = "policy-v1"},
                      &ids, error, sizeof(error));
    REQUIRE(orphan != NULL);
    REQUIRE(!xr_program_semantic_closure_freeze(orphan, error, sizeof(error)));
    REQUIRE(strstr(error, "function call graph is not closed") != NULL);
    xr_program_semantic_closure_free(orphan);

    memset(error, 0, sizeof(error));
    XrProgramSemanticClosure *rootless =
        build_fixture((ClosureFixtureOptions) {.include_dependency = true,
                                               .policy = "policy-v1"},
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
    REQUIRE(!xr_program_semantic_closure_create(&limits, zero, &closure, error,
                                                sizeof(error)));
    limits.max_modules = 1;
    REQUIRE(xr_program_semantic_closure_create(&limits, fingerprint("policy-v1"), &closure,
                                               error, sizeof(error)));
    XrProgramSemanticModuleInput app = {
        .module_identity = stable_id("module:memory:budget-app"),
        .source_fingerprint = fingerprint("budget-app-source"),
        .export_fingerprint = fingerprint("budget-app-export"),
    };
    XrProgramSemanticModuleInput library = {
        .module_identity = stable_id("module:memory:budget-library"),
        .source_fingerprint = fingerprint("budget-library-source"),
        .export_fingerprint = fingerprint("budget-library-export"),
    };
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
    generic_string.concrete_instance_identity =
        stable_id("instance:budget-app:Box<String>");
    generic_string.shape_fingerprint = fingerprint("Box<String>{value:String}");
    generic_string.ownership_fingerprint = fingerprint("Box<String>:managed-drop");
    XrStableId type_i64;
    XrStableId type_string;
    REQUIRE(xr_program_semantic_closure_add_type(closure, &generic_i64, &type_i64,
                                                 error, sizeof(error)));
    REQUIRE(xr_program_semantic_closure_add_type(closure, &generic_string, &type_string,
                                                 error, sizeof(error)));
    REQUIRE(memcmp(type_i64.bytes, type_string.bytes, sizeof(type_i64.bytes)) != 0);
    REQUIRE(!xr_program_semantic_closure_add_type(closure, &generic_i64, &type_i64,
                                                  error, sizeof(error)));

    XrProgramSemanticFunctionInput map_i64 = {
        .module_identity = app.module_identity,
        .declaration_identity = stable_id("declaration:budget-app:map"),
        .concrete_instance_identity = stable_id("instance:budget-app:map<i64>"),
        .signature_fingerprint = fingerprint("fn(i64):i64"),
        .effect_fingerprint = fingerprint("effect:pure"),
        .flags = XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY,
    };
    XrProgramSemanticFunctionInput map_string = map_i64;
    map_string.concrete_instance_identity = stable_id("instance:budget-app:map<String>");
    map_string.signature_fingerprint = fingerprint("fn(String):String");
    map_string.flags = XR_PROGRAM_SEMANTIC_FUNCTION_EXPORTED;
    XrStableId function_i64;
    XrStableId function_string;
    REQUIRE(xr_program_semantic_closure_add_function(closure, &map_i64, &function_i64,
                                                     error, sizeof(error)));
    REQUIRE(xr_program_semantic_closure_add_function(closure, &map_string,
                                                     &function_string, error,
                                                     sizeof(error)));
    REQUIRE(memcmp(function_i64.bytes, function_string.bytes,
                   sizeof(function_i64.bytes)) != 0);
    XrProgramSemanticFunctionInput invalid_reserved = map_i64;
    invalid_reserved.concrete_instance_identity =
        stable_id("instance:budget-app:map<u64>");
    invalid_reserved.reserved[0] = 1u;
    REQUIRE(!xr_program_semantic_closure_add_function(
        closure, &invalid_reserved, &function_i64, error, sizeof(error)));
    REQUIRE(xr_program_semantic_closure_freeze(closure, error, sizeof(error)));
    REQUIRE(xr_program_semantic_closure_type_count(closure) == 2);
    REQUIRE(xr_program_semantic_closure_function_count(closure) == 2);
    xr_program_semantic_closure_free(closure);
}

static XrProgramSemanticClosure *fresh_frozen_fixture(void) {
    char error[256] = {0};
    ClosureFixtureIds ids = {0};
    XrProgramSemanticClosure *closure =
        build_fixture((ClosureFixtureOptions) {.include_dependency = true,
                                               .policy = "policy-v1"},
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
    REQUIRE(strstr(error, "locator does not match") != NULL);
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
