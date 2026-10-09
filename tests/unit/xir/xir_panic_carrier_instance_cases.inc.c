#include "xir_construction_fixture.h"
/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_panic_carrier_instance_cases.inc.c - Sticky failure ownership and early ABI rejection
 */
typedef struct PanicProgramFixture {
    XrXirArtifact *lowered;
    XrXirCallEntry entries[2];
    XrXirProgramSpec spec;
    uint32_t callbacks, releases;
    bool catch_panic;
    XrXirValue caught_message;
} PanicProgramFixture;
extern const XrXirCallEntry old_provider_entry;
uint32_t old_provider_callbacks(void);
size_t old_provider_entry_bytes(void);
static XrXirAction program_initialize(XrXirCallView *view) {
    PanicProgramFixture *f = (PanicProgramFixture *)view->environment;
    CarrierFrame *frame = view->state; ++f->callbacks;
    XrXirCallStatus status = xr_xir_instance_literal(view, 0, &frame->message);
    if (status != XR_XIR_CALL_READY)
        return xr_xir_call_fault(status == XR_XIR_CALL_OOM ? XR_XIR_RUN_OUT_OF_MEMORY : XR_XIR_RUN_STEP_LIMIT);
    XrXirAction assertion = xr_xir_call_assertion(&frame->message);
    if (!f->catch_panic) return assertion;
    uint32_t pc = 0;
    XrXirAction landed = xr_xir_instance_panic_land(view, frame, assertion,
        (uint32_t)offsetof(CarrierFrame, info_payload), 17, &pc);
    if (landed.kind != XR_XIR_ACTION_CONTINUE) return landed;
    XrXirValue info = {XR_XIR_PANIC_INFO, 0, frame->info_payload};
    CHECK(pc == 17 && xr_xir_value_valid(&info));
    XrXirValueStatus copied = xr_xir_panic_info_message(&info, &f->caught_message);
    if (copied != XR_XIR_VALUE_OK) return xr_xir_call_fault(XR_XIR_RUN_STEP_LIMIT);
    return control(XR_XIR_ACTION_RETURN);
}
static XrXirAction program_entry(XrXirCallView *view) {
    ++((PanicProgramFixture *)view->environment)->callbacks;
    return (XrXirAction){XR_XIR_ACTION_RETURN, 0, NULL, 0, {XR_XIR_I64, 0, 7}, {0}, 0};
}
static void program_release(XrXirCallView *view, XrXirCallStatus reason) {
    (void)reason; ++((PanicProgramFixture *)view->environment)->releases;
    xr_xir_value_drop(&((CarrierFrame *)view->state)->message);
    xr_xir_owned_slot_clear(view->state, (uint32_t)offsetof(CarrierFrame, info_payload));
}
static void program_fixture(PanicProgramFixture *f) {
    memset(f, 0, sizeof(*f));
    const XrXirInstruction init[] = {{XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}};
    const XrXirInstruction main_ops[] = {
        {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 7, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}
    };
    const XrXirBlock blocks[] = {{0, 1, 0, 0}, {0, 2, 0, 0}};
    const XrXirFunction functions[] = {
        {"init", 4, NULL, 0, XR_XIR_UNIT, blocks, 1, init, 1, NULL, 0},
        {"main", 4, NULL, 0, XR_XIR_I64, blocks + 1, 1, main_ops, 2, NULL, 0}
    };
    const XrXirSourceModule source = {"carrier-test", 12, NULL, 0, 0};
    const XrXirFunctionIdentity identities[2] = {{0}, {0}};
    const XrXirLiteral literal = {expected_bytes, sizeof(expected_bytes)};
    const XrXirDeclarations declarations = {&source, 1, identities, NULL, 0, &literal, 1, 0, 1, NULL};
    const XrXirModule built = {XR_XIR_BUILT, functions, 2, &declarations, NULL, NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrXirArtifact *checked = NULL, *closed = NULL;
    XrCompileResources *resources = NULL;
    const XrCompileResourceLimits limits = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    const XrXirCompileContext context = {resources, xr_xir_compile_default_limits()};
    CHECK(xir_fixture_check(&context, &built, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_OK);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &f->lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked); xr_xir_compile_artifact_free(closed);
    xr_compile_resources_release(resources);
    const XrXirModule *owned = xr_xir_compile_artifact_module(f->lowered);
    f->entries[0] = (XrXirCallEntry){XR_XIR_CALL_ABI_VERSION, NULL, 0, XR_XIR_UNIT,
        sizeof(CarrierFrame), program_initialize, program_release, f, 0, 0};
    f->entries[1] = (XrXirCallEntry){XR_XIR_CALL_ABI_VERSION, NULL, 0, XR_XIR_I64,
        sizeof(CarrierFrame), program_entry, program_release, f, 0, 0};
    f->spec = (XrXirProgramSpec){XR_XIR_PROGRAM_ABI_VERSION, target, f->entries, 2,
        owned->declarations, {0}, NULL, xr_xir_compile_program_proof(f->lowered)};
}
static void abi_cases(void) {
    PanicProgramFixture fixture; program_fixture(&fixture);
    size_t baseline_count = runtime_live, baseline_bytes = runtime_bytes;
    for (unsigned i = 0; i < 3; ++i) {
        XrXirProgramSpec invalid = fixture.spec; XrXirProgram *program = NULL;
        if (i == 0) invalid.abi_version = 25;
        if (i == 1) invalid.target.abi_version = 15;
        if (i == 2) fixture.entries[0].abi_version = 19;
        runtime_attempts = 0;
        CHECK(xr_xir_compile_program_seal(xr_xir_compile_artifact_context(fixture.lowered), &invalid, &program) == XR_XIR_BAD_LAYOUT);
        CHECK(!program && !runtime_attempts && !fixture.callbacks && !fixture.releases);
        CHECK(runtime_live == baseline_count && runtime_bytes == baseline_bytes);
        fixture.entries[0].abi_version = XR_XIR_CALL_ABI_VERSION;
    }
    XrXirCallAccounting accounting = {0};
    XrXirCallConfig config; CHECK(xr_xir_call_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY); config.entries = fixture.entries; config.entry_count = 2; config.instance = NULL; config.byte_limit = 65536; config.poll_limit = 32; config.depth_limit = 8; config.accounting = &accounting; config.output = (XrXirOutputProvider) {0}; config.admission = (XrXirValueAdmission) {0};
    fixture.entries[1].abi_version = 19;
    XrXirCall *call = NULL; runtime_attempts = 0;
    CHECK(xr_xir_call_new(&config, 0, NULL, 0, &call) == XR_XIR_CALL_BAD_ABI && !call && !runtime_attempts);
    CHECK(!fixture.callbacks && !fixture.releases);
    fixture.entries[1].abi_version = XR_XIR_CALL_ABI_VERSION;
    CHECK(old_provider_entry_bytes() == sizeof(XrXirCallEntry));
    XrXirCallConfig old_config; CHECK(xr_xir_call_config_init(&old_config, sizeof(old_config)) == XR_XIR_CALL_READY); old_config.entries = &old_provider_entry; old_config.entry_count = 1; old_config.instance = NULL; old_config.byte_limit = 65536; old_config.poll_limit = 32; old_config.depth_limit = 8; old_config.accounting = &accounting; old_config.output = (XrXirOutputProvider) {0}; old_config.admission = (XrXirValueAdmission) {0};
    runtime_attempts = 0;
    CHECK(xr_xir_call_new(&old_config, 0, NULL, 0, &call) == XR_XIR_CALL_BAD_ABI && !call && !runtime_attempts);
    XrXirCallEntry saved = fixture.entries[0]; fixture.entries[0] = old_provider_entry;
    XrXirProgram *refused = NULL; runtime_attempts = 0;
    CHECK(xr_xir_compile_program_seal(xr_xir_compile_artifact_context(fixture.lowered), &fixture.spec, &refused) == XR_XIR_BAD_LAYOUT);
    CHECK(!refused && !runtime_attempts && !old_provider_callbacks()); fixture.entries[0] = saved;
    const XrXirProgramProof proof = fixture.spec.proof;
    CHECK(proof.length >= 64);
    uint8_t *old = xr_malloc(proof.length); CHECK(old);
    memcpy(old, proof.bytes, proof.length); old[12] = 53; old[13] = old[14] = old[15] = 0;
    uint8_t digest[32]; checked_digest(old, proof.length, digest); memcpy(old + 32, digest, 32);
    /* Even a correctly rehashed old contract rejects before body allocation. */
    XrXirArtifact *output = NULL; runtime_attempts = 0;
    CHECK(xr_xir_compile_checked_read(xr_xir_compile_artifact_context(fixture.lowered), old, proof.length, &output, NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(!output && !runtime_attempts); xr_free(old);
    xr_xir_compile_artifact_free(fixture.lowered); physical_empty();
}
static XrXirCallStatus instance_once(PanicProgramFixture *f, size_t fault_index, size_t *sites) {
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(xr_xir_compile_artifact_context(f->lowered), &f->spec, &program) == XR_XIR_OK);
    size_t baseline_count = runtime_live, baseline_bytes = runtime_bytes;
    runtime_attempts = 0; runtime_fail_at = fault_index;
    XrXirInstance *instance = NULL;
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    XrXirCallStatus status = xr_xir_instance_new(program, &config, &instance);
    if (status == XR_XIR_CALL_READY) status = xr_xir_instance_start(instance, 1, NULL, 0);
    XrXirCallResult held = {0}, again = {0};
    if (status == XR_XIR_CALL_READY) {
        XrXirCallResult result = xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome;
        status = result.status;
        CHECK(status == XR_XIR_CALL_ASSERTION || status == XR_XIR_CALL_OOM || status == XR_XIR_CALL_LIMIT);
        CHECK(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_FAILED && result.value.type == XR_XIR_UNIT);
        size_t attempts = runtime_attempts;
        CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status == status && runtime_attempts == attempts);
        CHECK(xr_xir_instance_copy_failure(instance, &held) == status);
        CHECK(xr_xir_instance_copy_failure(instance, &again) == status);
        if (status == XR_XIR_CALL_ASSERTION) {
            expect_bytes(&held.panic.message, expected_bytes, sizeof(expected_bytes));
            XirObject *object = object_pointer(&held.panic.message);
            uint32_t references = atomic_load(&object->references);
            atomic_store(&object->references, UINT32_MAX);
            XrXirCallResult failed = {0};
            CHECK(xr_xir_instance_copy_failure(instance, &failed) == XR_XIR_CALL_LIMIT && xr_xir_call_result_empty(&failed));
            atomic_store(&object->references, references);
        }
    }
    if (instance) CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    if (status == XR_XIR_CALL_ASSERTION) {
        expect_bytes(&held.panic.message, expected_bytes, sizeof(expected_bytes));
        expect_bytes(&again.panic.message, expected_bytes, sizeof(expected_bytes));
    }
    xr_xir_call_result_drop(&held); xr_xir_call_result_drop(&again);
    CHECK(runtime_live == baseline_count && runtime_bytes == baseline_bytes);
    if (sites) *sites = runtime_attempts;
    runtime_fail_at = SIZE_MAX;
    xr_xir_compile_program_drop(program);
    return status;
}
static void instance_cases(void) {
    PanicProgramFixture f; program_fixture(&f);
    size_t sites = 0; CHECK(instance_once(&f, SIZE_MAX, &sites) == XR_XIR_CALL_ASSERTION);
    for (size_t point = 0; point < sites; ++point) {
        XrXirCallStatus status = instance_once(&f, point, NULL);
        CHECK(status == XR_XIR_CALL_OOM || status == XR_XIR_CALL_LIMIT);
    }
    printf("Sticky failure owner-independent copies and OOM physical gates PASS; instance sites=%zu\n", sites);
    xr_xir_compile_artifact_free(f.lowered); physical_empty();
}
static XrXirCallStatus handler_once(PanicProgramFixture *f, size_t point, size_t *sites) {
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(xr_xir_compile_artifact_context(f->lowered), &f->spec, &program) == XR_XIR_OK);
    size_t baseline_count = runtime_live, baseline_bytes = runtime_bytes;
    runtime_attempts = 0; runtime_fail_at = point;
    XrXirInstance *instance = NULL;
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    XrXirCallStatus status = xr_xir_instance_new(program, &config, &instance);
    if (status == XR_XIR_CALL_READY) status = xr_xir_instance_start(instance, 1, NULL, 0);
    if (status == XR_XIR_CALL_READY) {
        XrXirCallResult result = xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome;
        status = result.status;
        CHECK(xr_xir_call_result_valid(&result));
        if (status == XR_XIR_CALL_RETURNED) {
            CHECK(result.value.type == XR_XIR_I64 && result.value.payload == 7 && xr_xir_panic_empty(&result.panic));
        } else CHECK(status == XR_XIR_CALL_OOM || status == XR_XIR_CALL_LIMIT);
    }
    if (instance) CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    if (status == XR_XIR_CALL_RETURNED) expect_bytes(&f->caught_message, expected_bytes, sizeof(expected_bytes));
    xr_xir_value_drop(&f->caught_message);
    CHECK(runtime_live == baseline_count && runtime_bytes == baseline_bytes);
    if (sites) *sites = runtime_attempts;
    runtime_fail_at = SIZE_MAX; xr_xir_compile_program_drop(program);
    return status;
}
static void handler_cases(void) {
    PanicProgramFixture f; program_fixture(&f); f.catch_panic = true;
    size_t sites = 0; CHECK(handler_once(&f, SIZE_MAX, &sites) == XR_XIR_CALL_RETURNED);
    for (size_t point = 0; point < sites; ++point) {
        XrXirCallStatus status = handler_once(&f, point, NULL);
        CHECK(status == XR_XIR_CALL_OOM || status == XR_XIR_CALL_LIMIT);
    }
    printf("Typed assertion PanicInfo handler and OOM physical gates PASS; handler sites=%zu\n", sites);
    xr_xir_compile_artifact_free(f.lowered); physical_empty();
}
