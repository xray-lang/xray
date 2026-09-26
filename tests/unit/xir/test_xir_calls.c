/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_calls.c - Resumption, native callback state, and release witnesses
 *
 * KEY CONCEPT:
 *   Native insertion sort retains its state while a real Lowered comparator
 *   returns to the host, resumes, throws, or is cancelled.
 */
#include "xir/xxir_call.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_call_fixture.h"

typedef struct Witness {
    const XrXirArtifact *comparator;
    uint32_t mode, comparisons, cleaned, cleanup_ids[16];
    uint64_t recursive_cleanups;
    uintptr_t stack_low, stack_high;
} Witness;
static const uint32_t identities[] = {0, 1, 2};
static const XrXirType types[] = {XR_XIR_I64, XR_XIR_I64, XR_XIR_I64};

static XrXirAction returned(XrXirValue value) {
    return (XrXirAction) {XR_XIR_ACTION_RETURN, 0, NULL, 0, value};
}
static XrXirAction propagate(XrXirCallResult result) {
    return (XrXirAction) {result.status == XR_XIR_CALL_THROWN ? XR_XIR_ACTION_THROW : XR_XIR_ACTION_RETURN,
        0, NULL, 0, result.value};
}
static XrXirAction root_resume(XrXirCallView *view) {
    uint32_t *started = view->state;
    CHECK((uintptr_t) view->state % XR_XIR_CALL_STATE_ALIGNMENT == 0);
    if (!(*started)++)
        return (XrXirAction) {XR_XIR_ACTION_CALL, 1, view->arguments, 3, {0, 0, 0}};
    CHECK(view->inbox.status == XR_XIR_CALL_RETURNED || view->inbox.status == XR_XIR_CALL_THROWN);
    return propagate(view->inbox);
}
typedef struct NativeSortState {
    XrXirValue values[3], arguments[2];
    uint32_t index, cursor;
    bool initialized, waiting;
} NativeSortState;

static XrXirAction native_sort_resume(XrXirCallView *view) {
    NativeSortState *state = view->state;
    if (!state->initialized) {
        memcpy(state->values, view->arguments, sizeof(state->values));
        state->index = state->cursor = 1;
        state->initialized = true;
    }
    if (state->waiting) {
        if (view->inbox.status == XR_XIR_CALL_THROWN)
            return propagate(view->inbox);
        CHECK(view->inbox.status == XR_XIR_CALL_RETURNED && view->inbox.value.type == XR_XIR_BOOL);
        if (view->inbox.value.payload) {
            XrXirValue swap = state->values[state->cursor];
            state->values[state->cursor] = state->values[state->cursor - 1];
            state->values[state->cursor - 1] = swap;
            --state->cursor;
        } else state->cursor = 0;
        state->waiting = false;
    }
    if (!state->cursor)
        state->cursor = ++state->index;
    if (state->index == 3) {
        int64_t encoded = state->values[0].payload * 100 + state->values[1].payload * 10 + state->values[2].payload;
        return returned((XrXirValue) {XR_XIR_I64, 0, encoded});
    }
    state->arguments[0] = state->values[state->cursor];
    state->arguments[1] = state->values[state->cursor - 1];
    state->waiting = true;
    return (XrXirAction) {XR_XIR_ACTION_CALL, 2, state->arguments, 2, {0, 0, 0}};
}

static XrXirAction comparator_resume(XrXirCallView *view) {
    Witness *witness = view->instance;
    uint32_t *resumed = view->state;
    if (!*resumed)
        ++witness->comparisons;
    CHECK(xr_xir_call_poll(view->activation).status == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_call_resume(view->activation, 1) == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_call_free(view->activation) == XR_XIR_CALL_BUSY);
    uint32_t before = (*resumed)++;
    if ((witness->mode == 1 && before == 0) || (witness->mode == 5 && before < 2))
        return (XrXirAction) {XR_XIR_ACTION_SUSPEND, 0, NULL, 0, {0, 0, 0}};
    if (witness->mode == 2)
        return (XrXirAction) {XR_XIR_ACTION_THROW, 0, NULL, 0, {XR_XIR_I64, 0, 91}};
    if (witness->mode == 3)
        CHECK(xr_xir_call_cancel(view->activation) == XR_XIR_CALL_CANCELLED);
    if (witness->mode == 4)
        return returned((XrXirValue) {XR_XIR_I64, 0, 7});
    XrXirRunContext context = {2, 24, 0, 0, 0, 0};
    XrXirValue result;
    CHECK(xr_xir_vm_run(witness->comparator, 0, &context, view->arguments, 2, &result) == XR_XIR_RUN_OK);
    CHECK(context.live_bytes == 0 && context.allocations == context.frees);
    return returned(result);
}

static void cleanup(XrXirCallView *view, XrXirCallStatus reason) {
    Witness *witness = view->instance;
    CHECK(reason != XR_XIR_CALL_READY && reason != XR_XIR_CALL_SUSPENDED);
    CHECK(xr_xir_call_poll(view->activation).status == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_call_free(view->activation) == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_call_cancel(view->activation) == XR_XIR_CALL_BUSY);
    CHECK(witness->cleaned < 16);
    witness->cleanup_ids[witness->cleaned++] = *(const uint32_t *) view->environment;
}

static XrXirArtifact *comparator_artifact(void) {
    XrXirInstruction instructions[] = {
        {XR_XIR_LT_I64, XR_XIR_BOOL, {0, 1}, {0, 0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {2, 0}, {0, 0}, 0}
    };
    XrXirBlock block = {0, 2};
    XrXirFunction function = {"compare", 7, types, 2, XR_XIR_BOOL, &block, 1, instructions, 2, NULL, 0};
    XrXirModule module = {XR_XIR_BUILT, &function, 1, NULL, NULL, NULL};
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    XrXirArtifact *checked = NULL, *lowered = NULL;
    CHECK(xr_xir_check(&module, NULL, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_lower(checked, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    return lowered;
}

static void callback_cases(void) {
    XrXirArtifact *artifact = comparator_artifact();
    for (uint32_t mode = 0; mode < 9; ++mode) {
        Witness witness = {0};
        witness.comparator = artifact;
        witness.mode = mode >= 6 ? 1 : mode;
        XrXirCallEntry entries[] = {
            {XR_XIR_CALL_ABI_VERSION, types, 3, XR_XIR_I64, sizeof(uint32_t), root_resume, cleanup, &identities[0]},
            {XR_XIR_CALL_ABI_VERSION, types, 3, XR_XIR_I64, sizeof(NativeSortState), native_sort_resume, cleanup, &identities[1]},
            {XR_XIR_CALL_ABI_VERSION, types, 2, XR_XIR_BOOL, sizeof(uint32_t), comparator_resume, cleanup, &identities[2]}
        };
        XrXirCallAccounting accounting = {0};
        XrXirCallConfig config = {entries, 3, &witness, 65536, 100, 10, &accounting, {NULL, NULL}};
        XrXirValue args[] = {{XR_XIR_I64, 0, 3}, {XR_XIR_I64, 0, 1}, {XR_XIR_I64, 0, 2}};
        XrXirCall *call = NULL;
        CHECK(xr_xir_call_new(&config, 0, args, 3, &call) == XR_XIR_CALL_READY);
        memset(entries, 0xa5, sizeof(entries));
        memset(args, 0xa5, sizeof(args));
        if (mode == 8) {
            CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
            CHECK(witness.cleaned == 1 && witness.cleanup_ids[0] == 0);
        } else {
            XrXirCallResult result = xr_xir_call_poll(call);
            uint64_t old_wake = 0;
            while (result.status == XR_XIR_CALL_SUSPENDED) {
                CHECK(result.wake > old_wake);
                uint64_t polls = accounting.polls;
                CHECK(xr_xir_call_poll(call).wake == result.wake && accounting.polls == polls);
                CHECK(xr_xir_call_resume(call, old_wake) == XR_XIR_CALL_BAD_STATE);
                if (mode == 6 || mode == 7) break;
                old_wake = result.wake;
                CHECK(xr_xir_call_resume(call, result.wake) == XR_XIR_CALL_READY);
                CHECK(xr_xir_call_resume(call, result.wake) == XR_XIR_CALL_BAD_STATE);
                result = xr_xir_call_poll(call);
            }
            if (mode == 6) {
                CHECK(xr_xir_call_cancel(call) == XR_XIR_CALL_CANCELLED);
                CHECK(xr_xir_call_resume(call, result.wake) == XR_XIR_CALL_BAD_STATE);
                result = xr_xir_call_poll(call);
            }
            if (mode == 2) CHECK(result.status == XR_XIR_CALL_THROWN && result.value.payload == 91);
            else if (mode == 3 || mode == 6) CHECK(result.status == XR_XIR_CALL_CANCELLED);
            else if (mode == 4) CHECK(result.status == XR_XIR_CALL_BAD_STATE);
            else if (mode != 7) CHECK(result.status == XR_XIR_CALL_RETURNED && result.value.payload == 123);
            CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
            CHECK(witness.cleaned >= 3 && witness.cleanup_ids[witness.cleaned - 1] == 0);
            CHECK(witness.cleanup_ids[witness.cleaned - 2] == 1);
            for (uint32_t i = 0; i + 2 < witness.cleaned; ++i) CHECK(witness.cleanup_ids[i] == 2);
            if (mode == 0 || mode == 1 || mode == 5) CHECK(witness.comparisons == 3 && witness.cleaned == 5);
        }
        CHECK(accounting.live_bytes == 0 && accounting.depth == 0 && accounting.allocations == accounting.frees);
    }
    xr_xir_artifact_free(artifact);
}

typedef struct RecursiveState { uint32_t entered; XrXirValue child; } RecursiveState;
static XrXirAction recursive_resume(XrXirCallView *view) {
    Witness *witness = view->instance;
    RecursiveState *state = view->state;
#if defined(__clang__) || defined(__GNUC__)
    uintptr_t probe = (uintptr_t) __builtin_frame_address(0);
#elif defined(_MSC_VER)
    uintptr_t probe = (uintptr_t) _AddressOfReturnAddress();
#else
    uintptr_t probe = (uintptr_t) &state;
#endif
    if (!witness->stack_low || probe < witness->stack_low) witness->stack_low = probe;
    if (probe > witness->stack_high) witness->stack_high = probe;
    if (!state->entered++) {
        if (view->arguments[0].payload == 0) return returned((XrXirValue) {XR_XIR_I64, 0, 0});
        state->child = (XrXirValue) {XR_XIR_I64, 0, view->arguments[0].payload - 1};
        return (XrXirAction) {XR_XIR_ACTION_CALL, 0, &state->child, 1, {0, 0, 0}};
    }
    CHECK(view->inbox.status == XR_XIR_CALL_RETURNED);
    return returned((XrXirValue) {XR_XIR_I64, 0, view->inbox.value.payload + 1});
}
static void recursive_cleanup(XrXirCallView *view, XrXirCallStatus reason) {
    Witness *witness = view->instance;
    if (reason == XR_XIR_CALL_RETURNED)
        CHECK(view->arguments[0].payload == (int64_t) witness->recursive_cleanups);
    ++witness->recursive_cleanups;
}
static void bounded_stack(void) {
    XrXirCallEntry entry = {XR_XIR_CALL_ABI_VERSION, types, 1, XR_XIR_I64, sizeof(RecursiveState), recursive_resume, recursive_cleanup, NULL};
    for (uint32_t variant = 0; variant < 3; ++variant) {
        Witness witness = {0};
        XrXirCallAccounting accounting = {0};
        XrXirCallConfig config = {&entry, 1, &witness, 4 * 1024 * 1024, variant == 2 ? 5 : 30000,
            variant == 1 ? 5 : 10001, &accounting, {NULL, NULL}};
        XrXirValue argument = {XR_XIR_I64, 0, 10000};
        XrXirCall *call = NULL;
        CHECK(xr_xir_call_new(&config, 0, &argument, 1, &call) == XR_XIR_CALL_READY);
        XrXirCallResult result = xr_xir_call_poll(call);
        if (!variant) {
            CHECK(result.status == XR_XIR_CALL_RETURNED && result.value.payload == 10000);
            CHECK(accounting.peak_depth == 10001 && witness.recursive_cleanups == 10001);
            CHECK(witness.stack_high - witness.stack_low < 4096);
        } else CHECK(result.status == XR_XIR_CALL_LIMIT);
        CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
        CHECK(accounting.live_bytes == 0 && accounting.allocations == accounting.frees);
    }
}
XR_DATA const XrXirCallEntry fixture_calls0_entries[3];
XR_DATA const XrXirCallEntry fixture_calls1_entries[3];
XR_DATA const XrXirCallEntry fixture_calls2_entries[3];
static void xir_instruction_calls(void) {
    for (uint32_t mode = 0; mode < 3; ++mode) for (uint32_t native = 0; native < 2; ++native) {
        XrXirArtifact *artifact = call_fixture(mode);
        XrXirVmBinding bindings[3];
        XrXirCallEntry entries[3];
        for (uint32_t i = 0; i < 3; ++i)
            CHECK(xr_xir_vm_bind(artifact, i, &bindings[i], &entries[i]) == XR_XIR_OK);
        if (native) {
            const XrXirCallEntry *tables[] = {fixture_calls0_entries, fixture_calls1_entries, fixture_calls2_entries};
            entries[1] = tables[mode][1];
        }
        XrXirCallAccounting accounting = {0};
        XrXirCallConfig config = {entries, 3, NULL, 65536, 100, 10, &accounting, {NULL, NULL}};
        XrXirValue args[] = {{XR_XIR_I64, 0, 9}, {XR_XIR_I64, 0, 4}};
        XrXirCall *call = NULL;
        CHECK(xr_xir_call_new(&config, 0, args, 2, &call) == XR_XIR_CALL_READY);
        XrXirCallResult result = xr_xir_call_poll(call);
        if (mode != 2) {
            CHECK(result.status == XR_XIR_CALL_SUSPENDED && accounting.depth == 3);
            CHECK(xr_xir_call_resume(call, result.wake) == XR_XIR_CALL_READY);
            result = xr_xir_call_poll(call);
        }
        if (mode == 0) CHECK(result.status == XR_XIR_CALL_RETURNED && result.value.payload == 4);
        if (mode == 1) CHECK(result.status == XR_XIR_CALL_THROWN && result.value.payload == 91);
        if (mode == 2) CHECK(result.status == XR_XIR_CALL_DIVIDE_BY_ZERO && result.value.type == XR_XIR_UNIT);
        CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
        CHECK(accounting.live_bytes == 0 && accounting.allocations == accounting.frees);
        xr_xir_artifact_free(artifact);
    }
}
static XrXirAction wrong_comparator(XrXirCallView *view) {
    (void) view;
    return returned((XrXirValue) {XR_XIR_I64, 0, 2});
}
static void call_admission(void) {
    XrXirArtifact *artifact = call_fixture(0);
    const XrXirModule *module = xr_xir_artifact_module(artifact);
    XrXirInstruction *op = (XrXirInstruction *) module->functions[0].instructions;
    op[0].immediate = -1;
    CHECK(xr_xir_artifact_verify(artifact, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    op[0].immediate = 3;
    CHECK(xr_xir_artifact_verify(artifact, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    op[0].immediate = 1;
    op[0].type = XR_XIR_BOOL;
    CHECK(xr_xir_artifact_verify(artifact, NULL, NULL) == XR_XIR_BAD_TYPE);
    op[0].type = XR_XIR_I64;
    op[0].args[0] = 99;
    CHECK(xr_xir_artifact_verify(artifact, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    op[0].args[0] = 0;
    uint32_t *operands = (uint32_t *) module->functions[0].operands;
    operands[0] = 99;
    CHECK(xr_xir_artifact_verify(artifact, NULL, NULL) == XR_XIR_BAD_VALUE);
    operands[0] = 0;
    CHECK(xr_xir_artifact_verify(artifact, NULL, NULL) == XR_XIR_OK);
    XrXirVmBinding bindings[3];
    XrXirCallEntry entries[3];
    for (uint32_t i = 0; i < 3; ++i)
        CHECK(xr_xir_vm_bind(artifact, i, &bindings[i], &entries[i]) == XR_XIR_OK);
    XrXirCallAccounting accounting = {0};
    XrXirCallConfig config = {entries, 3, NULL, 65536, 100, 10, &accounting, {NULL, NULL}};
    XrXirValue arguments[] = {{XR_XIR_I64, 0, 9}, {XR_XIR_I64, 0, 4}};
    XrXirCall *call = NULL;
    const uint32_t rejected_abis[] = {0, 1, 2, 3, 4, 5, 6, XR_XIR_CALL_ABI_VERSION + 1};
    for (size_t i = 0; i < sizeof(rejected_abis) / sizeof(rejected_abis[0]); ++i) {
        entries[1].abi_version = rejected_abis[i];
        CHECK(xr_xir_call_new(&config, 0, arguments, 2, &call) == XR_XIR_CALL_BAD_ABI);
        CHECK(!call && accounting.live_bytes == 0);
    }
    entries[1].abi_version = XR_XIR_CALL_ABI_VERSION;
    config.byte_limit = 1;
    CHECK(xr_xir_call_new(&config, 0, arguments, 2, &call) == XR_XIR_CALL_LIMIT);
    config.byte_limit = 65536;
    config.depth_limit = 0;
    CHECK(xr_xir_call_new(&config, 0, arguments, 2, &call) == XR_XIR_CALL_LIMIT);
    CHECK(!call && accounting.live_bytes == 0 && accounting.allocations == accounting.frees);
    config.depth_limit = 10;
    arguments[0].reserved = 1;
    CHECK(xr_xir_call_new(&config, 0, arguments, 2, &call) == XR_XIR_CALL_BAD_ARGUMENT);
    arguments[0].reserved = 0;
    entries[2].result = XR_XIR_I64;
    entries[2].resume = wrong_comparator;
    for (uint32_t native = 0; native < 2; ++native) {
        if (native) entries[1] = fixture_calls0_entries[1];
        CHECK(xr_xir_call_new(&config, 0, arguments, 2, &call) == XR_XIR_CALL_READY);
        CHECK(xr_xir_call_poll(call).status == XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
        CHECK(accounting.live_bytes == 0 && accounting.allocations == accounting.frees);
    }
    xr_xir_artifact_free(artifact);
}
int main(void) {
    callback_cases();
    bounded_stack();
    xir_instruction_calls();
    call_admission();
    puts("Resumable native-to-Lowered-VM comparator and bounded trampoline witnesses passed");
    return 0;
}
