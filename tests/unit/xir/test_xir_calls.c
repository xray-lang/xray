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
#include "xir/xxir_program_internal.h"
#include "xir/xxir_compile_memory.h"
#include "xir_mixed_binding_projection.h"
#include "xir_construction_fixture.h"
#include "xir/xxir_call.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
static uint32_t call_factory_abi=XR_XIR_PROGRAM_ABI_VERSION;
static unsigned call_binding_attack,call_binding_callbacks;
static unsigned call_code_releases;
static XrXirVmBinding *call_attack_bindings;
static void call_before_compile_allocation(size_t bytes) {
    (void)bytes;
    if(!call_attack_bindings)return;
    ++call_binding_callbacks;
    if(call_binding_attack==1)call_attack_bindings[0].artifact=(const XrXirArtifact *)(uintptr_t)1;
    else call_attack_bindings[0].function=1;
}
#define XR_INSTANCE_COMPILE_BEFORE_ALLOCATION(bytes) call_before_compile_allocation(bytes)
#include "xir_instance_compile_observer.h"
#undef XR_INSTANCE_COMPILE_BEFORE_ALLOCATION

static void call_runtime_retiring(void *pointer);
#define XIR_RUNTIME_BEFORE_FREE(pointer) call_runtime_retiring(pointer)
#include "xir_runtime_allocations.h"
#undef XIR_RUNTIME_BEFORE_FREE
#include "xir_call_fixture.h"
#include "xir_call_instance_owner.h"

typedef struct Witness {
    const XrXirArtifact *comparator;
    XrXirValue error;
    uint32_t mode, comparisons, cleaned, cleanup_ids[16];
    uint64_t recursive_cleanups;
    uintptr_t stack_low, stack_high;
} Witness;
static const uint32_t identities[] = {0, 1, 2};
static const XrXirType types[] = {XR_XIR_I64, XR_XIR_I64, XR_XIR_I64};

static XrXirAction returned(XrXirValue value) {
    return (XrXirAction) {XR_XIR_ACTION_RETURN, 0, NULL, 0, value, {0}, 0};
}
static XrXirAction propagate(XrXirCallResult result) {
    return (XrXirAction) {result.status == XR_XIR_CALL_THROWN ? XR_XIR_ACTION_THROW : XR_XIR_ACTION_RETURN,
        0, NULL, 0, result.value, {0}, 0};
}
static XrXirAction root_resume(XrXirCallView *view) {
    uint32_t *started = view->state;
    CHECK((uintptr_t) view->state % XR_XIR_CALL_STATE_ALIGNMENT == 0);
    if (!(*started)++)
        return (XrXirAction) {XR_XIR_ACTION_CALL, 1, view->arguments, 3, {0, 0, 0}, {0}, 0};
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
    return (XrXirAction) {XR_XIR_ACTION_CALL, 2, state->arguments, 2, {0, 0, 0}, {0}, 0};
}

static XrXirAction comparator_resume(XrXirCallView *view) {
    Witness *witness = view->instance;
    uint32_t *resumed = view->state;
    if (!*resumed)
        ++witness->comparisons;
    CHECK(xr_xir_call_poll_bounded(view->activation, UINT64_MAX).status == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_call_resume(view->activation, 1) == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_call_free(view->activation) == XR_XIR_CALL_BUSY);
    uint32_t before = (*resumed)++;
    if ((witness->mode == 1 && before == 0) || (witness->mode == 5 && before < 2))
        return (XrXirAction) {XR_XIR_ACTION_SUSPEND, 0, NULL, 0, {0, 0, 0}, {0}, 0};
    if (witness->mode == 2)
        return (XrXirAction) {XR_XIR_ACTION_THROW, 0, NULL, 0, witness->error, {0}, 0};
    if (witness->mode == 9)
        return (XrXirAction) {XR_XIR_ACTION_THROW,0,NULL,0,{XR_XIR_I64,0,91},{0}, 0};
    if (witness->mode == 10) {
        XrXirValue invalid=witness->error; invalid.reserved=1;
        return (XrXirAction) {XR_XIR_ACTION_THROW,0,NULL,0,invalid,{0}, 0};
    }
    if (witness->mode == 3)
        CHECK(xr_xir_call_request_cancel(view->activation) == XR_XIR_CALL_CANCEL_REQUESTED);
    if (witness->mode == 4)
        return returned((XrXirValue) {XR_XIR_I64, 0, 7});
    XrXirRunContext context = {2, 24, 0, 0, 0, 0};
    XrXirValue result;
    CHECK(xr_xir_compile_vm_run(witness->comparator, 0, &context, view->arguments, 2, &result) == XR_XIR_RUN_OK);
    CHECK(context.live_bytes == 0 && context.allocations == context.frees);
    return returned(result);
}

static void cleanup(XrXirCallView *view, XrXirCallStatus reason) {
    Witness *witness = view->instance;
    CHECK(reason != XR_XIR_CALL_READY && reason != XR_XIR_CALL_SUSPENDED);
    CHECK(xr_xir_call_poll_bounded(view->activation, UINT64_MAX).status == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_call_free(view->activation) == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_call_request_cancel(view->activation) == XR_XIR_CALL_BUSY);
    CHECK(witness->cleaned < 16);
    witness->cleanup_ids[witness->cleaned++] = *(const uint32_t *) view->environment;
}

static XrXirArtifact *comparator_artifact(const XrXirCompileContext *context) {
    XrXirInstruction instructions[] = {
        {XR_XIR_LT_INT, XR_XIR_BOOL, {0, 1}, {0, 0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {2, 0}, {0, 0}, 0, {0}}
    };
    XrXirBlock block = {0, 2, 0, 0};
    XrXirFunction function = {"compare", 7, types, 2, XR_XIR_BOOL, &block, 1, instructions, 2, NULL, 0};
    XrXirModule module = {XR_XIR_BUILT, &function, 1, NULL, NULL, NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    XrXirArtifact *checked = NULL, *lowered = NULL;
    CHECK(xir_fixture_check(context, &module, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_lower(checked, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    return lowered;
}

static void callback_cases(const XrXirCompileContext *context) {
    XrXirArtifact *artifact = comparator_artifact(context);
    for (uint32_t mode = 0; mode < 11; ++mode) {
        Witness witness = {0};
        witness.comparator = artifact;
        witness.mode = mode >= 6 && mode <= 8 ? 1 : mode;
        XrXirCallEntry entries[] = {
            {XR_XIR_CALL_ABI_VERSION, types, 3, XR_XIR_I64, sizeof(uint32_t), root_resume, cleanup, &identities[0], 0, 0},
            {XR_XIR_CALL_ABI_VERSION, types, 3, XR_XIR_I64, sizeof(NativeSortState), native_sort_resume, cleanup, &identities[1], 0, 0},
            {XR_XIR_CALL_ABI_VERSION, types, 2, XR_XIR_BOOL, sizeof(uint32_t), comparator_resume, cleanup, &identities[2], 0, 0}
        };
        XrXirDomain *domain=NULL; CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
        XrXirTypeArena *arena=NULL;
        CHECK(error_fixture_arena(context, &arena)==XR_XIR_VALUE_OK);
        XrXirCallAccounting accounting = {0};
        XrXirCallConfig config; CHECK(xr_xir_call_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY); config.entries = entries; config.entry_count = 3; config.instance = &witness; config.byte_limit = 65536; config.poll_limit = 100; config.depth_limit = 10; config.accounting = &accounting; config.output = (XrXirOutputProvider) {0}; config.admission = (XrXirValueAdmission) {0};
        config.admission=error_fixture_admission(domain,arena);
        witness.error=error_fixture_code(&config.admission,91);
        XrXirValue args[] = {{XR_XIR_I64, 0, 3}, {XR_XIR_I64, 0, 1}, {XR_XIR_I64, 0, 2}};
        XrXirCall *call = NULL;
        CHECK(xr_xir_call_new(&config, 0, args, 3, &call) == XR_XIR_CALL_READY);
        memset(entries, 0xa5, sizeof(entries));
        memset(args, 0xa5, sizeof(args));
        if (mode == 8) {
            CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
            CHECK(witness.cleaned == 1 && witness.cleanup_ids[0] == 0);
        } else {
            XrXirCallResult result = xr_xir_call_poll_bounded(call, UINT64_MAX);
            uint64_t old_wake = 0;
            while (result.status == XR_XIR_CALL_SUSPENDED) {
                CHECK(result.wake > old_wake);
                uint64_t polls = accounting.polls;
                CHECK(xr_xir_call_poll_bounded(call, UINT64_MAX).wake == result.wake && accounting.polls == polls);
                CHECK(xr_xir_call_resume(call, old_wake) == XR_XIR_CALL_BAD_STATE);
                if (mode == 6 || mode == 7) break;
                old_wake = result.wake;
                CHECK(xr_xir_call_resume(call, result.wake) == XR_XIR_CALL_READY);
                CHECK(xr_xir_call_resume(call, result.wake) == XR_XIR_CALL_BAD_STATE);
                result = xr_xir_call_poll_bounded(call, UINT64_MAX);
            }
            if (mode == 6) {
                { CHECK(xr_xir_call_request_cancel(call) == XR_XIR_CALL_CANCEL_REQUESTED); CHECK(xr_xir_call_poll_bounded(call, UINT64_MAX).status == XR_XIR_CALL_CANCELLED); }
                CHECK(xr_xir_call_resume(call, result.wake) == XR_XIR_CALL_BAD_STATE);
                result = xr_xir_call_poll_bounded(call, UINT64_MAX);
            }
            if (mode == 2) CHECK(result.status == XR_XIR_CALL_THROWN && error_fixture_is_code(&result.value,domain,91));
            else if (mode == 3 || mode == 6) CHECK(result.status == XR_XIR_CALL_CANCELLED);
            else if (mode == 4 || mode == 9 || mode == 10) CHECK(result.status == XR_XIR_CALL_BAD_STATE);
            else if (mode != 7) CHECK(result.status == XR_XIR_CALL_RETURNED && result.value.payload == 123);
            CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
            CHECK(witness.cleaned >= 3 && witness.cleanup_ids[witness.cleaned - 1] == 0);
            CHECK(witness.cleanup_ids[witness.cleaned - 2] == 1);
            for (uint32_t i = 0; i + 2 < witness.cleaned; ++i) CHECK(witness.cleanup_ids[i] == 2);
            if (mode == 0 || mode == 1 || mode == 5) CHECK(witness.comparisons == 3 && witness.cleaned == 5);
        }
        CHECK(accounting.live_bytes == 0 && accounting.depth == 0 && accounting.allocations == accounting.frees);
        xr_xir_value_drop(&witness.error); xr_xir_compile_type_arena_drop(arena); xr_xir_domain_drop(domain);
    }
    xr_xir_compile_artifact_free(artifact);
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
        return (XrXirAction) {XR_XIR_ACTION_CALL, 0, &state->child, 1, {0, 0, 0}, {0}, 0};
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
    XrXirCallEntry entry = {XR_XIR_CALL_ABI_VERSION, types, 1, XR_XIR_I64, sizeof(RecursiveState), recursive_resume, recursive_cleanup, NULL, 0, 0};
    for (uint32_t variant = 0; variant < 3; ++variant) {
        Witness witness = {0};
        XrXirCallAccounting accounting = {0};
        XrXirCallConfig config; CHECK(xr_xir_call_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY); config.entries = &entry; config.entry_count = 1; config.instance = &witness; config.byte_limit = 4 * 1024 * 1024; config.poll_limit = variant == 2 ? 5 : 30000; config.depth_limit = variant == 1 ? 5 : 10001; config.accounting = &accounting; config.output = (XrXirOutputProvider) {0}; config.admission = (XrXirValueAdmission) {0};
        XrXirValue argument = {XR_XIR_I64, 0, 10000};
        XrXirCall *call = NULL;
        CHECK(xr_xir_call_new(&config, 0, &argument, 1, &call) == XR_XIR_CALL_READY);
        XrXirCallResult result = xr_xir_call_poll_bounded(call, UINT64_MAX);
        if (!variant) {
            CHECK(result.status == XR_XIR_CALL_RETURNED && result.value.payload == 10000);
            CHECK(accounting.peak_depth == 10001 && witness.recursive_cleanups == 10001);
            CHECK(witness.stack_high - witness.stack_low < 4096);
        } else CHECK(result.status == XR_XIR_CALL_LIMIT);
        CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
        CHECK(accounting.live_bytes == 0 && accounting.allocations == accounting.frees);
    }
}
XR_DATA const XrXirCallEntry fixture_calls0_entries[5];
XR_DATA const XrXirCallEntry fixture_calls1_entries[5];
XR_DATA const XrXirCallEntry fixture_calls2_entries[5];
XR_DATA const XrXirCallEntry fixture_calls3_entries[5];
XR_DATA const XrXirCallEntry fixture_calls4_entries[5];
XR_DATA const XrXirCallEntry fixture_calls5_entries[5];
XR_DATA const XrXirCallEntry fixture_calls6_entries[5];
XR_DATA const XrXirCallEntry fixture_calls7_entries[5];
XR_DATA const XrXirCallEntry fixture_calls8_entries[5];
XR_DATA const XrXirCallEntry fixture_calls9_entries[5];
XR_DATA const XrXirCallEntry fixture_calls10_entries[5];
XR_DATA const XrXirCallEntry fixture_calls11_entries[5];
XR_DATA const XrXirCallEntry fixture_calls12_entries[5];
XR_DATA const XrXirCallEntry fixture_calls13_entries[5];

typedef struct CallFixtureCode {
    XrXirVmBinding *bindings;
    XrXirCallEntry *entries;
} CallFixtureCode;
static void call_fixture_code_drop(void *pointer) {
    CallFixtureCode *code=pointer;if(!code)return;
    xr_compile_resources_free(code->bindings);xr_compile_resources_free(code->entries);
    xr_compile_resources_free(code);
}
static void call_fixture_code_release(void *pointer) {
    ++call_code_releases;call_fixture_code_drop(pointer);
}
static XrXirStatus call_fixture_program_take(const XrXirCompileContext *context,
    XrXirArtifact **artifact,const XrXirCallEntry *native,uint32_t mode,
    XrXirResumeEntry invalid_result,XrXirProgram **output) {
    CHECK(artifact && *artifact && output && !*output);
    XrXirCompileContext current=*context;context=&current;
    XrXirArtifact *original=*artifact;
    const XrXirModule *module=xr_xir_compile_artifact_module(original);
    CHECK(module && module->stage==XR_XIR_LOWERED && module->function_count==5 && module->declarations);
    XrXirStatus status=XR_XIR_OK;
    CallFixtureCode *code=xir_compile_calloc(context,1,sizeof(*code),&status);
    if(!code)return status;
    XirMixedProjection projection;
    XrXirProgramInput *input=NULL;XrXirProgram *program=NULL;
    status=xr_xir_compile_vm_bind_table(original,&code->bindings,&code->entries);
    if(status==XR_XIR_OK)status=xir_mixed_projection_prepare(context,original,code->bindings,code->entries,5,&projection);
    if(status==XR_XIR_OK && native){
        uint32_t callee=mode>=12?4u:1u;
        status=xir_mixed_projection_native(context,&projection,callee,&native[callee]);
        if(status==XR_XIR_OK)code->entries[callee]=native[callee];
    }
    if(status==XR_XIR_OK && invalid_result){
        CHECK(code->entries[2].result==XR_XIR_BOOL);
        code->entries[2].resume=invalid_result;
        status=xir_mixed_projection_native(context,&projection,2,&code->entries[2]);
    }
    if(status==XR_XIR_OK){
        XrXirProgramSpec spec={call_factory_abi,*xr_xir_compile_artifact_target(original),
            code->entries,5,module->declarations,{0},module->types,{0}};
        XrXirArtifact *owned=original;
        call_attack_bindings=call_binding_attack?code->bindings:NULL;
        status=xir_program_input_take(context,&owned,&input);
        if(status==XR_XIR_OK)status=xir_program_receive(context,&spec,&input,NULL,&program);
        call_attack_bindings=NULL;
    }
    if(status==XR_XIR_OK && (*output || *artifact!=original))status=XR_XIR_BAD_STRUCTURE;
    if(status==XR_XIR_OK)status=xir_mixed_projection_match(context,program,&projection);
    if(status!=XR_XIR_OK){
        if(program){program->permissions->input->artifact=NULL;xr_xir_compile_program_drop(program);}
        else if(input){input->artifact=NULL;xir_program_input_free(input);}
        call_fixture_code_drop(code);return status;
    }
    program->code=(XrXirCodeLease){code,call_fixture_code_release};
    *artifact=NULL;*output=program;return XR_XIR_OK;
}

static void xir_instruction_calls(const XrXirCompileContext *context) {
    for (uint32_t mode=0;mode<14;++mode) for (uint32_t native=0;native<2;++native) {
        uint32_t kind=mode>=12?mode-12:mode>=10?mode-10:mode%4;
        const XrXirCallEntry *tables[]={fixture_calls0_entries,fixture_calls1_entries,fixture_calls2_entries,
            fixture_calls3_entries,fixture_calls4_entries,fixture_calls5_entries,fixture_calls6_entries,
            fixture_calls7_entries,fixture_calls8_entries,fixture_calls9_entries,fixture_calls10_entries,
            fixture_calls11_entries,fixture_calls12_entries,fixture_calls13_entries};
        XrXirArtifact *artifact=call_fixture(context,mode);
        XrXirProgram *program=NULL;
        CHECK(call_fixture_program_take(context,&artifact,native?tables[mode]:NULL,mode,NULL,&program)==XR_XIR_OK && !artifact && program);
        XrXirInstance *instance=call_instance_new(program);
        xr_xir_compile_program_drop(program);program=NULL;
        XrXirValue args[]={{XR_XIR_I64,0,9},{XR_XIR_I64,0,4}};
        CHECK(xr_xir_instance_start(instance,0,args,2)==XR_XIR_CALL_READY);
        XrXirInstanceResult polled=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
        XrXirCallResult result=polled.outcome;
        const XrXirCallAccounting *accounting=call_instance_accounting(instance);
        if (kind!=2) {
            if (result.status!=XR_XIR_CALL_SUSPENDED || call_instance_language_depth(instance)!=(mode>=12?2u:3u))
                fprintf(stderr,"Instruction call mode=%u native=%u status=%u depth=%u\n",
                    mode,native,result.status,accounting->depth);
            CHECK(result.status==XR_XIR_CALL_SUSPENDED && call_instance_language_depth(instance)==(mode>=12?2u:3u));
            CHECK(xr_xir_instance_resume(instance,polled.epoch,result.wake)==XR_XIR_CALL_READY);
            polled=xr_xir_instance_poll_bounded(instance,UINT64_MAX);result=polled.outcome;
        }
        if (mode==9) {
            CHECK(result.status==XR_XIR_CALL_SUSPENDED && call_instance_language_depth(instance)==1);
            CHECK(xr_xir_instance_resume(instance,polled.epoch,result.wake)==XR_XIR_CALL_READY);
            polled=xr_xir_instance_poll_bounded(instance,UINT64_MAX);result=polled.outcome;
        }
        if (mode==0 || mode==4 || mode==8 || mode==12) CHECK(result.status==XR_XIR_CALL_RETURNED && result.value.payload==4);
        if (mode==10) CHECK(result.status==XR_XIR_CALL_RETURNED && result.value.type==XR_XIR_I64 && result.value.payload==44);
        if (mode==5 || mode==11 || mode==13) CHECK(result.status==XR_XIR_CALL_RETURNED && result.value.type==XR_XIR_I64 && result.value.payload==77);
        if (mode==1 || mode==9) CHECK(result.status==XR_XIR_CALL_THROWN && error_fixture_is_code(&result.value,instance->domain,91));
        if (kind==2) CHECK(result.status==XR_XIR_CALL_DIVIDE_BY_ZERO && result.value.type==XR_XIR_UNIT);
        if (kind==3) CHECK(result.status==XR_XIR_CALL_MATCH_FAILURE && result.value.type==XR_XIR_UNIT && xr_xir_fault_match_valid(result.panic.detail));
        call_instance_free(instance,XR_XIR_CALL_READY);
        CHECK(!runtime_live && !runtime_bytes);
    }
}

static XrXirAction wrong_comparator(XrXirCallView *view) {
    (void) view;
    return returned((XrXirValue) {XR_XIR_I64, 0, 2});
}
static void call_admission(const XrXirCompileContext *context) {
    XrXirArtifact *artifact = call_fixture(context, 0);
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    XrXirInstruction *throw_op=(XrXirInstruction *)&module->functions[2].instructions[3];
    XrXirInstruction saved=*throw_op;
    *throw_op=(XrXirInstruction){XR_XIR_THROW,XR_XIR_UNIT,{4},{0},0, {0}};
    CHECK(xr_xir_compile_artifact_verify(artifact,NULL)==XR_XIR_BAD_TYPE);
    *throw_op=saved;
    XrXirInstruction *op = (XrXirInstruction *) module->functions[0].instructions;
    op[0].immediate = -1;
    CHECK(xr_xir_compile_artifact_verify(artifact, NULL) == XR_XIR_BAD_STRUCTURE);
    op[0].immediate = 3;
    CHECK(xr_xir_compile_artifact_verify(artifact, NULL) == XR_XIR_BAD_STRUCTURE);
    op[0].immediate = 1;
    op[0].type = XR_XIR_BOOL;
    CHECK(xr_xir_compile_artifact_verify(artifact, NULL) == XR_XIR_BAD_TYPE);
    op[0].type = XR_XIR_I64;
    op[0].args[0] = 99;
    CHECK(xr_xir_compile_artifact_verify(artifact, NULL) == XR_XIR_BAD_STRUCTURE);
    op[0].args[0] = 0;
    uint32_t *operands = (uint32_t *) module->functions[0].operands;
    operands[0] = 99;
    CHECK(xr_xir_compile_artifact_verify(artifact, NULL) == XR_XIR_BAD_VALUE);
    operands[0] = 0;
    CHECK(xr_xir_compile_artifact_verify(artifact, NULL) == XR_XIR_OK);
    XrXirVmBinding bindings[3];
    XrXirCallEntry entries[3];
    for (uint32_t i = 0; i < 3; ++i)
        CHECK(xr_xir_compile_vm_bind(artifact, i, &bindings[i], &entries[i]) == XR_XIR_OK);
    XrXirCallAccounting accounting = {0};
    XrXirCallConfig config; CHECK(xr_xir_call_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY); config.entries = entries; config.entry_count = 3; config.instance = NULL; config.byte_limit = 65536; config.poll_limit = 100; config.depth_limit = 10; config.accounting = &accounting; config.output = (XrXirOutputProvider) {0}; config.admission = (XrXirValueAdmission) {0};
    XrXirValue arguments[] = {{XR_XIR_I64, 0, 9}, {XR_XIR_I64, 0, 4}};
    XrXirCall *call = NULL;
    const uint32_t rejected_abis[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, XR_XIR_CALL_ABI_VERSION + 1};
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
        XrXirArtifact *invalid=call_fixture(context,0);
        XrXirProgram *program=NULL;
        CHECK(call_fixture_program_take(context,&invalid,native?fixture_calls0_entries:NULL,0,wrong_comparator,&program)==XR_XIR_OK && !invalid && program);
        XrXirInstance *instance=call_instance_new(program);
        xr_xir_compile_program_drop(program);
        CHECK(xr_xir_instance_start(instance,0,arguments,2)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_BAD_STATE);
        /* An accepted malformed return remains the executor's exact failure. */
        call_instance_free(instance,XR_XIR_CALL_BAD_STATE);
        CHECK(accounting.live_bytes == 0 && accounting.allocations == accounting.frees);
    }
    xr_xir_compile_artifact_free(artifact);
}
typedef struct FaultWitness {
    XrXirRunStatus failure;
    XrXirCallStatus reason;
    uint32_t cleanups;
} FaultWitness;

static XrXirAction fault_parent(XrXirCallView *view) {
    (void) view;
    return (XrXirAction) {XR_XIR_ACTION_CALL, 1, NULL, 0, {XR_XIR_UNIT, 0, 0}, {0}, 0};
}

static XrXirAction fault_child(XrXirCallView *view) {
    FaultWitness *witness = view->instance;
    return xr_xir_call_fault(witness->failure);
}

static void fault_cleanup(XrXirCallView *view, XrXirCallStatus reason) {
    FaultWitness *witness = view->instance;
    const uint32_t *identity = view->environment;
    CHECK(reason == witness->reason);
    CHECK(*identity == 1u - witness->cleanups);
    ++witness->cleanups;
}

static void fault_boundary(void) {
    const struct {
        XrXirRunStatus failure;
        XrXirCallStatus reason;
    } cases[] = {
        {XR_XIR_RUN_DIVIDE_BY_ZERO, XR_XIR_CALL_DIVIDE_BY_ZERO},
        {XR_XIR_RUN_NUMERIC_RANGE, XR_XIR_CALL_NUMERIC_RANGE},
        {XR_XIR_RUN_OUT_OF_MEMORY, XR_XIR_CALL_OOM},
        {XR_XIR_RUN_STEP_LIMIT, XR_XIR_CALL_LIMIT},
        {XR_XIR_RUN_FRAME_LIMIT, XR_XIR_CALL_LIMIT},
        {XR_XIR_RUN_BAD_ARGUMENT, XR_XIR_CALL_BAD_STATE},
        {XR_XIR_RUN_BAD_ARTIFACT, XR_XIR_CALL_BAD_STATE},
        {XR_XIR_RUN_BAD_ABI, XR_XIR_CALL_BAD_STATE},
        {XR_XIR_RUN_OK, XR_XIR_CALL_BAD_STATE},
        {(XrXirRunStatus) -1, XR_XIR_CALL_BAD_STATE},
        {(XrXirRunStatus) 999, XR_XIR_CALL_BAD_STATE},
    };
    const XrXirCallEntry entries[] = {
        {XR_XIR_CALL_ABI_VERSION, NULL, 0, XR_XIR_UNIT, 0, fault_parent, fault_cleanup, &identities[0], 0, 0},
        {XR_XIR_CALL_ABI_VERSION, NULL, 0, XR_XIR_UNIT, 0, fault_child, fault_cleanup, &identities[1], 0, 0},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        FaultWitness witness = {cases[i].failure, cases[i].reason, 0};
        XrXirCallAccounting accounting = {0};
        XrXirCallConfig config; CHECK(xr_xir_call_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY); config.entries = entries; config.entry_count = 2; config.instance = &witness; config.byte_limit = 65536; config.poll_limit = 10; config.depth_limit = 2; config.accounting = &accounting; config.output = (XrXirOutputProvider) {0}; config.admission = (XrXirValueAdmission) {0};
        XrXirCall *call = NULL;
        CHECK(xr_xir_call_new(&config, 0, NULL, 0, &call) == XR_XIR_CALL_READY);
        XrXirCallResult result = xr_xir_call_poll_bounded(call, UINT64_MAX);
        CHECK(result.status == cases[i].reason);
        CHECK(result.value.type == XR_XIR_UNIT && !result.value.reserved && !result.value.payload);
        CHECK(result.wake == 0 && witness.cleanups == 2 && accounting.depth == 0);
        CHECK(xr_xir_call_poll_bounded(call, UINT64_MAX).status == cases[i].reason);
        CHECK(witness.cleanups == 2);
        CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
        CHECK(accounting.live_bytes == 0 && accounting.allocations == accounting.frees);
    }
}

#include "xir_bounds_call_cases.h"

static int call_complete_cases(void) {
    SourceFixtureOwner compiler = {0}; source_fixture_owner_new(&compiler);
    bounds_fault_boundary();
    match_fault_boundary();
    fault_boundary();
    callback_cases(&compiler.context);
    bounded_stack();
    xir_instruction_calls(&compiler.context);
    call_admission(&compiler.context);
    source_fixture_owner_free(&compiler);
    puts("Resumable native-to-Lowered-VM comparator and bounded trampoline witnesses passed");
    return 0;
}

static void call_owned_recovery(void) {
    for(unsigned mode=0;mode<2;++mode)for(unsigned native=0;native<2;++native)
    for(unsigned attack=0;attack<3;++attack){
        SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
        const XrXirCallEntry *table=mode?fixture_calls1_entries:fixture_calls0_entries;
        XrXirArtifact *artifact=call_fixture(&owner.context,mode),*original_artifact=artifact;
        XrXirProgram *program=NULL;
        unsigned releases=call_code_releases;
        call_binding_attack=attack;call_binding_callbacks=0;
        call_factory_abi=attack?XR_XIR_PROGRAM_ABI_VERSION:0;
        XrXirStatus status=call_fixture_program_take(&owner.context,&artifact,
            native?table:NULL,mode,NULL,&program);
        call_factory_abi=XR_XIR_PROGRAM_ABI_VERSION;call_binding_attack=0;
        CHECK(status==(attack?XR_XIR_BAD_STRUCTURE:XR_XIR_BAD_LAYOUT));
        CHECK(!program && artifact==original_artifact && (attack?call_binding_callbacks>0:!call_binding_callbacks));
        CHECK(call_code_releases==releases);
        CHECK(call_fixture_program_take(&owner.context,&artifact,native?table:NULL,
            mode,NULL,&program)==XR_XIR_OK && !artifact && program);
        XrXirInstance *instance=call_instance_new(program);
        XrCompileResourceStats stats={0};
        CHECK(xr_compile_resources_stats(owner.context.resources,&stats)==XR_COMPILE_RESOURCE_OK);
        XrCompileResources *producer=owner.context.resources;
        memset(&owner,0xa5,sizeof(owner));xr_compile_resources_release(producer);
        xr_xir_compile_program_drop(program);program=NULL;
        CHECK(call_code_releases==releases);
        XrXirValue args[]={{XR_XIR_I64,0,9},{XR_XIR_I64,0,4}};
        CHECK(xr_xir_instance_start(instance,0,args,2)==XR_XIR_CALL_READY);
        XrXirInstanceResult run=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
        CHECK(run.outcome.status==XR_XIR_CALL_SUSPENDED);
        CHECK(xr_xir_instance_resume(instance,run.epoch,run.outcome.wake)==XR_XIR_CALL_READY);
        run=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
        CHECK(run.outcome.status==(mode?XR_XIR_CALL_THROWN:XR_XIR_CALL_RETURNED));
        XrXirValue escaped={0};
        CHECK(xr_xir_instance_take_result(instance,&escaped)==run.outcome.status);
        call_instance_free(instance,XR_XIR_CALL_READY);CHECK(call_code_releases==releases+1);
        if(mode){
            CHECK(instance_compile_live && instance_compile_bytes && xr_xir_value_valid(&escaped));
            XrXirDomain *reader=NULL;CHECK(xr_xir_domain_new(65536,&reader)==XR_XIR_VALUE_OK);
            CHECK(error_fixture_is_code(&escaped,reader,91));xr_xir_domain_drop(reader);
        }else CHECK(escaped.type==XR_XIR_I64 && escaped.payload==4);
        xr_xir_value_drop(&escaped);
        CHECK(!runtime_live && !runtime_bytes);instance_compile_zero();
        printf("Calls mode=%u native=%u attack=%u real_callbacks=%u lease=1 compiler_work=%llu physical_zero\n",
            mode,native,attack,call_binding_callbacks,(unsigned long long)stats.work);
    }
    instance_compile_report();puts("Calls current Input failure recovery, producer death, actual callbacks, execution4 and escaped Error91 passed");
}
int main(int argc,char **argv) {
    if(argc==1){int status=call_complete_cases();instance_compile_zero();return status;}
    CHECK(argc==2 && !strcmp(argv[1],"--owned-recovery"));call_owned_recovery();return 0;
}
