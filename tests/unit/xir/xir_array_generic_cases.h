/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_generic_cases.h - Definition proof before Array specialization
 *
 * KEY CONCEPT:
 *   Concrete i64 instances cannot repair a generic index without an i64 proof.
 */
#ifndef XIR_ARRAY_GENERIC_CASES_H
#define XIR_ARRAY_GENERIC_CASES_H
#include "xir_construction_fixture.h"
#include "xir_array_generic_fixture.h"

static void array_generic_cases(void) {
    XrXirArtifact *checked = array_generic_fixture(suite_context), *decoded = NULL, *closed = NULL, *lowered = NULL;
    const XrXirModule *module = xr_xir_compile_artifact_module(checked);
    XrXirFunction views[4]; memcpy(views, module->functions, 3 * sizeof(*views));
    views[3] = views[1]; views[3].name = "unused"; views[3].name_length = 6;
    XrXirGeneric generics[4]; memcpy(generics, module->generics, 3 * sizeof(*generics));
    generics[3] = generics[1];
    XrXirInstruction body[6]; memcpy(body, views[3].instructions, sizeof(body));
    views[3].instructions = body;
    XrXirModule built = *module; built.stage = XR_XIR_BUILT;
    built.functions = views; built.function_count = 4; built.generics = generics;
    CHECK(xir_fixture_verify(suite_context, &built, NULL) == XR_XIR_OK);
    body[4].args[1] = 0; /* T can be i64 in an instance; it is not i64 in the definition. */
    CHECK(xir_fixture_verify(suite_context, &built, NULL) == XR_XIR_BAD_TYPE);
    XrXirArtifact *rejected = NULL;
    CHECK(xir_fixture_check(suite_context, &built, &rejected, NULL) == XR_XIR_BAD_TYPE && !rejected);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked); checked=NULL;
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
    XrXirCompileContext bounded=consumer_context_default();bounded.limits.functions=2;
    XrXirArtifact *limited=NULL;
    /* The template itself has three functions: immutable whole-graph limits
     * reject the bounded receiver before it can publish that template. */
    CHECK(xr_xir_compile_checked_read(&bounded,packet.bytes,packet.length,&limited,NULL)==XR_XIR_BUDGET && !limited);
    generic_specialization_work_boundary(decoded);
    memset(packet.bytes, 0xCC, packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(decoded, &closed, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded); decoded=NULL;
    module = xr_xir_compile_artifact_module(closed);
    CHECK(!module->generics && module->function_count == 3 && module->types->count == 2);
    XrXirType first = module->functions[1].instructions[0].type;
    XrXirType second = module->functions[2].instructions[0].type;
    CHECK(first != second && xr_xir_array_element(module->types, first) == XR_XIR_I64 &&
        xr_xir_array_element(module->types, second) == XR_XIR_U8);
    for (uint32_t f = 1; f < 3; ++f) {
        const XrXirInstruction *ops = module->functions[f].instructions;
        CHECK(ops[0].type == ops[1].type && !xr_xir_type_span(module->types, ops[0].type));
        CHECK(ops[4].type == module->functions[f].parameters[0]);
        CHECK(ops[2].args[0] == 2 && ops[4].args[0] == 2 && ops[4].args[1] == 4);
    }
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed); closed=NULL;
    CHECK(xr_xir_compile_artifact_verify(lowered, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_module(lowered)->functions[1].instructions[1].op == XR_XIR_OWNED_LOCAL_NEW);
    xr_xir_compile_artifact_free(lowered); lowered=NULL;
}
#endif // XIR_ARRAY_GENERIC_CASES_H
