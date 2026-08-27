/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * plan_fixture_writer.c - Matched semantic and target artifacts on disk
 *
 * KEY CONCEPT:
 *   The plan command needs real artifacts to inspect, and the only artifacts
 *   the toolchain writes today live inside the incremental cache behind its
 *   own framing. This writer produces the unframed pair directly from the
 *   ordinary builders, so the command under test reads exactly what the
 *   runtime would bind. The returned value is a parameter, which lets a
 *   caller ask for two plans that differ in one instruction immediate and
 *   nothing else.
 */

#include "../../src/base/xmalloc.h"
#include "../../src/ir/xi.h"
#include "../../src/ir/xi_module.h"
#include "../../src/plan/format/xr_xsm_schema.h"
#include "../../src/plan/format/xr_xtp_schema.h"
#include "../../src/plan/semantic/xr_semantic_builder.h"
#include "../../src/plan/target/xr_target_builder.h"
#include "../../src/runtime/abi/xr_runtime_target_authority.h"
#include "../../src/runtime/value/xtype.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);   \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static XrType plan_fixture_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};

static XrSemanticPlan *build_semantic(const char *name, int64_t value) {
    XiFunc *function = xi_func_new(name, &plan_fixture_int);
    if (!function)
        return NULL;
    XiBlock *entry = xi_block_new(function);
    XiValue *constant = entry ? xi_const_int(function, entry, value, &plan_fixture_int) : NULL;
    if (!constant) {
        xi_func_free(function);
        return NULL;
    }
    xi_block_set_return(entry, constant);
    function->stage = XI_STAGE_OPTIMIZED;
    /* The SemanticPlan builder requires a lowered graph to carry a typed
     * durable module identity and synthesizes none, so this fixture names its
     * own memory-namespace identity derived from the function it writes.
     * xi_func_free owns the module and releases it with the function. */
    char identity[256];
    int written = snprintf(identity, sizeof(identity), "memory-module-v1:id=%zu:%s",
                           strlen(name), name);
    function->module = written > 0 && (size_t) written < sizeof(identity)
                           ? xi_module_new("plan_fixture.xr", name, function)
                           : NULL;
    if (!function->module || !xi_module_set_identity(function->module, identity)) {
        fprintf(stderr, "plan fixture module identity failed for '%s'\n", name);
        xi_func_free(function);
        return NULL;
    }
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    bool built = xr_semantic_plan_build(function, &semantic, error, sizeof(error));
    xi_func_free(function);
    if (built)
        return semantic;
    fprintf(stderr, "semantic plan build failed: %s\n", error);
    return NULL;
}

/* The runtime rebuilds this exact profile from its own authority when it
 * binds an artifact, so a fixture built on anything else would encode a
 * foreign machine and fail identity binding rather than exercise the
 * command. */
static XrTargetProfile *build_native_hosted_profile(void) {
    XrRuntimeTargetAuthority authority;
    if (xr_runtime_target_authority_native_hosted(&authority) != XR_RUNTIME_ABI_OK)
        return NULL;
    XrTargetProfileBuildInput input = {
        .machine = authority.machine,
        .runtime_abi = &authority.runtime_abi,
        .object_header_materialization = &authority.object_header_materialization,
        .string_contract = &authority.string_contract,
        .providers = authority.providers,
        .provider_count = authority.provider_count,
    };
    XrTargetProfile *profile = NULL;
    char error[512] = {0};
    if (xr_target_profile_build(&input, &profile, error, sizeof(error)))
        return profile;
    fprintf(stderr, "target profile build failed: %s\n", error);
    return NULL;
}

static bool write_bytes(const char *path, const uint8_t *bytes, size_t size) {
    FILE *file = fopen(path, "wb");
    if (!file)
        return false;
    bool written = fwrite(bytes, 1, size, file) == size;
    return fclose(file) == 0 && written;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s <function-name> <return-value> <xsm-path> <xtp-path>\n",
                argc > 0 ? argv[0] : "plan_fixture_writer");
        return 2;
    }
    const char *name = argv[1];
    int64_t value = (int64_t) strtoll(argv[2], NULL, 10);
    const char *xsm_path = argv[3];
    const char *xtp_path = argv[4];

    XrSemanticPlan *semantic = build_semantic(name, value);
    REQUIRE(semantic != NULL);
    XrTargetProfile *profile = build_native_hosted_profile();
    REQUIRE(profile != NULL);

    char error[512] = {0};
    XrTargetPlan *plan = NULL;
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, error, sizeof(error)));
    REQUIRE(xr_target_plan_is_verified(plan));

    uint8_t *xtp_bytes = NULL;
    size_t xtp_size = 0;
    REQUIRE(xr_xtp_encode_plan(plan, &xtp_bytes, &xtp_size, error, sizeof(error)));
    uint8_t *xsm_bytes = NULL;
    size_t xsm_size = 0;
    REQUIRE(xr_xsm_encode(semantic, &xsm_bytes, &xsm_size, error, sizeof(error)));

    bool written = write_bytes(xsm_path, xsm_bytes, xsm_size) &&
                   write_bytes(xtp_path, xtp_bytes, xtp_size);

    xr_free(xsm_bytes);
    xr_xtp_encoded_free(xtp_bytes);
    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
    if (!written) {
        fprintf(stderr, "artifact pair could not be written\n");
        return 1;
    }
    return 0;
}
