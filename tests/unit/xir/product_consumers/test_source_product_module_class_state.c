/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_module_class_state.c - Class publication order and physical reverse reclamation
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
#include "module_class_state_cases.h"
_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Current Checked identity");
_Static_assert(XR_XIR_VALUE_ABI_VERSION == 22 && XR_XIR_CALL_ABI_VERSION == 28 &&
    XR_XIR_PROGRAM_ABI_VERSION == 29, "Current execution identity");
_Static_assert(XR_XIR_UNIT == 0 && XR_XIR_I64 == 2 && XR_XIR_CONST_INT == 2 && XR_XIR_RETURN == 33,
    "Independent module execution model");
_Static_assert(XR_XIR_CLASS_NEW == 118 && XR_XIR_CLASS_SET == 120 && XR_XIR_COPY == 18, "Independent class operations");
_Static_assert(XR_XIR_SLOT_LOAD == 4 && XR_XIR_SLOT_INIT == 5 && XR_XIR_SLOT_STORE == 6 &&
    XR_XIR_CONST_STRING == 3 && XR_XIR_PRINT == 24 && XR_XIR_ADD_INT == 25, "Independent slot operations");

#if defined(XR_MODULE_NATIVE)
extern const XrXirCallEntry source_module_class_state_0_entries[9], source_module_class_state_1_entries[9];
extern const char source_module_class_state_0_sha[65], source_module_class_state_1_sha[65];
static const XrXirCallEntry *native_entries(unsigned graph) {
    const XrXirCallEntry *entries[] = {source_module_class_state_0_entries, source_module_class_state_1_entries};
    CHECK(graph < 2); return entries[graph];
}
static const char *native_digest(unsigned graph) {
    const char *digests[] = {source_module_class_state_0_sha, source_module_class_state_1_sha};
    CHECK(graph < 2); return digests[graph];
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
static unsigned active_instance, releases, active_policy;
static unsigned lifecycle_count[2], published[2], released_slots[2], returns[2][9];
static uint32_t init_trace[2][4], init_count[2];
static size_t resumed[2];
static uintptr_t objects[2][4], fields[2][4];
static const unsigned publication_order[] = {1, 0, 3, 2}, release_order[] = {2, 3, 0, 1};

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
    ++resumed[active_owner->native[id] ? 1 : 0];
    XrXirAction action = entry->resume(view);
    if (action.kind == XR_XIR_ACTION_RETURN) {
        ++returns[active_instance][id];
        if (id < 4) { CHECK(init_count[active_instance] < 4); init_trace[active_instance][init_count[active_instance]++] = id; }
    }
    return action;
}
static void observe_lifecycle(void *context, XrXirLifecycleEvent event, uint32_t index) {
    unsigned instance = *(const unsigned *)context; CHECK(instance < 2 && instance == active_instance);
    if (event == XR_XIR_SLOT_PUBLISHED) {
        CHECK(lifecycle_count[instance] == 7 && published[instance] < 4 && !released_slots[instance]);
        CHECK(index == publication_order[published[instance]++]); return;
    }
    if (event == XR_XIR_SLOT_RELEASED) {
        CHECK(lifecycle_count[instance] == 8 && published[instance] == 4 && released_slots[instance] < 4);
        CHECK(index == release_order[released_slots[instance]++]);
        for (unsigned i = 0; i < 4; ++i) {
            unsigned slot = release_order[i]; bool live = i >= released_slots[instance] || (active_policy && slot == 2);
            CHECK(physically_live(objects[instance][slot]) == live);
            CHECK(physically_live(fields[instance][slot]) == live);
        }
        return;
    }
    CHECK(lifecycle_count[instance] < 8);
    unsigned cursor = lifecycle_count[instance]++;
    CHECK(event == (cursor % 2 ? XR_XIR_MODULE_READY : XR_XIR_MODULE_BEGIN)); CHECK(index == cursor / 2);
    if (cursor == 7) CHECK(published[instance] == 4);
}

static void release_code(void *pointer) {
    ModuleCodeOwner *owner = pointer;
    CHECK(owner == active_owner && !releases);
    xr_xir_compile_artifact_free(owner->lowered);
    active_owner = NULL; xr_compile_resources_free(owner); ++releases;
}

static XrXirArtifact *read_lower(const XrXirCompileContext *context, unsigned graph) {
    const ModuleClassStateCase *test = &module_class_state_cases[graph];
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
    char symbol[48]; CHECK(snprintf(symbol, sizeof(symbol), "source_module_class_state_%u", graph) > 0);
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
    printf("module-class-state graph=%u generated-bytes=%zu sha256=%s deterministic=1\n", graph, source->length, digest);
}

static XrXirValue verify_class(const XrXirValue *value, unsigned graph) {
    CHECK(value->type == 256 && !value->reserved && xr_xir_value_valid(value));
    XrXirValue field = {0}; XrXirValueAdmission admission = {0};
    admission.arena = xr_xir_value_arena(value); admission.work = 10000;
    CHECK(admission.arena && xr_xir_class_get(value, 0, &admission, &field) == XR_XIR_VALUE_OK);
    const char *bytes = NULL; size_t length = 0; const char *expected = module_class_state_cases[graph].field;
    CHECK(xr_xir_string_view(&field, &bytes, &length) && length == strlen(expected) && !memcmp(bytes, expected, length));
    return field;
}
static XrXirOutputStatus forbidden_output(void *context, const XrXirOutputGroup *group) {
    (void)context; (void)group; CHECK(false); return XR_XIR_OUTPUT_ERROR;
}

static unsigned original_shapes(const XrXirCompileContext *context) {
    unsigned mismatches = 0;
    for (unsigned i = 2; i < 3; ++i) {
        const ModuleClassStateCase *test = &module_class_state_cases[i];
        size_t live = instance_compile_live, bytes = instance_compile_bytes;
        uint8_t *input = xr_malloc(test->length); CHECK(input); memcpy(input, test->bytes, test->length);
        XrXirArtifact *checked = NULL; XrXirDiagnostic diagnostic = {0};
        XrXirStatus status = xr_xir_compile_checked_read(context, input, test->length, &checked, &diagnostic);
        CHECK(!memcmp(input, test->bytes, test->length)); memset(input, 0xa5, test->length); xr_free(input);
        if (status == XR_XIR_OK) { CHECK(checked); xr_xir_compile_artifact_free(checked); }
        else CHECK(!checked && diagnostic.status == status);
        CHECK(instance_compile_live == live && instance_compile_bytes == bytes);
        mismatches += status != XR_XIR_OK;
        printf("module-class-state original=%s expected=0 actual=%u compiler-refund=1 result=%s\n", test->name, status, status == XR_XIR_OK ? "PASS" : "FAIL");
    }
    return mismatches;
}

static XrXirValue invoke(XrXirInstance *instance, unsigned function) {
    CHECK(xr_xir_instance_start(instance, function, NULL, 0) == XR_XIR_CALL_READY);
    XrXirInstanceResult result = {0}; unsigned polls = 0;
    do { CHECK(++polls < 32); result = xr_xir_instance_poll_bounded(instance, 10000); }
    while (result.outcome.status == XR_XIR_CALL_READY);
    CHECK(result.outcome.status == XR_XIR_CALL_RETURNED);
    XrXirValue value = {0}; CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED); return value;
}
static void execute(const XrXirCompileContext *context, XrXirArtifact *lowered, unsigned graph, unsigned mode, unsigned policy) {
    const XrXirCallEntry *native = native_entries(graph); if (mode) CHECK(native);
    memset(returns, 0, sizeof(returns)); memset(init_count, 0, sizeof(init_count));
    memset(resumed, 0, sizeof(resumed)); memset(lifecycle_count, 0, sizeof(lifecycle_count));
    memset(objects, 0, sizeof(objects)); memset(fields, 0, sizeof(fields));
    releases = 0; active_policy = policy;
    memset(published, 0, sizeof(published)); memset(released_slots, 0, sizeof(released_slots));
    ModuleCodeOwner *owner = NULL;
    CHECK(xr_compile_resources_calloc(context->resources, 1, sizeof(*owner), (void **)&owner) == XR_COMPILE_RESOURCE_OK);
    owner->lowered = lowered;
    for (uint32_t f = 0; f < 9; ++f) {
        owner->native[f] = mode == 1 || (mode == 2 && !(f % 2)) || (mode == 3 && f % 2);
        if (owner->native[f]) owner->actual[f] = native[f];
        else CHECK(xr_xir_compile_vm_bind(lowered, f, &owner->bindings[f], &owner->actual[f]) == XR_XIR_OK);
        CHECK(!owner->actual[f].parameter_count && owner->actual[f].result ==
            (f < 4 ? XR_XIR_UNIT : f == 4 ? XR_XIR_I64 : (XrXirType)256));
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
    XrXirValue escaped[2] = {0};
    for (unsigned repeat = 0; repeat < 24; ++repeat) for (unsigned i = 0; i < 2; ++i) {
        active_instance = i; XrXirValue result = invoke(instances[i], 4);
        CHECK(result.type == XR_XIR_I64 && !result.reserved && !result.payload); xr_xir_value_drop(&result);
        for (unsigned slot = 0; slot < 4; ++slot) {
            XrXirValue value = invoke(instances[i], 5 + slot); XrXirValue field = verify_class(&value, graph);
            uintptr_t object = value_address(&value), text = value_address(&field);
            CHECK(physically_live(object) && physically_live(text));
            if (!repeat) { objects[i][slot] = object; fields[i][slot] = text; }
            CHECK(objects[i][slot] == object && fields[i][slot] == text);
            for (unsigned j = 0; j < 2; ++j) for (unsigned k = 0; k < 4; ++k)
                if (objects[j][k] && (j != i || k != slot)) CHECK(objects[j][k] != object);
            if (policy && repeat == 23 && slot == 2) CHECK(xr_xir_value_copy(&value, &escaped[i]) == XR_XIR_VALUE_OK);
            xr_xir_value_drop(&field); xr_xir_value_drop(&value);
        }
        CHECK(lifecycle_count[i] == 8 && init_count[i] == 4 && published[i] == 4 && !released_slots[i]);
        for (unsigned j = 0; j < 4; ++j) CHECK(init_trace[i][j] == j && returns[i][j] == 1);
        for (unsigned j = 4; j < 9; ++j) CHECK(returns[i][j] == repeat + 1);
        printf("module-class-state graph=%u mode=%u policy=%u instance=%u repeat=%u entry=0 class-identities=4 fields-verified=4 init-once=1 result=PASS\n",
            graph, mode, policy, i, repeat);
    }
    for (unsigned i = 0; i < 2; ++i) {
        active_instance = i; CHECK(!releases && active_owner);
        CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY); CHECK(releases == i && released_slots[i] == 4);
        if (!i) for (unsigned j = 0; j < 4; ++j) CHECK(physically_live(objects[1][j]) && physically_live(fields[1][j]));
    }
    CHECK(releases == 1 && !active_owner);
    if (mode == 0) CHECK(resumed[0] && !resumed[1]);
    else if (mode == 1) CHECK(resumed[1] && !resumed[0]); else CHECK(resumed[0] && resumed[1]);
    for (unsigned i = 0; i < 2; ++i) if (policy) {
        CHECK(physically_live(objects[i][2]) && physically_live(fields[i][2]));
        XrXirValue copy = {0}; CHECK(xr_xir_value_copy(&escaped[i], &copy) == XR_XIR_VALUE_OK);
        XrXirValue field = verify_class(&copy, graph); xr_xir_value_drop(&copy); xr_xir_value_drop(&escaped[i]);
        CHECK(!physically_live(objects[i][2]) && physically_live(fields[i][2]));
        const char *text = NULL; size_t length = 0; const char *expected = module_class_state_cases[graph].field;
        CHECK(xr_xir_string_view(&field, &text, &length) && length == strlen(expected) && !memcmp(text, expected, length));
        xr_xir_value_drop(&field); CHECK(!physically_live(fields[i][2]));
    }
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    printf("module-class-state graph=%u mode=%u policy=%u main-calls=48 inspection-calls=192 classes=8 initializers=8 module-events=16 publications=8 releases=8 detached-classes=%u owners-dead=1 vm-resumes=%zu native-resumes=%zu code-release=1 physical=0/0 result=PASS\n",
        graph, mode, policy, 2*policy, resumed[0], resumed[1]);
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
    for (unsigned graph = 0; graph < 2; ++graph) {
        XrXirArtifact *lowered = read_lower(&context, graph);
        XrXirCSource source = {0}; char digest[65]; emit_source(lowered, graph, &source, digest);
        if (emit) {
            char path[2048]; int size = snprintf(path, sizeof(path), "%s/graph%u.c", argv[2], graph);
            CHECK(size > 0 && (size_t)size < sizeof(path)); FILE *file = fopen(path, "wb"); CHECK(file);
            CHECK(fwrite(source.text, 1, source.length, file) == source.length);
            CHECK(fprintf(file, "\nconst char source_module_class_state_%u_sha[65]=\"%s\";\n", graph, digest) > 0);
            CHECK(fclose(file) == 0); xr_xir_compile_artifact_free(lowered);
        } else {
#if defined(XR_MODULE_NATIVE)
            if (mode) CHECK(!strcmp(digest, native_digest(graph)));
#endif
            execute(&context, lowered, graph, mode, 0);
            execute(&context, read_lower(&context, graph), graph, mode, 1);
        }
        xr_xir_compile_c_source_free(&source);
    }
    XrCompileResourceStats stats = {0};
    CHECK(xr_compile_resources_stats(context.resources, &stats) == XR_COMPILE_RESOURCE_OK);
    printf("module-class-state allocated=%" PRIu64 " peak=%" PRIu64 " work=%" PRIu64 "\n",
        stats.allocated_bytes, stats.peak_bytes, stats.work);
    xr_compile_resources_release(context.resources); instance_compile_report();
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    puts("module-class-state release compiler=0/0 runtime=0/0 table=0 result=PASS");
    printf("module-class-state original-shape-mismatches=%u result=%s\n", mismatches, mismatches ? "FAIL" : "PASS");
    return mismatches ? 1 : 0;
}
