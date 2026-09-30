/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_instances.c - Sealed code, initialization and independent state
 *
 * KEY CONCEPT:
 *   Host callbacks exercise the runtime contract without a compiler oracle.
 */
#include "xir/xxir_program.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)

#include "xir_native_metadata_fixture.h"
#include "xir_error_fixture.h"

typedef struct Witness { uint32_t mode, releases, begins[3]; } Witness;
typedef struct Environment { Witness *witness; uint32_t module; } Environment;
typedef struct Frame { uint32_t phase; XrXirValue value; int64_t sum; } Frame;
typedef struct Trace { uint32_t events[64], count; XrXirInstance *instance; } Trace;
static XrXirAction action(XrXirActionKind kind, XrXirValue value) {
    return (XrXirAction) {kind, 0, NULL, 0, value, {0}, 0};
}
static XrXirAction done(void) { return action(XR_XIR_ACTION_RETURN, (XrXirValue) {0}); }
static XrXirAction fault(XrXirCallStatus status) {
    CHECK(status != XR_XIR_CALL_READY);
    return action(XR_XIR_ACTION_FAULT, (XrXirValue) {0});
}
static void cleanup(XrXirCallView *view, XrXirCallStatus reason) {
    (void) reason;
    Frame *frame = view->state;
    xr_xir_value_drop(&frame->value);
}
static XrXirAction initializer(XrXirCallView *view) {
    const Environment *env = view->environment;
    Frame *frame = view->state;
    uint32_t module = env->module, slot = module == 2 ? 0 : module == 1 ? 1 : 2;
    if (!frame->phase) {
        ++env->witness->begins[module];
        CHECK(xr_xir_instance_start(view->instance, 3, NULL, 0) == XR_XIR_CALL_BUSY);
        CHECK(xr_xir_instance_free(view->instance) == XR_XIR_CALL_BUSY);
        if (env->witness->mode == 3 && module == 2) return done();
        XrXirCallStatus status = module ? xr_xir_instance_atomic(view, module == 2 ? 10 : 20, &frame->value) :
            xr_xir_instance_literal(view, 0, &frame->value);
        if (status != XR_XIR_CALL_READY) return fault(status);
        CHECK(xr_xir_instance_slot_write(view, slot, &frame->value, true) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_slot_write(view, slot, &frame->value, true) == XR_XIR_CALL_BAD_STATE);
        xr_xir_value_drop(&frame->value);
        frame->phase = 1;
        if (module == 2 && (env->witness->mode == 1 || env->witness->mode == 2))
            return action(XR_XIR_ACTION_SUSPEND, (XrXirValue) {0});
    }
    if (module == 2 && env->witness->mode == 2) {
        frame->value=error_fixture_code(xr_xir_call_admission(view),91);
        return action(XR_XIR_ACTION_THROW,frame->value);
    }
    if (module == 2 && env->witness->mode == 4)
        return xr_xir_call_bounds(INT64_MIN, 3);
    return done();
}
static XrXirAction increment(XrXirCallView *view) {
    const Environment *env = view->environment;
    Frame *frame = view->state;
    uint32_t slot = env->module == 2 ? 0 : 1;
    CHECK(xr_xir_instance_slot_read(view, slot, &frame->value) == XR_XIR_CALL_READY);
    XrXirValue absent = {0};
    CHECK(xr_xir_instance_slot_read(view, 2, &absent) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_slot_write(view, slot, &frame->value, false) == XR_XIR_CALL_BAD_STATE);
    int64_t previous = 0;
    CHECK(xr_xir_atomic_i64_fetch_add(&frame->value, 1, &previous));
    return action(XR_XIR_ACTION_RETURN, (XrXirValue) {XR_XIR_I64, 0, previous});
}
static XrXirAction root(XrXirCallView *view) {
    Frame *frame = view->state;
    if (frame->phase == 0) {
        frame->phase = 1;
        return (XrXirAction) {XR_XIR_ACTION_CALL, 4, NULL, 0, {0}, {0}, 0};
    }
    CHECK(view->inbox.status == XR_XIR_CALL_RETURNED);
    if (frame->phase == 1) {
        frame->sum = view->inbox.value.payload;
        frame->phase = 2;
        return (XrXirAction) {XR_XIR_ACTION_CALL, 5, NULL, 0, {0}, {0}, 0};
    }
    return action(XR_XIR_ACTION_RETURN, (XrXirValue) {XR_XIR_I64, 0, frame->sum + view->inbox.value.payload});
}
static XrXirAction string_result(XrXirCallView *view) {
    Frame *frame = view->state;
    CHECK(xr_xir_instance_slot_read(view, 2, &frame->value) == XR_XIR_CALL_READY);
    return action(XR_XIR_ACTION_RETURN, frame->value);
}
static XrXirAction echo(XrXirCallView *view) {
    CHECK(xr_xir_instance_slot_write(view, 2, &view->arguments[0], false) == XR_XIR_CALL_READY);
    return action(XR_XIR_ACTION_RETURN, view->arguments[0]);
}
static XrXirAction suspended_string(XrXirCallView *view) {
    Frame *frame = view->state;
    if (!frame->phase++) return action(XR_XIR_ACTION_SUSPEND, (XrXirValue) {0});
    CHECK(xr_xir_instance_literal(view, 1, &frame->value) == XR_XIR_CALL_READY);
    return action(XR_XIR_ACTION_RETURN, frame->value);
}
static XrXirAction atomic_result(XrXirCallView *view) {
    Frame *frame = view->state;
    CHECK(xr_xir_instance_slot_read(view, 0, &frame->value) == XR_XIR_CALL_READY);
    return action(XR_XIR_ACTION_RETURN, frame->value);
}
static void release(void *owner) { ++((Witness *) owner)->releases; }
static void trace(void *context, XrXirLifecycleEvent event, uint32_t index) {
    Trace *log = context;
    CHECK(log->count < 64);
    log->events[log->count++] = (uint32_t) event * 10 + index;
    CHECK(xr_xir_instance_free(log->instance) == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_stop(log->instance) == XR_XIR_CALL_BUSY);
}
typedef struct Fixture {
    Witness witness;
    Environment env[3];
    uint32_t dependencies[2];
    XrXirSourceModule modules[3];
    XrXirFunctionIdentity identities[10];
    XrXirSlot slots[3];
    XrXirLiteral literals[2];
    XrXirCallEntry entries[10];
    XrXirDeclarations declarations;
    XrXirProgramSpec spec;
    XrXirArtifact *proof;
    ErrorFixture error;
} Fixture;
static const XrXirType string_parameter = XR_XIR_STRING;
static void fixture(Fixture *f, uint32_t mode) {
    memset(f, 0, sizeof(*f));
    f->witness.mode = mode;
    for (uint32_t i = 0; i < 3; ++i) f->env[i] = (Environment) {&f->witness, i};
    f->dependencies[0] = 1; f->dependencies[1] = 2;
    f->modules[0] = (XrXirSourceModule) {"root", 4, f->dependencies, 2, 0};
    f->modules[1] = (XrXirSourceModule) {"beta", 4, NULL, 0, 1};
    f->modules[2] = (XrXirSourceModule) {"alpha", 5, NULL, 0, 2};
    uint32_t owners[] = {0, 1, 2, 0, 2, 1, 0, 0, 0, 2};
    for (uint32_t i = 0; i < 10; ++i) {
        f->identities[i] = (XrXirFunctionIdentity) {owners[i], i >= 4, 0, 0, 0, 0, XR_XIR_NON_MEMBER};
        f->entries[i] = (XrXirCallEntry) {XR_XIR_CALL_ABI_VERSION, NULL, 0, XR_XIR_UNIT,
            sizeof(Frame), initializer, cleanup, &f->env[owners[i]], 0, 0};
    }
    f->entries[3].resume = root; f->entries[3].result = XR_XIR_I64;
    for (uint32_t i = 4; i <= 5; ++i) {
        f->entries[i].resume = increment; f->entries[i].result = XR_XIR_I64;
    }
    f->entries[6].resume = string_result; f->entries[6].result = XR_XIR_STRING;
    f->entries[7].resume = echo; f->entries[7].result = XR_XIR_STRING;
    f->entries[7].parameters = &string_parameter; f->entries[7].parameter_count = 1;
    f->entries[8].resume = suspended_string; f->entries[8].result = XR_XIR_STRING;
    f->entries[9].resume = atomic_result; f->entries[9].result = XR_XIR_ATOMIC_I64;
    f->slots[0] = (XrXirSlot) {2, XR_XIR_ATOMIC_I64, 0};
    f->slots[1] = (XrXirSlot) {1, XR_XIR_ATOMIC_I64, 0};
    f->slots[2] = (XrXirSlot) {0, XR_XIR_STRING, 1};
    f->literals[0] = (XrXirLiteral) {"A\0\xe4\xb8\xad", 5};
    f->literals[1] = (XrXirLiteral) {"independent", 11};
    f->declarations = (XrXirDeclarations) {f->modules, 3, f->identities, f->slots, 3, f->literals, 2, 0, 3, NULL};
    f->spec = (XrXirProgramSpec) {XR_XIR_PROGRAM_ABI_VERSION,
        {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}, f->entries, 10, &f->declarations, {&f->witness, release}, NULL, {0}};
    if (mode==2) { error_fixture_init(&f->error,false); f->spec.types=&f->error.types; }
    CHECK(native_metadata_fixture(&f->spec, &f->proof) == XR_XIR_OK);
    f->spec.types=xr_xir_artifact_module(f->proof)->types;
    f->spec.proof = xr_xir_program_proof(f->proof);
}
static XrXirInstance *new_instance(XrXirProgram *program, Trace *log) {
    XrXirInstanceConfig config = xr_xir_instance_defaults();
    config.trace = trace; config.trace_context = log;
    CHECK(xr_xir_instance_new(program, &config, &log->instance) == XR_XIR_CALL_READY);
    return log->instance;
}
static XrXirValue run(XrXirInstance *instance, uint32_t entry) {
    CHECK(xr_xir_instance_start(instance, entry, NULL, 0) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll(instance).outcome.status == XR_XIR_CALL_RETURNED);
    XrXirValue value = {0};
    CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
    XrXirValue second = {0};
    CHECK(xr_xir_instance_take_result(instance, &second) == XR_XIR_CALL_BAD_STATE);
    return value;
}
static void strings_equal(const XrXirValue *value, const char *expected, size_t count) {
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(value, &bytes, &length));
    CHECK(length == count && !memcmp(bytes, expected, count));
}
static void isolation(void) {
    Fixture f; fixture(&f, 1);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_program_seal(&f.spec, (XrXirProgramBudget) {2097152, 16000000}, &program) == XR_XIR_OK);
    xr_xir_artifact_free(f.proof); f.proof = NULL;
    /* Metadata inputs may die or change after sealing; code environments stay leased. */
    f.modules[2].name = "other"; f.dependencies[0] = 99; f.literals[0].bytes = "wrong";
    f.slots[0].module = 0; f.identities[9].module = 0; f.entries[3].resume = NULL;
    Trace a = {0}, b = {0};
    XrXirInstance *first = new_instance(program, &a), *second = new_instance(program, &b);
    xr_xir_program_drop(program);
    CHECK(!f.witness.releases);
    CHECK(xr_xir_instance_start(first, 3, NULL, 0) == XR_XIR_CALL_READY);
    XrXirInstanceResult suspended = xr_xir_instance_poll(first);
    CHECK(suspended.outcome.status == XR_XIR_CALL_SUSPENDED);
    CHECK(xr_xir_instance_state(first) == XR_XIR_INSTANCE_INITIALIZING);
    CHECK(xr_xir_instance_start(first, 3, NULL, 0) == XR_XIR_CALL_BUSY);
    CHECK(a.count == 2 && a.events[0] == 2 && a.events[1] == 20);
    CHECK(xr_xir_instance_start(second, 3, NULL, 0) == XR_XIR_CALL_READY);
    XrXirInstanceResult other = xr_xir_instance_poll(second);
    CHECK(other.outcome.status == XR_XIR_CALL_SUSPENDED);
    CHECK(xr_xir_instance_resume(first, suspended.epoch + 1, suspended.outcome.wake) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_resume(first, suspended.epoch, suspended.outcome.wake) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll(first).outcome.value.payload == 30);
    CHECK(xr_xir_instance_state(first) == XR_XIR_INSTANCE_READY);
    XrXirValue value = {0};
    CHECK(xr_xir_instance_take_result(first, &value) == XR_XIR_CALL_RETURNED);
    CHECK(run(first, 3).payload == 32);
    CHECK(xr_xir_instance_resume(second, other.epoch, other.outcome.wake) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll(second).outcome.value.payload == 30);
    const uint32_t expected[] = {2, 20, 12, 1, 21, 11, 0, 22, 10};
    CHECK(a.count == 9 && !memcmp(a.events, expected, sizeof(expected)));
    CHECK(b.count == 9 && !memcmp(b.events, expected, sizeof(expected)));
    for (uint32_t i = 0; i < 3; ++i) CHECK(f.witness.begins[i] == 2);
    XrXirValue string = run(first, 6), atomic = run(first, 9);
    CHECK(xr_xir_instance_free(first) == XR_XIR_CALL_READY);
    CHECK(!f.witness.releases);
    CHECK(xr_xir_instance_free(second) == XR_XIR_CALL_READY);
    CHECK(f.witness.releases == 1);
    CHECK(a.count == 12 && a.events[9] == 32 && a.events[10] == 31 && a.events[11] == 30);
    strings_equal(&string, "A\0\xe4\xb8\xad", 5);
    int64_t old = 0;
    CHECK(xr_xir_atomic_i64_fetch_add(&atomic, 3, &old) && old == 12);
    xr_xir_value_drop(&string); xr_xir_value_drop(&atomic);
}
static void borrowed_restart(void) {
    Fixture f; fixture(&f, 0);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_program_seal(&f.spec, (XrXirProgramBudget) {2097152, 16000000}, &program) == XR_XIR_OK);
    xr_xir_artifact_free(f.proof); f.proof = NULL;
    Trace log = {0}; XrXirInstance *instance = new_instance(program, &log);
    CHECK(xr_xir_instance_start(instance, 8, NULL, 0) == XR_XIR_CALL_READY);
    XrXirInstanceResult old = xr_xir_instance_poll(instance);
    CHECK(old.outcome.status == XR_XIR_CALL_SUSPENDED);
    CHECK(xr_xir_instance_resume(instance, old.epoch, old.outcome.wake) == XR_XIR_CALL_READY);
    XrXirInstanceResult borrowed = xr_xir_instance_poll(instance);
    CHECK(borrowed.outcome.status == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_start(instance, 7, &borrowed.outcome.value, 1) == XR_XIR_CALL_READY);
    XrXirInstanceResult echoed = xr_xir_instance_poll(instance);
    CHECK(echoed.outcome.status == XR_XIR_CALL_RETURNED);
    strings_equal(&echoed.outcome.value, "independent", 11);
    XrXirValue result = {0};
    CHECK(xr_xir_instance_take_result(instance, &result) == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_start(instance, 8, NULL, 0) == XR_XIR_CALL_READY);
    XrXirInstanceResult fresh = xr_xir_instance_poll(instance);
    CHECK(fresh.outcome.status == XR_XIR_CALL_SUSPENDED && fresh.epoch != old.epoch);
    CHECK(fresh.outcome.wake == old.outcome.wake);
    CHECK(xr_xir_instance_resume(instance, old.epoch, old.outcome.wake) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_stop(instance) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll(instance).outcome.status == XR_XIR_CALL_CANCELLED);
    CHECK(xr_xir_instance_start(instance, 3, NULL, 0) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    xr_xir_program_drop(program);
    strings_equal(&result, "independent", 11); xr_xir_value_drop(&result);
}
static void failed_initialization(void) {
    for (uint32_t mode = 1; mode <= 4; ++mode) {
        Fixture f; fixture(&f, mode);
        XrXirProgram *program = NULL;
        CHECK(xr_xir_program_seal(&f.spec, (XrXirProgramBudget) {2097152, 16000000}, &program) == XR_XIR_OK);
    xr_xir_artifact_free(f.proof); f.proof = NULL;
        Trace log = {0}; XrXirInstance *instance = new_instance(program, &log);
        CHECK(xr_xir_instance_start(instance, 3, NULL, 0) == XR_XIR_CALL_READY);
        XrXirInstanceResult result = xr_xir_instance_poll(instance);
        if (mode == 1) CHECK(xr_xir_instance_stop(instance) == XR_XIR_CALL_READY);
        if (mode == 2) CHECK(xr_xir_instance_resume(instance, result.epoch, result.outcome.wake) == XR_XIR_CALL_READY);
        result = xr_xir_instance_poll(instance);
        XrXirCallStatus expected = mode == 1 ? XR_XIR_CALL_CANCELLED : mode == 2 ? XR_XIR_CALL_THROWN :
            mode == 4 ? XR_XIR_CALL_BOUNDS : XR_XIR_CALL_BAD_STATE;
        CHECK(result.outcome.status == expected);
        CHECK(log.count == (mode == 3 ? 1u : 3u));
        if (mode != 3) CHECK(log.events[2] == 30);
        XrXirDomain *reader=NULL; CHECK(xr_xir_domain_new(65536,&reader)==XR_XIR_VALUE_OK);
        XrXirCallResult escaped = {0};
        for (uint32_t repeat = 0; repeat < 3; ++repeat) {
            CHECK(xr_xir_instance_start(instance, 3, NULL, 0) == expected);
            XrXirCallResult copy = {0};
            CHECK(xr_xir_instance_copy_failure(instance, &copy) == expected);
            CHECK(copy.status == expected && (mode == 2 ? error_fixture_is_code(&copy.value,reader,91) : copy.value.payload == 0) && !copy.wake);
            if (mode == 4) {
                CHECK(copy.fault.code == 430 && !copy.fault.reserved &&
                    copy.fault.index == INT64_MIN && copy.fault.length == 3);
                escaped = copy;
            } else CHECK(xr_xir_fault_empty(copy.fault));
            xr_xir_value_drop(&copy.value);
            XrXirCallResult repeated = xr_xir_instance_poll(instance).outcome;
            CHECK(repeated.status == expected && !memcmp(&repeated.fault, &copy.fault, sizeof(copy.fault)));
        }
        CHECK(f.witness.begins[2] == 1 && !f.witness.begins[0] && !f.witness.begins[1]);
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
        xr_xir_program_drop(program); CHECK(f.witness.releases == 1);
        if (mode == 4) CHECK(escaped.fault.code == 430 && escaped.fault.index == INT64_MIN && escaped.fault.length == 3);
        xr_xir_domain_drop(reader);
    }
}
static void seal_rejection(void) {
    for (uint32_t invalid = 0; invalid < 27; ++invalid) {
        Fixture f; fixture(&f, 0);
        uint32_t cycle = 0;
        uint8_t identity[32];
        XrXirFunctionLayout layouts[10];
        switch (invalid) {
        case 0: f.spec.abi_version = 0; break;
        case 1: f.spec.target.abi_version = 2; break;
        case 2: f.entries[3].abi_version = 2; break;
        case 3: f.spec.code.release = NULL; break;
        case 4: f.spec.code.owner = NULL; break;
        case 5: f.entries[0].result = XR_XIR_I64; break;
        case 6: f.entries[3].result = XR_XIR_UNIT; break;
        case 7: f.entries[3].resume = NULL; break;
        case 8: f.modules[1].dependencies = &cycle; f.modules[1].dependency_count = 1; break;
        case 9: break;
        case 10: f.spec.abi_version = 6; break;
        case 11: f.spec.target.abi_version = 7; break;
        case 12: f.entries[3].abi_version = 11; break;
        case 13: f.spec.abi_version = 8; break;
        case 14: f.entries[3].abi_version = 13; break;
        case 15:
            f.spec.abi_version = 9;
            f.spec.types = (const XrXirTypes *) (uintptr_t) 1;
            break;
        case 16:
            f.spec.target.abi_version = 9;
            f.spec.types = (const XrXirTypes *) (uintptr_t) 1;
            break;
        case 17:
            f.spec.abi_version = 10;
            f.spec.types = (const XrXirTypes *) (uintptr_t) 1;
            break;
        case 18:
            f.spec.abi_version = 11;
            f.spec.types = (const XrXirTypes *) (uintptr_t) 1;
            f.spec.proof.bytes = (const uint8_t *) (uintptr_t) 1;
            f.spec.proof.layouts = (const XrXirFunctionLayout *) (uintptr_t) 1;
            break;
        case 19: f.spec.proof.bytes = NULL; break;
        case 20:
            memcpy(identity, f.spec.proof.identity, sizeof(identity)); identity[0] ^= 1;
            f.spec.proof.identity = identity; break;
        case 21: --f.spec.proof.length; break;
        case 22:
            memcpy(layouts, f.spec.proof.layouts, sizeof(layouts)); ++layouts[0].frame_bytes;
            f.spec.proof.layouts = layouts; break;
        case 23: f.slots[0].mutable ^= 1; break;
        case 24: f.identities[3].promises = XR_XIR_FUNCTION_NO_SUSPEND; break;
        case 25:
            f.spec.abi_version = 21;
            f.spec.declarations = (const XrXirDeclarations *) (uintptr_t) 1;
            break;
        case 26:
            f.spec.abi_version = 22;
            f.spec.declarations = (const XrXirDeclarations *) (uintptr_t) 1;
            f.spec.types = (const XrXirTypes *) (uintptr_t) 1;
            f.spec.proof.bytes = (const uint8_t *) (uintptr_t) 1;
            break;
        }
        XrXirProgram *program = NULL;
        XrXirStatus status = xr_xir_program_seal(&f.spec, (XrXirProgramBudget) {invalid == 9 ? 1 : 2097152, 16000000}, &program);
        xr_xir_artifact_free(f.proof); f.proof = NULL;
        CHECK(status != XR_XIR_OK);
        if (invalid >= 15 && invalid <= 18) CHECK(status == XR_XIR_BAD_LAYOUT);
        if (invalid >= 19 && invalid < 25) CHECK(status == XR_XIR_BAD_STRUCTURE);
        if (invalid >= 25) CHECK(status == XR_XIR_BAD_LAYOUT);
        CHECK(!program && !f.witness.releases);
    }
}
#include "xir_function_cases.h"
#include "xir_effect_binding_cases.h"
#include "xir_weaken_authority_cases.h"
#include "xir_array_instance_cases.h"
int main(void) {
    effect_binding_cases();
    weaken_authority_cases();
    array_instance_cases();
    CHECK(function_case_run(false) && function_case_run(true));
    isolation(); borrowed_restart(); failed_initialization(); seal_rejection();
    puts("Program leases, deterministic initialization, isolated cells, sticky failure and result lifetime passed");
    return 0;
}
