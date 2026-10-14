/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_observation.c - Actual backend frames for interleaved Source bodies
 */
#define main terminal_reference_main
#include "test_xir_mixed_terminals.c"
#undef main
#include "xir/xxir_call_observer_internal.h"

enum { OBS_FUNCTIONS = 64, OBS_FRAMES = 64, OBS_INSTANCES = 2, OBS_ROLES = 3 };
typedef struct ObservationFrame {
    XrXirCallEvent key;
    XrXirCallCodeKind code;
    XrXirCallStatus leave;
    unsigned begins, ends, codes;
    bool dispatch, left;
} ObservationFrame;
typedef struct ObservationCode ObservationCode;
typedef struct ObservationRecord {
    ObservationCode *code;
    XrXirInstance *instance;
    XrXirCallObserver observer;
    const XrXirCallView *view;
    ObservationFrame frames[OBS_FRAMES];
    TerminalTrace trace;
    unsigned count, events, installations;
    bool binding, installed;
} ObservationRecord;
struct ObservationCode {
    TerminalCode base;
    XrXirResumeEntry original[OBS_FUNCTIONS];
    XrXirCallCodeKind expected[OBS_FUNCTIONS];
    uint32_t roles[OBS_ROLES], functions;
    ObservationRecord records[OBS_INSTANCES];
};
static ObservationCode *observation_code;
static XrXirCallStatus observation_expected_fault;

static bool observation_key(const XrXirCallEvent *a, const XrXirCallEvent *b) {
    return a->binding.owner == b->binding.owner && a->binding.activation == b->binding.activation &&
        a->binding.generation == b->binding.generation && a->binding.ticket == b->binding.ticket && a->frame == b->frame;
}
static ObservationFrame *observation_frame(ObservationRecord *record, const XrXirCallEvent *event) {
    for (unsigned i = 0; i < record->count; ++i)
        if (observation_key(&record->frames[i].key, event)) return &record->frames[i];
    return NULL;
}
static void observation_event(void *pointer, const XrXirCallEvent *event) {
    ObservationRecord *record = pointer; CHECK(event && event->instance == record->instance);
    CHECK(event->binding.owner && event->binding.activation && event->binding.generation && event->binding.ticket);
    CHECK(event->frame && event->entry <= record->code->functions && event->depth);
    const size_t attempts = runtime_attempts;
    XrXirValue value = {0};
    CHECK(xr_xir_instance_poll_bounded(record->instance, 1).outcome.status == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_take_result(record->instance, &value) == XR_XIR_CALL_BUSY && value.type == XR_XIR_UNIT);
    CHECK(xr_xir_instance_stop(record->instance) == XR_XIR_CALL_BUSY);
    CHECK(runtime_attempts == attempts);
    ObservationFrame *frame = observation_frame(record, event);
    if (event->kind == XR_XIR_CALL_FRAME_PRESENT || event->kind == XR_XIR_CALL_FRAME_ENTER) {
        CHECK(!frame && record->count < OBS_FRAMES);
        frame = &record->frames[record->count++]; frame->key = *event; frame->code = event->code;
        if (event->kind == XR_XIR_CALL_FRAME_ENTER) CHECK(event->code == XR_XIR_CALL_CODE_UNKNOWN);
    } else CHECK(frame && !frame->left);
    CHECK(frame->key.parent == event->parent && frame->key.entry == event->entry && frame->key.depth == event->depth);
    if (event->kind == XR_XIR_CALL_FRAME_CODE) {
        CHECK(!frame->codes && frame->code == XR_XIR_CALL_CODE_UNKNOWN);
        CHECK(event->code == (event->entry == record->code->functions ?
            XR_XIR_CALL_CODE_NATIVE : record->code->expected[event->entry]));
        frame->code = event->code; ++frame->codes;
    } else if (event->kind == XR_XIR_CALL_DISPATCH_BEGIN) {
        CHECK(!frame->dispatch && event->code == frame->code);
        frame->dispatch = true; ++frame->begins;
    } else if (event->kind == XR_XIR_CALL_DISPATCH_END) {
        /* Installing in the first body replays its frame after BEGIN occurred. */
        CHECK(frame->dispatch || (!frame->begins && !frame->ends && frame->key.kind == XR_XIR_CALL_FRAME_PRESENT));
        CHECK(event->code == frame->code); frame->dispatch = false; ++frame->ends;
    } else if (event->kind == XR_XIR_CALL_FRAME_LEAVE) {
        CHECK(!frame->dispatch && event->code == frame->code);
        CHECK(event->status == XR_XIR_CALL_RETURNED ||
            (observation_expected_fault != XR_XIR_CALL_READY && event->status == observation_expected_fault));
        for (unsigned i = 0; i < record->count; ++i) {
            const ObservationFrame *child = &record->frames[i]; XrXirCallEvent parent_key = child->key;
            parent_key.frame = event->frame;
            if (child->key.parent == event->frame && observation_key(&parent_key, event)) CHECK(child->left);
        }
        frame->left = true; frame->leave = event->status;
    }
    ++record->events;
}
static ObservationRecord *observation_record(const void *instance) {
    CHECK(observation_code);
    for (unsigned i = 0; i < OBS_INSTANCES; ++i)
        if (observation_code->records[i].instance == instance) return &observation_code->records[i];
    CHECK(false); return NULL;
}
static XrXirAction observation_resume(XrXirCallView *view) {
    ObservationRecord *record = observation_record(view->instance);
    uint32_t entry = xr_xir_call_current_entry(view->activation);
    CHECK(entry < record->code->functions && !record->view);
    record->view = view; record->binding = true;
    XrXirCallView copy = *view;
    CHECK(xr_xir_call_observer_bind(&copy, &record->observer) == XR_XIR_CALL_BAD_STATE);
    XrXirCallStatus bound = xr_xir_call_observer_bind(view, &record->observer);
    if (bound != XR_XIR_CALL_READY) {
        CHECK(observation_expected_fault == XR_XIR_CALL_LIMIT && bound == XR_XIR_CALL_LIMIT && !record->events);
        record->binding = false; record->view = NULL;
        return xr_xir_call_fault(XR_XIR_RUN_FRAME_LIMIT);
    }
    record->binding = false;
    if (!record->installed) { record->installed = true; ++record->installations; }
    XrXirAction action = record->code->original[entry](view);
    record->view = NULL; return action;
}
static void observation_free(void *pointer) {
    ObservationCode *code = pointer; CHECK(observation_code == code);
    observation_code = NULL; terminal_code_free(&code->base);
}
static uint32_t observation_crossed_target(const XrXirModule *module, uint32_t root, uint32_t declaration) {
    const XrXirFunction *body = &module->functions[root];
    uint32_t target = UINT32_MAX, matches = 0;
    for (uint32_t i = 0; i < body->instruction_count; ++i) {
        const XrXirInstruction *op = &body->instructions[i];
        if (op->op != XR_XIR_CALL && op->op != XR_XIR_INVOKE) continue;
        CHECK(op->immediate >= 0 && (uint64_t)op->immediate < module->function_count);
        const uint32_t candidate = (uint32_t)op->immediate;
        const XrXirFunction *function = &module->functions[candidate];
        if (module->declarations->functions[candidate].module == module->declarations->functions[declaration].module &&
            function->parameter_count == 1 && function->result == XR_XIR_I64 &&
            xr_xir_callable_signature(module->types, function->parameters[0])) { target = candidate; ++matches; }
    }
    CHECK(matches == 1); return target;
}
static XrXirProgram *observation_program(TerminalBackend backend) {
    const XrXirCompileContext context = *effects_source_owner(UINT64_C(64)*1024*1024, UINT64_C(128000000));
    XrXirArtifact *checked = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(&context, terminal_fixture_program.proof.bytes,
        terminal_fixture_program.proof.length, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_lower(checked, &terminal_fixture_program.target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    CHECK(module->function_count == terminal_fixture_program.entry_count && module->function_count <= OBS_FUNCTIONS);
    ObservationCode *code = NULL;
    CHECK(xr_compile_resources_calloc(context.resources, 1, sizeof(*code), (void **)&code) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_calloc(context.resources, module->function_count,
        sizeof(*code->base.entries), (void **)&code->base.entries) == XR_COMPILE_RESOURCE_OK);
    code->functions = module->function_count;
    for (unsigned r = 0; r < OBS_ROLES; ++r) code->roles[r] = UINT32_MAX;
    XrXirProgramProof proof = xr_xir_compile_program_proof(lowered);
    CHECK(xr_xir_compile_vm_program_take(&lowered, &code->base.vm) == XR_XIR_OK && !lowered);
    for (uint32_t f = 0; f < module->function_count; ++f) {
        bool root = module->declarations->functions[f].module == module->declarations->root_module;
        bool native = backend == TERMINAL_NATIVE || (backend < TERMINAL_VM && root == (backend == TERMINAL_MIXED_NATIVE_ROOT));
        CHECK(terminal_fixture_program.entries[f].resume != code->base.vm->entries[f].resume);
        code->base.entries[f] = native ? terminal_fixture_program.entries[f] : code->base.vm->entries[f];
        code->original[f] = code->base.entries[f].resume; code->base.entries[f].resume = observation_resume;
        code->expected[f] = native ? XR_XIR_CALL_CODE_NATIVE : XR_XIR_CALL_CODE_VM;
        const XrXirFunction *function = &module->functions[f];
        if (function->name_length == 7 && !memcmp(function->name, "crossed", 7)) code->roles[root ? 0 : 1] = f;
        if (root && function->name_length == 8 && !memcmp(function->name, "callback", 8)) code->roles[2] = f;
    }
    CHECK(code->roles[0] == terminal_fixture_functions[3] && code->roles[1] != UINT32_MAX && code->roles[2] != UINT32_MAX);
    /* Follow the actual closed direct edge, rather than an unused declaration template. */
    code->roles[1] = observation_crossed_target(module, code->roles[0], code->roles[1]);
    for (unsigned i = 0; i < OBS_INSTANCES; ++i) {
        code->records[i].code = code; code->records[i].trace.kind = 3;
        code->records[i].observer = (XrXirCallObserver){&code->records[i], observation_event};
    }
    XrXirProgramSpec spec = terminal_fixture_program;
    spec.entries = code->base.entries; spec.types = module->types; spec.declarations = module->declarations;
    spec.proof = proof; spec.code = (XrXirCodeLease){code, observation_free};
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(&context, &spec, &program) == XR_XIR_OK && program);
    observation_code = code; return program;
}
static XrXirOutputStatus observation_output(void *pointer, const XrXirOutputGroup *group) {
    ObservationRecord *record = pointer;
    CHECK(!record->view && group && group->count == 1);
    /* Output adapters may own the dispatch. Source frame identity is verified
     * independently from its exact entry and backend observation below. */
    return terminal_output(&record->trace, group);
}
static void observation_verify(ObservationRecord *record, TerminalBackend backend) {
    unsigned roles[OBS_ROLES] = {0}, vm = 0, native = 0, launchers = 0, missing_begin = 0;
    for (unsigned i = 0; i < record->count; ++i) {
        ObservationFrame *frame = &record->frames[i];
        CHECK(frame->left && !frame->dispatch && frame->ends);
        CHECK(frame->codes == 1 || (frame->key.kind == XR_XIR_CALL_FRAME_PRESENT &&
            frame->key.code == XR_XIR_CALL_CODE_NATIVE && !frame->codes));
        CHECK(frame->ends >= frame->begins); missing_begin += frame->ends - frame->begins;
        CHECK(frame->code == (frame->key.entry == record->code->functions ?
            XR_XIR_CALL_CODE_NATIVE : record->code->expected[frame->key.entry]));
        if (frame->key.parent) {
            XrXirCallEvent key = frame->key; key.frame = key.parent;
            ObservationFrame *parent = observation_frame(record, &key);
            CHECK(parent && parent->key.depth + 1 == frame->key.depth);
        } else CHECK(frame->key.depth == 1);
        for (unsigned r = 0; r < OBS_ROLES; ++r) if (frame->key.entry == record->code->roles[r]) ++roles[r];
        if (frame->key.entry == record->code->functions) ++launchers;
        else if (frame->code == XR_XIR_CALL_CODE_VM) ++vm; else ++native;
    }
    CHECK(record->installed && record->installations == 1 && missing_begin == 1);
    CHECK(roles[0] == 2 && roles[1] == 2 && roles[2] == 4 && launchers == 2);
    if (backend == TERMINAL_VM) CHECK(vm && !native);
    else if (backend == TERMINAL_NATIVE) CHECK(native && !vm);
    else CHECK(vm && native);
}
static int observation_normal_main(int argc, char **argv) {
    CHECK(argc == 2); TerminalBackend backend = TERMINAL_MIXED_VM_ROOT;
    while (backend <= TERMINAL_NATIVE && strcmp(argv[1], terminal_backend_names[backend])) ++backend;
    CHECK(backend <= TERMINAL_NATIVE);
    XrXirProgram *program = observation_program(backend); ObservationCode *code = observation_code;
    XrXirInstance *instances[OBS_INSTANCES] = {0};
    for (unsigned i = 0; i < OBS_INSTANCES; ++i) {
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, observation_output, &code->records[i]};
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
        code->records[i].instance = instances[i];
    }
    xr_xir_compile_program_drop(program); CHECK(!terminal_code_releases);
    for (unsigned round = 0; round < 2; ++round) {
        for (unsigned i = 0; i < OBS_INSTANCES; ++i) {
            code->records[i].trace.count = 0;
            CHECK(xr_xir_instance_start(instances[i], code->roles[0], NULL, 0) == XR_XIR_CALL_READY);
        }
        bool done[OBS_INSTANCES] = {false, false}; unsigned polls = 0;
        while (!done[0] || !done[1]) for (unsigned i = 0; i < OBS_INSTANCES; ++i) {
            if (done[i]) continue;
            ObservationRecord peer = code->records[1-i];
            XrXirCallStatus status = xr_xir_instance_poll_bounded(instances[i], 1).outcome.status;
            CHECK(++polls < 4096 && !memcmp(&peer, &code->records[1-i], sizeof(peer)));
            CHECK(status == XR_XIR_CALL_READY || status == XR_XIR_CALL_RETURNED);
            done[i] = status == XR_XIR_CALL_RETURNED;
        }
        for (unsigned i = 0; i < OBS_INSTANCES; ++i) {
            TerminalTrace *trace = &code->records[i].trace;
            CHECK(trace->count == 6 && trace->root_bodies == round + 1 && trace->callee_bodies == round + 1);
            CHECK(trace->callback_bodies == 2*(round + 1));
            XrXirValue value = {0}; CHECK(xr_xir_instance_take_result(instances[i], &value) == XR_XIR_CALL_RETURNED);
            CHECK(value.type == XR_XIR_I64 && value.payload == 42); xr_xir_value_drop(&value);
        }
    }
    for (unsigned i = 0; i < OBS_INSTANCES; ++i) observation_verify(&code->records[i], backend);
    CHECK(xr_xir_instance_free(instances[0]) == XR_XIR_CALL_READY && !terminal_code_releases);
    code->records[0].instance = NULL;
    CHECK(xr_xir_instance_free(instances[1]) == XR_XIR_CALL_READY && terminal_code_releases == 1);
    CHECK(!observation_code && !runtime_live && !runtime_bytes); effects_source_owners_free();
    printf("Observed Source backend=%s root/callee/callback frames=4/4/8, exact code kind, two Instances two rounds quantum1, fixed result42 and peer unchanged, code lease1, physical0/0\n", terminal_backend_names[backend]);
    return 0;
}
#ifndef XR_OBSERVATION_RESOURCE_MAIN
int main(int argc, char **argv) { return observation_normal_main(argc, argv); }
#endif
