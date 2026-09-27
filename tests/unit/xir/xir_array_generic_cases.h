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
#include "xir_array_generic_fixture.h"

static void array_generic_cases(void) {
    XrXirArtifact *checked = array_generic_fixture(), *decoded = NULL, *closed = NULL, *lowered = NULL;
    const XrXirModule *module = xr_xir_artifact_module(checked);
    XrXirFunction views[4]; memcpy(views, module->functions, 3 * sizeof(*views));
    views[3] = views[1]; views[3].name = "unused"; views[3].name_length = 6;
    XrXirGeneric generics[4]; memcpy(generics, module->generics, 3 * sizeof(*generics));
    generics[3] = generics[1];
    XrXirInstruction body[6]; memcpy(body, views[3].instructions, sizeof(body));
    views[3].instructions = body;
    XrXirModule built = *module; built.stage = XR_XIR_BUILT;
    built.functions = views; built.function_count = 4; built.generics = generics;
    CHECK(xr_xir_verify(&built, NULL, NULL) == XR_XIR_OK);
    body[4].args[1] = 0; /* T can be i64 in an instance; it is not i64 in the definition. */
    CHECK(xr_xir_verify(&built, NULL, NULL) == XR_XIR_BAD_TYPE);
    XrXirArtifact *rejected = (XrXirArtifact *) (uintptr_t) 1;
    CHECK(xr_xir_check(&built, NULL, &rejected, NULL) == XR_XIR_BAD_TYPE && !rejected);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xCC, packet.length); xr_xir_checked_packet_free(&packet);
    XrXirBudget budget = xr_xir_default_budget(); budget.functions = 2;
    CHECK(xr_xir_specialize(decoded, &budget, &closed, NULL) == XR_XIR_BUDGET && !closed);
    CHECK(xr_xir_specialize(decoded, NULL, &closed, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(decoded);
    module = xr_xir_artifact_module(closed);
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
    CHECK(xr_xir_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(closed);
    CHECK(xr_xir_artifact_verify(lowered, NULL, NULL) == XR_XIR_OK);
    CHECK(xr_xir_artifact_module(lowered)->functions[1].instructions[1].op == XR_XIR_OWNED_LOCAL_NEW);
    xr_xir_artifact_free(lowered);
}
#endif // XIR_ARRAY_GENERIC_CASES_H
