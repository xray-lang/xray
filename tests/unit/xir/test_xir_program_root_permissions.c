/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_program_root_permissions.c - Sealed facts and authentic callable gates
 *
 * KEY CONCEPT:
 *   Trusted callbacks try false carrier claims against independently sealed bodies.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_effects.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_program_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_callable_root_owner.h"
#include "xir_program_root_fixture.h"
#include "xir/xxir_instance.c"

static uint32_t probe_entry, probe_mode;
static XrXirType probe_type;
static XrXirValue probe_capture, probe_input, probe_output;
static XrXirCallStatus probe_expected;
static XrXirAction permission_probe(XrXirCallView *view) {
    XrXirInstance *instance = view_instance(view);
    CHECK(instance && !probe_output.type && !probe_output.payload && !probe_output.reserved);
    XrXirDomainStats before = xr_xir_domain_stats(instance->domain);
    uint32_t refs = instance->function_gate ? atomic_load(&instance->function_gate->references) : 0;
    uint64_t metadata = instance->metadata_bytes;
    XrXirCallStatus status;
    if (!probe_mode) {
        bool captured = probe_entry == 8 || probe_entry == 9;
        status = xr_xir_instance_function(view, probe_type, probe_entry,
            captured ? &probe_capture : NULL, captured ? 1u : 0u, &probe_output);
    } else if (probe_mode == 1) {
        status = xr_xir_instance_weaken_function(view, probe_type, &probe_input, &probe_output);
    } else {
        uint32_t entry = 701;
        status = xr_xir_instance_resolve_function(view, &probe_input, &entry);
        CHECK(status == XR_XIR_CALL_READY ? entry == probe_entry : entry == 701);
    }
    if (status != probe_expected) fprintf(stderr, "probe mode%u entry%u type%u status%u expected%u\n",
        probe_mode, probe_entry, probe_type, status, probe_expected);
    CHECK(status == probe_expected);
    if (status != XR_XIR_CALL_READY) {
        XrXirDomainStats after = xr_xir_domain_stats(instance->domain);
        CHECK(!probe_output.type && !probe_output.payload && !probe_output.reserved);
        CHECK(!memcmp(&before, &after, sizeof(before)) && metadata == instance->metadata_bytes);
        CHECK(refs == (instance->function_gate ? atomic_load(&instance->function_gate->references) : 0));
    }
    return (XrXirAction){XR_XIR_ACTION_RETURN, 0, NULL, 0, {0}, {0}, 0};
}
static void permission_drive(XrXirInstance *instance) {
    CHECK(xr_xir_instance_start(instance, 4, NULL, 0) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
    XrXirValue unit = {0};
    CHECK(xr_xir_instance_take_result(instance, &unit) == XR_XIR_CALL_RETURNED);
    CHECK(!unit.type && !unit.reserved && !unit.payload);
}
static XrXirValue permission_factory(XrXirInstance *instance, uint32_t entry,
    uint32_t type, XrXirCallStatus expected) {
    probe_mode = 0; probe_entry = entry;
    probe_type = (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + type);
    probe_expected = expected;
    permission_drive(instance);
    XrXirValue result = probe_output; probe_output = (XrXirValue){0};
    return result;
}
typedef struct PermissionLease { XrXirArtifact *artifact; uint32_t releases; } PermissionLease;
static void permission_release(void *owner) {
    PermissionLease *lease = owner;
    CHECK(!lease->releases++);
    xr_xir_compile_artifact_free(lease->artifact); lease->artifact = NULL;
}
static void permission_runtime(void) {
    static const bool facts[13][2] = {
        {true,false},{false,false},{true,false},{false,false},{false,false},
        {false,false},{false,false},{true,false},{false,true},{true,true},
        {true,true},{false,true},{false,true}};
    static const uint32_t targets[] = {3,2,8,9};
    static const bool accepted[4][4] = {
        {true,true,true,true},{false,true,true,true},
        {false,false,true,true},{false,false,true,true}};
    BoundMark mark = bound_mark();
    XrXirCompileContext context = bound_owner(bound_caps());
    XrXirArtifact *checked = NULL, *specialized = NULL, *lowered = NULL;
    CHECK(permission_fixture(&context, 0, 3, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked, &specialized, NULL) == XR_XIR_OK);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(specialized, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked); xr_xir_compile_artifact_free(specialized);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    CHECK(module->function_count == 13);
    XrXirVmBinding bindings[13]; XrXirCallEntry entries[13];
    for (uint32_t f = 0; f < 13; ++f)
        CHECK(xr_xir_compile_vm_bind(lowered, f, &bindings[f], &entries[f]) == XR_XIR_OK);
    entries[4].resume = permission_probe; entries[4].release = NULL;
    PermissionLease lease = {lowered, 0};
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, target, entries, 13,
        module->declarations, {&lease, permission_release}, module->types, xr_xir_compile_program_proof(lowered)};
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(&context, &spec, &program) == XR_XIR_OK && program);
    CHECK(program->permissions && program->permissions->function_count == 13 && program->permissions->slot_count == 1);
    for (uint32_t f = 0; f < 13; ++f) {
        CHECK(program->permissions->entries[f].requires_root == facts[f][0]);
        CHECK(program->permissions->entries[f].unresolved == facts[f][1]);
    }
    CHECK(program->permissions->entries[0].worker == XR_XIR_BAD_TYPE);
    CHECK(program->permissions->entries[3].worker == XR_XIR_OK);
    /* A pure body with a non-Sendable Fn parameter still fails worker admission. */
    CHECK(program->permissions->entries[6].worker == XR_XIR_BAD_TYPE && !facts[6][0] && !facts[6][1]);
    CHECK(program->permissions->entries[13].worker == XR_XIR_BAD_TYPE);
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    XrXirInstance *a = NULL, *b = NULL;
    CHECK(xr_xir_instance_new(program, &config, &a) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program, &config, &b) == XR_XIR_CALL_READY);
    XrXirValue pure = permission_factory(a, 3, 0, XR_XIR_CALL_READY);
    XrXirValue foreign = permission_factory(b, 3, 0, XR_XIR_CALL_READY);
    XrXirValue captures[2] = {
        permission_factory(a, 3, 2, XR_XIR_CALL_READY),
        permission_factory(a, 3, 3, XR_XIR_CALL_READY)};
    for (uint32_t row = 0; row < 4; ++row) for (uint32_t type = 0; type < 4; ++type) {
        probe_capture = row >= 2 ? captures[row - 2] : (XrXirValue){0};
        XrXirValue result = permission_factory(a, targets[row], type,
            accepted[row][type] ? XR_XIR_CALL_READY : XR_XIR_CALL_BAD_ARGUMENT);
        xr_xir_value_drop(&result);
    }
    probe_capture = (XrXirValue){0};
    probe_mode = 1; probe_type = (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + 2);
    probe_input = pure; probe_expected = XR_XIR_CALL_READY; permission_drive(a);
    XrXirValue weakened = probe_output; probe_output = (XrXirValue){0};
    CHECK(weakened.type == (uint32_t)probe_type);
    probe_input = weakened; probe_type = XR_XIR_CONSTRUCTED_TYPE_BASE;
    probe_expected = XR_XIR_CALL_BAD_ARGUMENT; permission_drive(a);
    probe_input = foreign; probe_type = (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + 2);
    permission_drive(a);
    /* The carrier is structurally valid; the false real-body claim is rejected. */
    XrXirFunctionBinding *forged = (XrXirFunctionBinding *)xr_xir_function_binding(&pure);
    CHECK(forged && forged->entry == 3);
    forged->entry = 2; probe_input = pure;
    CHECK(xr_xir_value_valid(&pure));
    CHECK(xr_xir_instance_start_function(a, &pure, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
    probe_mode = 2; permission_drive(a);
    probe_mode = 1; permission_drive(a);
    forged->entry = 3;
    CHECK(xr_xir_instance_start_function(a, &pure, NULL, 0) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(a, UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
    XrXirValue result = {0};
    CHECK(xr_xir_instance_take_result(a, &result) == XR_XIR_CALL_RETURNED);
    CHECK(result.type == XR_XIR_I64 && result.payload == 3); xr_xir_value_drop(&result);
    xr_xir_value_drop(&foreign); xr_xir_value_drop(&captures[0]); xr_xir_value_drop(&captures[1]);
    xr_xir_value_drop(&weakened); probe_input = (XrXirValue){0};
    CHECK(xr_xir_instance_free(b) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_free(a) == XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    CHECK(!lease.releases && xr_xir_value_valid(&pure));
    xr_xir_value_drop(&pure);
    CHECK(lease.releases == 1 && !lease.artifact);
    bound_owner_free(&context); bound_balanced(mark);
    puts("13 sealed body facts, 16 factory bounds, real binding/retag isolation and final lease physical0 PASS");
}
int main(void) {
    permission_runtime(); return 0;
}
