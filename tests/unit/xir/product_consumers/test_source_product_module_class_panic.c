/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_module_class_panic.c - Class assertion failure, sticky panic ownership and physical reclamation
 *
 * KEY CONCEPT:
 *   Observe actual entry resumes without changing authenticated call views.
 *   Instance slots retain class identity and release in reverse publication order.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_output.h"
#include "xir/xxir_class.h"
#include "base/xsha256.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
#include "xir_runtime_allocations.h"
#include "module_class_panic_cases.h"
_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Current Checked identity");
_Static_assert(XR_XIR_VALUE_ABI_VERSION == 22 && XR_XIR_CALL_ABI_VERSION == 28 &&
    XR_XIR_PROGRAM_ABI_VERSION == 29, "Current execution identity");
_Static_assert(XR_XIR_UNIT == 0 && XR_XIR_I64 == 2 && XR_XIR_CONST_INT == 2 && XR_XIR_RETURN == 33,
    "Independent module execution model");
_Static_assert(XR_XIR_CLASS_NEW == 118 && XR_XIR_CLASS_SET == 120 && XR_XIR_COPY == 18, "Independent class operations");
_Static_assert(XR_XIR_ASSERT_CONDITION == 123 && XR_XIR_PANIC_ASSERTION == 445, "Independent assertion identity");
_Static_assert(XR_XIR_SLOT_LOAD == 4 && XR_XIR_SLOT_INIT == 5 && XR_XIR_SLOT_STORE == 6 &&
    XR_XIR_CONST_STRING == 3 && XR_XIR_PRINT == 24 && XR_XIR_ADD_INT == 25, "Independent slot operations");

#if defined(XR_MODULE_NATIVE)
extern const XrXirCallEntry source_module_class_panic_0_entries[9], source_module_class_panic_1_entries[9], source_module_class_panic_2_entries[9];
extern const char source_module_class_panic_0_sha[65], source_module_class_panic_1_sha[65], source_module_class_panic_2_sha[65];
static const XrXirCallEntry *native_entries(unsigned graph) {
    const XrXirCallEntry *entries[] = {source_module_class_panic_0_entries,source_module_class_panic_1_entries,source_module_class_panic_2_entries};
    CHECK(graph < 3); return entries[graph];
}
static const char *native_digest(unsigned graph) {
    const char *digests[] = {source_module_class_panic_0_sha,source_module_class_panic_1_sha,source_module_class_panic_2_sha};
    CHECK(graph < 3); return digests[graph];
}
#else
static const XrXirCallEntry *native_entries(unsigned graph) { (void)graph; return NULL; }
#endif

typedef struct ModuleCodeOwner {
    XrXirArtifact *lowered;
    XrXirCallEntry actual[9], observed[9];
    XrXirVmBinding bindings[9];
    bool native[9];
} ModuleCodeOwner;
static ModuleCodeOwner *active_owner;
static unsigned active_instance, releases, active_graph;
static uintptr_t messages[2];
static unsigned assertion_actions[2];
static unsigned lifecycle_count[2], published[2], released_slots[2], returns[2][9];
static uint32_t init_trace[2][4], init_count[2];
static size_t resumed[2];
static unsigned suspensions[2], entry_resumes[2][9];
static XrXirValue verify_class(const XrXirValue *value, unsigned graph);
static uintptr_t objects[2][4], fields[2][4];
static const unsigned publication_order[] = {1, 0, 3}, release_order[] = {3, 0, 1};

static uintptr_t value_address(const XrXirValue *value) {
    uintptr_t address = 0;
    _Static_assert(sizeof(address) == sizeof(value->payload), "Physical observation uses the admitted x64 target");
    memcpy(&address, &value->payload, sizeof(address)); return address;
}
static bool physically_live(uintptr_t address) {
    CHECK(address);
    for (size_t i = 0; i < runtime_owned_capacity; ++i)
        if ((uintptr_t)runtime_owned[i].pointer == address) return true;
    return false;
}
static XrXirAction observed_resume(XrXirCallView *view) {
    uint32_t id = xr_xir_call_current_entry(view->activation);
    CHECK(active_owner && id < 9 && active_instance < 2);
    const XrXirCallEntry *entry = &active_owner->actual[id]; CHECK(view->environment == entry->environment);
    ++resumed[active_owner->native[id] ? 1 : 0]; ++entry_resumes[active_instance][id];
    XrXirAction action = entry->resume(view);
    if (action.kind == XR_XIR_ACTION_RETURN) {
        ++returns[active_instance][id];
        if (id < 4) { CHECK(init_count[active_instance] < 4); init_trace[active_instance][init_count[active_instance]++] = id; }
    }
    if (action.kind == XR_XIR_ACTION_SUSPEND) {
        CHECK(active_graph == 1 && id == 3 && !suspensions[active_instance]);
        CHECK(!published[active_instance] && !released_slots[active_instance]);
        ++suspensions[active_instance];
    }
    if (action.kind == XR_XIR_ACTION_FAULT && action.value.payload == XR_XIR_CALL_ASSERTION) {
        CHECK(id == 3 && !assertion_actions[active_instance]++ && published[active_instance] == 3);
        CHECK(xr_xir_panic_valid(&action.panic) && action.panic.detail.code == XR_XIR_PANIC_ASSERTION);
        messages[active_instance] = value_address(&action.panic.message);
        CHECK(physically_live(messages[active_instance]));
        for (unsigned n = 0; n < 3; ++n) {
            unsigned slot = publication_order[n]; XrXirValue value = {0};
            CHECK(xr_xir_instance_slot_read(view, slot, &value) == XR_XIR_CALL_READY);
            XrXirValue field = verify_class(&value, active_graph);
            objects[active_instance][slot] = value_address(&value); fields[active_instance][slot] = value_address(&field);
            CHECK(physically_live(objects[active_instance][slot]) && physically_live(fields[active_instance][slot]));
            CHECK(fields[active_instance][slot] != messages[active_instance]);
            for (unsigned j = 0; j < n; ++j) CHECK(objects[active_instance][publication_order[j]] != objects[active_instance][slot]);
            xr_xir_value_drop(&field); xr_xir_value_drop(&value);
        }
        XrXirValue sentinel = {XR_XIR_I64, 0, 99};
        CHECK(xr_xir_instance_slot_read(view, 2, &sentinel) == XR_XIR_CALL_BAD_STATE);
        CHECK(sentinel.type == XR_XIR_I64 && !sentinel.reserved && sentinel.payload == 99);
    }
    return action;
}
static void observe_lifecycle(void *context, XrXirLifecycleEvent event, uint32_t index) {
    unsigned instance = *(const unsigned *)context; CHECK(instance < 2 && instance == active_instance);
    if (event == XR_XIR_SLOT_PUBLISHED) {
        CHECK(lifecycle_count[instance] == 7 && published[instance] < 3 && !released_slots[instance]);
        CHECK(index == publication_order[published[instance]++]); return;
    }
    if (event == XR_XIR_SLOT_RELEASED) {
        CHECK((lifecycle_count[instance] == 7 || lifecycle_count[instance] == 8) && published[instance] == 3 && released_slots[instance] < 3);
        CHECK(index == release_order[released_slots[instance]++]);
        for (unsigned i = 0; i < 3; ++i) {
            unsigned slot = release_order[i]; bool live = i >= released_slots[instance];
            CHECK(physically_live(objects[instance][slot]) == live);
            CHECK(physically_live(fields[instance][slot]) == live);
        }
        CHECK(physically_live(messages[instance]));
        return;
    }
    CHECK(lifecycle_count[instance] < 8);
    unsigned cursor = lifecycle_count[instance]++;
    CHECK(event == (cursor % 2 ? XR_XIR_MODULE_READY : XR_XIR_MODULE_BEGIN)); CHECK(index == cursor / 2);
    if (cursor == 7) CHECK(published[instance] == 3);
}

static void release_code(void *pointer) {
    ModuleCodeOwner *owner = pointer;
    CHECK(owner == active_owner && !releases);
    xr_xir_compile_artifact_free(owner->lowered);
    active_owner = NULL; xr_compile_resources_free(owner); ++releases;
}

static XrXirArtifact *read_lower(const XrXirCompileContext *context, unsigned graph) {
    const ModuleClassPanicCase *test = &module_class_panic_cases[graph];
    uint8_t *input = xr_malloc(test->length); CHECK(input); memcpy(input, test->bytes, test->length);
    XrXirArtifact *checked = NULL, *lowered = NULL;
    XrXirDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_checked_read(context, input, test->length, &checked, &diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr, "graph=%u admission=%u function=%u block=%u instruction=%u reason=%u\n",
        graph, status, diagnostic.function, diagnostic.block, diagnostic.instruction, diagnostic.reason);
    CHECK(status == XR_XIR_OK);
    CHECK(!memcmp(input, test->bytes, test->length)); memset(input, 0xa5, test->length); xr_free(input);
    CHECK(xr_xir_compile_artifact_verify(checked, NULL) == XR_XIR_OK);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    CHECK(packet.length == test->length && !memcmp(packet.bytes, test->bytes, test->length));
    xr_xir_compile_checked_packet_free(&packet);
    const XrXirModule *module = xr_xir_compile_artifact_module(checked);
    CHECK(module && module->function_count == 9 && module->declarations);
    CHECK(module->declarations->module_count == 4 && module->declarations->entry_function == 4);
    for (unsigned i = 0; i < 9; ++i) {
        CHECK(!module->functions[i].parameter_count);
        CHECK(module->functions[i].result == (i < 4 ? XR_XIR_UNIT : i == 4 ? XR_XIR_I64 : (XrXirType)256));
    }
    CHECK(module->declarations->slot_count == 4 && xr_xir_type_is_class(module->types, (XrXirType)256));
    for (unsigned i = 0; i < 4; ++i) CHECK(module->declarations->slots[i].module == 3 &&
        module->declarations->slots[i].type == 256 && module->declarations->slots[i].mutable == 1);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    XrXirArtifact *specialized = NULL;
    CHECK(xr_xir_compile_specialize(checked, &specialized, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_compile_artifact_verify(specialized, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_lower(specialized, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(specialized);
    CHECK(xr_xir_compile_artifact_verify(lowered, NULL) == XR_XIR_OK);
    return lowered;
}

static void emit_source(XrXirArtifact *lowered, unsigned graph, XrXirCSource *source, char digest[65]) {
    char symbol[48]; CHECK(snprintf(symbol, sizeof(symbol), "source_module_class_panic_%u", graph) > 0);
    CHECK(xr_xir_compile_emit_c(lowered, symbol, 1048576, source) == XR_XIR_OK);
    XrXirCSource repeated = {0};
    CHECK(xr_xir_compile_emit_c(lowered, symbol, 1048576, &repeated) == XR_XIR_OK);
    CHECK(source->text != repeated.text && source->length == repeated.length);
    CHECK(!memcmp(source->text, repeated.text, source->length)); xr_xir_compile_c_source_free(&repeated);
    uint8_t hash[32]; xr_sha256((const uint8_t *)source->text, source->length, hash);
    for (size_t i = 0; i < 32; ++i) {
        digest[2*i] = "0123456789abcdef"[hash[i] >> 4]; digest[2*i+1] = "0123456789abcdef"[hash[i] & 15];
    }
    digest[64] = 0;
    printf("module-class-panic graph=%u generated-bytes=%zu sha256=%s deterministic=1\n", graph, source->length, digest);
}

static XrXirValue verify_class(const XrXirValue *value, unsigned graph) {
    CHECK(value->type == 256 && !value->reserved && xr_xir_value_valid(value));
    XrXirValue field = {0}; XrXirValueAdmission admission = {0};
    admission.arena = xr_xir_value_arena(value); admission.work = 10000;
    CHECK(admission.arena && xr_xir_class_get(value, 0, &admission, &field) == XR_XIR_VALUE_OK);
    const char *bytes = NULL; size_t length = 0; const char *expected = "x"; (void)graph;
    CHECK(xr_xir_string_view(&field, &bytes, &length) && length == strlen(expected) && !memcmp(bytes, expected, length));
    return field;
}
static XrXirOutputStatus forbidden_output(void *context, const XrXirOutputGroup *group) {
    (void)context; (void)group; CHECK(false); return XR_XIR_OUTPUT_ERROR;
}

static unsigned original_shapes(const XrXirCompileContext *context) {
    unsigned mismatches = 0;
    for (unsigned i = 3; i < 6; ++i) {
        const ModuleClassPanicCase *test = &module_class_panic_cases[i];
        size_t live = instance_compile_live, bytes = instance_compile_bytes;
        uint8_t *input = xr_malloc(test->length); CHECK(input); memcpy(input, test->bytes, test->length);
        XrXirArtifact *checked = NULL; XrXirDiagnostic diagnostic = {0};
        XrXirStatus status = xr_xir_compile_checked_read(context, input, test->length, &checked, &diagnostic);
        CHECK(!memcmp(input, test->bytes, test->length)); memset(input, 0xa5, test->length); xr_free(input);
        if (status == XR_XIR_OK) { CHECK(checked); xr_xir_compile_artifact_free(checked); }
        else CHECK(!checked && diagnostic.status == status);
        CHECK(instance_compile_live == live && instance_compile_bytes == bytes);
        mismatches += status != XR_XIR_OK;
        printf("module-class-panic original=%s expected=0 actual=%u compiler-refund=1 result=%s\n", test->name, status, status == XR_XIR_OK ? "PASS" : "FAIL");
    }
    return mismatches;
}

static XrXirInstanceResult poll_terminal(XrXirInstance *instance) {
    XrXirInstanceResult result = {0}; size_t polls = 0;
    do { CHECK(++polls < 32); result = xr_xir_instance_poll_bounded(instance, 10000); }
    while (result.outcome.status == XR_XIR_CALL_READY);
    return result;
}

static void wait_authority(XrXirInstance *instance, XrXirInstanceResult pending) {
    CHECK(pending.outcome.status == XR_XIR_CALL_SUSPENDED && pending.epoch && pending.outcome.wake);
    CHECK(pending.epoch < UINT64_MAX && pending.outcome.wake < UINT64_MAX);
    XrXirWaitRequest request = {0};
    CHECK(xr_xir_instance_wait_request(instance, pending.epoch, pending.outcome.wake, &request) == XR_XIR_CALL_READY);
    CHECK(request.kind == XR_XIR_WAIT_YIELD && !request.reserved && !request.after_ms &&
        !request.subject && !request.generation && !request.ticket);
    XrXirWaitRequest sentinel, before; memset(&sentinel, 0xa5, sizeof(sentinel)); memcpy(&before, &sentinel, sizeof(before));
    CHECK(xr_xir_instance_wait_request(instance, pending.epoch+1, pending.outcome.wake, &sentinel) == XR_XIR_CALL_BAD_STATE);
    CHECK(!memcmp(&sentinel, &before, sizeof(sentinel)));
    CHECK(xr_xir_instance_wait_request(instance, pending.epoch, pending.outcome.wake+1, &sentinel) == XR_XIR_CALL_BAD_STATE);
    CHECK(!memcmp(&sentinel, &before, sizeof(sentinel)));
    CHECK(xr_xir_instance_resume(instance, pending.epoch+1, pending.outcome.wake) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_resume(instance, pending.epoch, pending.outcome.wake+1) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_start(instance, 4, NULL, 0) == XR_XIR_CALL_BUSY);
    XrXirInstanceResult repeated = poll_terminal(instance);
    CHECK(repeated.outcome.status == XR_XIR_CALL_SUSPENDED && repeated.epoch == pending.epoch &&
        repeated.outcome.wake == pending.outcome.wake);
    CHECK(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_INITIALIZING);
    CHECK(xr_xir_instance_wait_request(instance, pending.epoch, pending.outcome.wake, &request) == XR_XIR_CALL_READY);
    CHECK(request.kind == XR_XIR_WAIT_YIELD && !request.reserved && !request.after_ms &&
        !request.subject && !request.generation && !request.ticket);
}

static void verify_message(const XrXirCallResult *result, unsigned graph, unsigned instance) {
    CHECK(result->status == XR_XIR_CALL_ASSERTION && !result->wake && xr_xir_call_result_valid(result));
    CHECK(result->value.type == XR_XIR_UNIT && !result->value.reserved && !result->value.payload);
    CHECK(result->panic.detail.code == XR_XIR_PANIC_ASSERTION && !result->panic.detail.reserved &&
        !result->panic.detail.index && !result->panic.detail.length);
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(&result->panic.message, &bytes, &length));
    CHECK(length == (graph == 2 ? 1u : 0u) && (!length || bytes[0] == 'x'));
    CHECK(value_address(&result->panic.message) == messages[instance] && physically_live(messages[instance]));
}

static unsigned execute(const XrXirCompileContext *context, XrXirArtifact *lowered, unsigned graph, unsigned mode) {
    const XrXirCallEntry *native = native_entries(graph); if (mode) CHECK(native);
    memset(returns, 0, sizeof(returns)); memset(init_count, 0, sizeof(init_count));
    memset(resumed, 0, sizeof(resumed)); memset(lifecycle_count, 0, sizeof(lifecycle_count));
    memset(objects, 0, sizeof(objects)); memset(fields, 0, sizeof(fields));
    memset(suspensions, 0, sizeof(suspensions)); memset(entry_resumes, 0, sizeof(entry_resumes)); releases = 0; active_graph = graph;
    memset(messages, 0, sizeof(messages)); memset(assertion_actions, 0, sizeof(assertion_actions));
    memset(published, 0, sizeof(published)); memset(released_slots, 0, sizeof(released_slots));
    ModuleCodeOwner *owner = NULL;
    CHECK(xr_compile_resources_calloc(context->resources, 1, sizeof(*owner), (void **)&owner) == XR_COMPILE_RESOURCE_OK);
    owner->lowered = lowered;
    for (uint32_t f = 0; f < 9; ++f) {
        owner->native[f] = mode == 1 || (mode == 2 && !(f % 2)) || (mode == 3 && f % 2);
        if (owner->native[f]) owner->actual[f] = native[f];
        else CHECK(xr_xir_compile_vm_bind(lowered, f, &owner->bindings[f], &owner->actual[f]) == XR_XIR_OK);
        owner->observed[f] = owner->actual[f]; owner->observed[f].resume = observed_resume;
    }
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION},
        owner->observed, 9, module->declarations, {owner, release_code}, module->types, xr_xir_compile_program_proof(lowered)};
    XrXirProgram *program = NULL; CHECK(xr_xir_compile_program_seal(context, &spec, &program) == XR_XIR_OK); active_owner = owner;
    XrXirInstanceConfig config = {0}; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    XrXirInstance *instances[2] = {NULL, NULL}; unsigned ids[] = {0, 1}; config.trace = observe_lifecycle;
    config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, forbidden_output, NULL};
    for (unsigned i = 0; i < 2; ++i) { config.trace_context = &ids[i];
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY); }
    CHECK(instances[0] != instances[1]); xr_xir_compile_program_drop(program);
    XrXirInstanceResult pending[2] = {0};
    if (graph == 1) for (unsigned i = 0; i < 2; ++i) {
        active_instance = i;
        CHECK(xr_xir_instance_start(instances[i], 4, NULL, 0) == XR_XIR_CALL_READY);
        pending[i] = poll_terminal(instances[i]);
        size_t attempts = runtime_attempts, calls = resumed[0]+resumed[1];
        wait_authority(instances[i], pending[i]);
        CHECK(runtime_attempts == attempts && resumed[0]+resumed[1] == calls);
        CHECK(suspensions[i] == 1 && !published[i] && !released_slots[i] && !assertion_actions[i]);
        for (unsigned j = 0; j < 4; ++j) CHECK(!objects[i][j] && !fields[i][j]);
        printf("module-class-panic graph=%u mode=%u instance=%u pending=YIELD publications=0 wait-fields=exact bad-wakes=4 sentinel=preserved result=PASS\n",graph,mode,i);
    }
    XrXirCallResult retained[2] = {0}; unsigned mismatches = 0;
    for (unsigned repeat = 0; repeat < 24; ++repeat) for (unsigned i = 0; i < 2; ++i) {
        active_instance = i; size_t attempts = runtime_attempts, calls = resumed[0]+resumed[1];
        if (!repeat && graph == 1) CHECK(xr_xir_instance_resume(instances[i],pending[i].epoch,pending[i].outcome.wake) == XR_XIR_CALL_READY);
        else CHECK(xr_xir_instance_start(instances[i],4,NULL,0) == (repeat ? XR_XIR_CALL_ASSERTION : XR_XIR_CALL_READY));
        XrXirInstanceResult result = poll_terminal(instances[i]); verify_message(&result.outcome,graph,i);
        CHECK(xr_xir_instance_state(instances[i]) == XR_XIR_INSTANCE_FAILED);
        CHECK(lifecycle_count[i] == 7 && init_count[i] == 3 && published[i] == 3 && released_slots[i] == 3);
        CHECK(assertion_actions[i] == 1 && suspensions[i] == (graph == 1 ? 1u : 0u));
        for (unsigned j = 0; j < 3; ++j) CHECK(init_trace[i][j] == j && returns[i][j] == 1);
        CHECK(!returns[i][3]); for (unsigned j = 4; j < 9; ++j) CHECK(!entry_resumes[i][j]);
        if (graph == 1) CHECK(xr_xir_instance_resume(instances[i],pending[i].epoch,pending[i].outcome.wake) == XR_XIR_CALL_BAD_STATE);
        XrXirValue sentinel = {XR_XIR_I64,0,99};
        CHECK(xr_xir_instance_take_result(instances[i],&sentinel) == XR_XIR_CALL_BAD_STATE);
        CHECK(sentinel.type == XR_XIR_I64 && !sentinel.reserved && sentinel.payload == 99);
        XrXirCallResult copy = {0}; CHECK(xr_xir_instance_copy_failure(instances[i],&copy) == XR_XIR_CALL_ASSERTION);
        verify_message(&copy,graph,i); if (!repeat) xr_xir_call_result_move(&copy,&retained[i]);
        xr_xir_call_result_drop(&copy);
        if (repeat) CHECK(runtime_attempts == attempts && resumed[0]+resumed[1] == calls);
        bool present = result.outcome.panic.message.type == XR_XIR_STRING;
        bool matches = present == (graph == 2); mismatches += !matches;
        printf("module-class-panic graph=%u mode=%u instance=%u repeat=%u assertion=445 expected-message=%u actual-message=%u bytes=%u class-release=3 result=%s\n",
            graph,mode,i,repeat,graph==2,present,graph==2,matches ? "PASS" : "FAIL");
        if (!repeat && !i) {
            CHECK(!published[1] && !released_slots[1] && !assertion_actions[1]);
            if (graph == 1) CHECK(lifecycle_count[1] == 7 && init_count[1] == 3 && suspensions[1] == 1);
            else for (unsigned j = 0; j < 9; ++j) CHECK(!entry_resumes[1][j]);
        }
    }
    CHECK(messages[0] != messages[1]);
    for (unsigned i = 0; i < 2; ++i) {
        active_instance = i; CHECK(!releases && active_owner);
        CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
        CHECK(released_slots[i] == 3 && physically_live(messages[i]));
    }
    CHECK(releases == 1 && !active_owner);
    for (unsigned i = 0; i < 2; ++i) {
        verify_message(&retained[i],graph,i);
        XrXirCallResult copy = {0}; CHECK(xr_xir_call_result_copy(&retained[i],&copy) == XR_XIR_VALUE_OK);
        verify_message(&copy,graph,i); xr_xir_call_result_drop(&retained[i]);
        XrXirValue text = {0}; CHECK(xr_xir_value_copy(&copy.panic.message,&text) == XR_XIR_VALUE_OK);
        xr_xir_call_result_drop(&copy); CHECK(xr_xir_call_result_empty(&retained[i]) && xr_xir_call_result_empty(&copy));
        const char *bytes = NULL; size_t length = 0;
        CHECK(xr_xir_string_view(&text,&bytes,&length) && length == (graph == 2 ? 1u : 0u));
        CHECK(!length || bytes[0] == 'x'); CHECK(physically_live(messages[i]));
        xr_xir_value_drop(&text); CHECK(!physically_live(messages[i]));
        if (!i) verify_message(&retained[1],graph,1);
    }
    if (mode == 0) CHECK(resumed[0] && !resumed[1]);
    else if (mode == 1) CHECK(resumed[1] && !resumed[0]); else CHECK(resumed[0] && resumed[1]);
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    printf("module-class-panic graph=%u mode=%u observations=48 mismatches=%u sticky=46 retained-panics=2 escaped-messages=2 publications=6 releases=6 release-order=3,0,1 owners-dead=1 vm-resumes=%zu native-resumes=%zu code-release=1 physical=0/0 result=%s\n",
        graph,mode,mismatches,resumed[0],resumed[1],mismatches ? "FAIL" : "PASS");
    return mismatches;
}

int main(int argc, char **argv) {
    CHECK(argc == 2 || argc == 3);
    bool emit = !strcmp(argv[1], "emit");
    unsigned mode = !strcmp(argv[1], "vm") ? 0 : !strcmp(argv[1], "native") ? 1 :
        !strcmp(argv[1], "alternating-native") ? 2 : !strcmp(argv[1], "alternating-vm") ? 3 : 4;
    CHECK((emit && argc == 3) || (!emit && argc == 2 && mode < 4));
    XrXirCompileContext context = {0}; context.limits = xr_xir_compile_default_limits();
    XrCompileResourceLimits limits = {67108864, 8388608, 128000000};
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    unsigned mismatches = emit ? 0 : original_shapes(&context);
    for (unsigned graph = 0; graph < 3; ++graph) {
        XrXirArtifact *lowered = read_lower(&context, graph);
        XrXirCSource source = {0}; char digest[65]; emit_source(lowered, graph, &source, digest);
        if (emit) {
            char path[2048]; int size = snprintf(path, sizeof(path), "%s/graph%u.c", argv[2], graph);
            CHECK(size > 0 && (size_t)size < sizeof(path)); FILE *file = fopen(path, "wb"); CHECK(file);
            CHECK(fwrite(source.text, 1, source.length, file) == source.length);
            CHECK(fprintf(file, "\nconst char source_module_class_panic_%u_sha[65]=\"%s\";\n", graph, digest) > 0);
            CHECK(fclose(file) == 0); xr_xir_compile_artifact_free(lowered);
        } else {
#if defined(XR_MODULE_NATIVE)
            if (mode) CHECK(!strcmp(digest, native_digest(graph)));
#endif
            mismatches += execute(&context, lowered, graph, mode);
        }
        xr_xir_compile_c_source_free(&source);
    }
    XrCompileResourceStats stats = {0};
    CHECK(xr_compile_resources_stats(context.resources, &stats) == XR_COMPILE_RESOURCE_OK);
    printf("module-class-panic allocated=%" PRIu64 " peak=%" PRIu64 " work=%" PRIu64 "\n",
        stats.allocated_bytes, stats.peak_bytes, stats.work);
    xr_compile_resources_release(context.resources); instance_compile_report();
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    puts("module-class-panic release compiler=0/0 runtime=0/0 table=0 result=PASS");
    printf("module-class-panic original-and-message-mismatches=%u result=%s\n", mismatches, mismatches ? "FAIL" : "PASS");
    return mismatches ? 1 : 0;
}
