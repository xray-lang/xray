/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generics.c - Definition proof, specialization and template ownership
 *
 * KEY CONCEPT:
 *   Malformed templates fail before a concrete instance can hide their mistakes.
 */
#include "xir/xxir_generic.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_vm.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_generic_fixture.h"
static void rehash_generic(XrXirCheckedPacket *packet) {
    XrSHA256Context sha; xr_sha256_init(&sha);
    xr_sha256_update(&sha, packet->bytes, 32);
    xr_sha256_update(&sha, packet->bytes + 64, packet->length - 64);
    xr_sha256_final(&sha, packet->bytes + 32);
}
static void rejected_templates(void) {
    for (unsigned mode = 0; mode < 10; ++mode) {
        XrXirArtifact *checked = generic_fixture();
        XrXirFunction *functions = (XrXirFunction *) checked->module.functions;
        XrXirGeneric *generics = (XrXirGeneric *) checked->module.generics;
        XrXirInstruction *caller = (XrXirInstruction *) functions[0].instructions;
        XrXirInstruction *body = (XrXirInstruction *) functions[1].instructions;
        if (mode == 0) ((uint32_t *) generics[1].constraints)[0] = 2;
        if (mode == 1) caller[1].targets[0] = 0;
        if (mode == 2) caller[1].targets[1] = 0;
        if (mode == 3) ((XrXirType *) generics[0].arguments)[0] = XR_XIR_UNIT;
        if (mode == 4) ((XrXirType *) generics[0].arguments)[0] = (XrXirType) XR_XIR_TYPE_PARAMETER_BASE;
        if (mode == 5) body[0].type = (XrXirType) (XR_XIR_TYPE_PARAMETER_BASE + 1);
        if (mode == 6) body[0].op = XR_XIR_SCALAR_COPY;
        if (mode == 7) body[0].op = XR_XIR_CONST_I64;
        if (mode == 8) caller[0].type = XR_XIR_BOOL;
        if (mode == 9) generics[0].argument_count = 2;
        CHECK(xr_xir_artifact_verify(checked, NULL, NULL) != XR_XIR_OK);
        XrXirArtifact *closed = NULL; XrXirCheckedPacket packet = {0};
        CHECK(xr_xir_specialize(checked, NULL, &closed, NULL) != XR_XIR_OK && !closed);
        CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) != XR_XIR_OK && !packet.bytes);
        xr_xir_artifact_free(checked);
    }
}
static void forwarding(void) {
    XrXirArtifact *checked = generic_fixture();
    XrXirFunction *functions = (XrXirFunction *) checked->module.functions;
    XrXirGeneric generics[2] = {checked->module.generics[0], checked->module.generics[1]};
    uint32_t constraint = 0;
    const XrXirType t = (XrXirType) XR_XIR_TYPE_PARAMETER_BASE;
    XrXirType parameters[] = {t, t}, types[] = {t, t, t};
    generics[0].parameter_count = 1; generics[0].constraints = &constraint; generics[0].arguments = types;
    XrXirFunction caller = functions[0];
    caller.parameters = parameters; caller.result = t;
    XrXirInstruction ops[4]; memcpy(ops, caller.instructions, sizeof(ops));
    for (unsigned i = 0; i < 3; ++i) ops[i].type = t;
    caller.instructions = ops;
    XrXirFunction views[] = {caller, functions[1]};
    XrXirModule module = {XR_XIR_BUILT, views, 2, NULL, generics, NULL};
    CHECK(xr_xir_verify(&module, NULL, NULL) == XR_XIR_BAD_TYPE);
    constraint = XR_XIR_CONSTRAINT_SENDABLE;
    CHECK(xr_xir_verify(&module, NULL, NULL) == XR_XIR_OK);
    XrXirArtifact *valid = NULL, *forged = NULL;
    CHECK(xr_xir_check(&module, NULL, &valid, NULL) == XR_XIR_OK);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(valid, NULL, &packet, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(valid);
    CHECK(packet.length > 40 && packet.bytes[packet.length - 36] == XR_XIR_CONSTRAINT_SENDABLE);
    packet.bytes[packet.length - 36] = 0; rehash_generic(&packet);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &forged, NULL) == XR_XIR_BAD_TYPE && !forged);
    xr_xir_checked_packet_free(&packet);
    xr_xir_artifact_free(checked);
}
static void specialized_result(XrXirArtifact *lowered) {
    XrXirVmBinding bindings[3]; XrXirCallEntry entries[3];
    for (uint32_t f = 0; f < 3; ++f) CHECK(xr_xir_vm_bind(lowered, f, &bindings[f], &entries[f]) == XR_XIR_OK);
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    XrXirDomainStats baseline = xr_xir_domain_stats(domain);
    XrXirValue arguments[] = {{XR_XIR_I64, 0, 7}, {0}}, result = {0};
    CHECK(xr_xir_string_new(domain, "generic", 7, &arguments[1]) == XR_XIR_VALUE_OK);
    XrXirCallAccounting accounting = {0};
    XrXirCallConfig config = {entries, 3, NULL, 65536, 100, 8, &accounting, {NULL, NULL}};
    XrXirCall *call = NULL;
    CHECK(xr_xir_call_new(&config, 0, arguments, 2, &call) == XR_XIR_CALL_READY);
    CHECK(xr_xir_call_poll(call).status == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_call_take_result(call, &result) == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
    CHECK(!accounting.live_bytes && accounting.allocations == accounting.frees);
    xr_xir_value_drop(&arguments[1]); xr_xir_artifact_free(lowered);
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(&result, &bytes, &length) && length == 7 && !memcmp(bytes, "generic", 7));
    xr_xir_value_drop(&result);
    XrXirDomainStats stats = xr_xir_domain_stats(domain);
    CHECK(stats.live_bytes == baseline.live_bytes && stats.allocations == stats.frees + 1);
    xr_xir_domain_drop(domain);
}
static void recursive_closure(void) {
    XrXirArtifact *fixture = generic_fixture(), *checked = NULL, *closed = NULL;
    XrXirModule built = *xr_xir_artifact_module(fixture); built.stage = XR_XIR_BUILT;
    XrXirFunction functions[2]; memcpy(functions, built.functions, sizeof(functions));
    XrXirGeneric generics[2]; memcpy(generics, built.generics, sizeof(generics));
    const XrXirType t = (XrXirType) XR_XIR_TYPE_PARAMETER_BASE;
    const uint32_t operand = 0;
    XrXirInstruction body[] = {{XR_XIR_CALL, t, {0, 1}, {0, 1}, 1},
        {XR_XIR_RETURN, XR_XIR_UNIT, {1}, {0}, 0}};
    functions[1].instructions = body; functions[1].operands = &operand; functions[1].operand_count = 1;
    generics[1].arguments = &t; generics[1].argument_count = 1;
    built.functions = functions; built.generics = generics;
    CHECK(xr_xir_check(&built, NULL, &checked, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(fixture);
    CHECK(xr_xir_specialize(checked, NULL, &closed, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    const XrXirModule *module = xr_xir_artifact_module(closed);
    CHECK(module->function_count == 3 && module->functions[1].instructions[0].immediate == 1 &&
        module->functions[2].instructions[0].immediate == 2);
    xr_xir_artifact_free(closed);
}
int main(void) {
    rejected_templates(); forwarding(); recursive_closure();
    XrXirArtifact *checked = generic_fixture(), *decoded = NULL, *closed = NULL, *lowered = NULL;
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(checked, &target, NULL, &lowered, NULL) == XR_XIR_BAD_STAGE && !lowered);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    packet.bytes[8] = 1; packet.bytes[12] = 1; rehash_generic(&packet);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_BAD_STRUCTURE && !decoded);
    packet.bytes[8] = 2; packet.bytes[12] = 2; rehash_generic(&packet);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_BAD_STRUCTURE && !decoded);
    packet.bytes[12] = 3; rehash_generic(&packet);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_BAD_STRUCTURE && !decoded);
    packet.bytes[12] = 4; rehash_generic(&packet);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_BAD_STRUCTURE && !decoded);
    packet.bytes[12] = 5; rehash_generic(&packet);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_BAD_STRUCTURE && !decoded);
    packet.bytes[12] = 6; rehash_generic(&packet);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_BAD_STRUCTURE && !decoded);
    packet.bytes[8] = XR_XIR_CHECKED_SCHEMA; packet.bytes[12] = XR_XIR_CHECKED_CONTRACT; rehash_generic(&packet);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OK);
    xr_xir_checked_packet_free(&packet);
    XrXirBudget budget = xr_xir_default_budget(); budget.functions = 2;
    CHECK(xr_xir_specialize(decoded, &budget, &closed, NULL) == XR_XIR_BUDGET && !closed);
    CHECK(xr_xir_specialize(decoded, NULL, &closed, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(decoded);
    const XrXirModule *module = xr_xir_artifact_module(closed);
    CHECK(module->stage == XR_XIR_CHECKED && !module->generics && module->function_count == 3);
    CHECK(module->functions[0].instructions[1].immediate == module->functions[0].instructions[2].immediate);
    CHECK(module->functions[0].instructions[0].immediate != module->functions[0].instructions[1].immediate);
    CHECK(xr_xir_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(closed);
    module = xr_xir_artifact_module(lowered);
    CHECK(module->functions[1].instructions[0].op == XR_XIR_SCALAR_COPY);
    CHECK(module->functions[2].instructions[0].op == XR_XIR_OWNED_RETAIN);
    specialized_result(lowered);
    puts("Generic definition constraints, packet ownership and Checked specialization passed");
    return 0;
}
