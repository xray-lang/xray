/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_f64_transport.c - Exact runtime binary64 carrier bits
 *
 * KEY CONCEPT:
 *   Independent Checked input defines caller and carrier functions. Runtime
 *   arguments transport every original bit pattern without numeric operations.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "base/xsha256.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
#include "xir_runtime_allocations.h"
#include "f64_transport_fixture.h"

_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Current Checked identity");
_Static_assert(XR_XIR_VALUE_ABI_VERSION == 22 && XR_XIR_CALL_ABI_VERSION == 28 &&
    XR_XIR_PROGRAM_ABI_VERSION == 29, "Current execution identity");
_Static_assert(XR_XIR_F64 == 13 && XR_XIR_CALL == 28 && XR_XIR_COPY == 18 && XR_XIR_RETURN == 33,
    "Independent named transport model");
_Static_assert(sizeof(f64_transport_bits) / sizeof(f64_transport_bits[0]) == 10, "Original binary64 corpus");

#if defined(XR_F64_NATIVE)
extern const XrXirCallEntry source_f64_transport_entries[4];
extern const char source_f64_transport_sha[65];
static const XrXirCallEntry *native_entries(void) { return source_f64_transport_entries; }
#else
static const XrXirCallEntry *native_entries(void) { return NULL; }
#endif

typedef struct TransportOwner {
    XrXirArtifact *lowered;
    XrXirCallEntry actual[4], observed[4];
    XrXirVmBinding bindings[4];
    bool native[4];
} TransportOwner;
static TransportOwner *active_owner;
static size_t resumed[2], crossings, releases;

static XrXirAction observed_resume(XrXirCallView *view) {
    uint32_t id = xr_xir_call_current_entry(view->activation);
    CHECK(active_owner && id < 4);
    const XrXirCallEntry *entry = &active_owner->actual[id];
    CHECK(view->environment == entry->environment);
    ++resumed[active_owner->native[id] ? 1 : 0];
    XrXirAction action = entry->resume(view);
    if (id == 0 && action.kind == XR_XIR_ACTION_CALL) {
        CHECK(action.callee == 1);
        ++crossings;
    }
    return action;
}

static void release_code(void *pointer) {
    TransportOwner *owner = pointer;
    CHECK(owner == active_owner && !releases);
    xr_xir_compile_artifact_free(owner->lowered);
    active_owner = NULL;
    xr_compile_resources_free(owner);
    ++releases;
}

static void hex_digest(const char *bytes, size_t length, char output[65]) {
    uint8_t digest[32];
    xr_sha256((const uint8_t *)bytes, length, digest);
    for (size_t i = 0; i < 32; ++i) {
        output[2*i] = "0123456789abcdef"[digest[i] >> 4];
        output[2*i+1] = "0123456789abcdef"[digest[i] & 15];
    }
    output[64] = 0;
}

static XrXirArtifact *read_lower(const XrXirCompileContext *context) {
    XrXirArtifact *checked = NULL, *lowered = NULL;
    XrXirDiagnostic diagnostic = {0};
    CHECK(xr_xir_compile_checked_read(context, f64_transport_packet, sizeof(f64_transport_packet),
        &checked, &diagnostic) == XR_XIR_OK);
    const XrXirModule *module = xr_xir_compile_artifact_module(checked);
    CHECK(module && module->function_count == 4 && module->declarations);
    CHECK(module->declarations->entry_function == 2 && module->declarations->modules[0].initializer == 3);
    CHECK(module->declarations->functions[0].exported == 1 && !module->declarations->functions[1].exported);
    for (unsigned i = 0; i < 2; ++i) {
        const XrXirFunction *f = &module->functions[i];
        CHECK(f->parameter_count == 1 && f->parameters[0] == XR_XIR_F64 && f->result == XR_XIR_F64);
        CHECK(f->instruction_count == 2 && f->instructions[0].op == (i ? XR_XIR_COPY : XR_XIR_CALL));
        CHECK(f->instructions[1].op == XR_XIR_RETURN && f->instructions[1].args[0] == 1);
    }
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(checked, &packet, &diagnostic) == XR_XIR_OK);
    CHECK(packet.length == sizeof(f64_transport_packet) && !memcmp(packet.bytes, f64_transport_packet, packet.length));
    xr_xir_compile_checked_packet_free(&packet);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(checked, &target, &lowered, &diagnostic) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_compile_artifact_verify(lowered, &diagnostic) == XR_XIR_OK);
    return lowered;
}

static void emit_source(XrXirArtifact *lowered, XrXirCSource *source, char digest[65]) {
    CHECK(xr_xir_compile_emit_c(lowered, "source_f64_transport", 1048576, source) == XR_XIR_OK);
    XrXirCSource repeated = {0};
    CHECK(xr_xir_compile_emit_c(lowered, "source_f64_transport", 1048576, &repeated) == XR_XIR_OK);
    CHECK(source->text != repeated.text && source->length == repeated.length);
    CHECK(!memcmp(source->text, repeated.text, source->length));
    xr_xir_compile_c_source_free(&repeated);
    hex_digest(source->text, source->length, digest);
    printf("f64-transport generated-bytes=%zu sha256=%s deterministic=1\n", source->length, digest);
}

static void execute(const XrXirCompileContext *context, XrXirArtifact *lowered, unsigned mode) {
    const XrXirCallEntry *native = native_entries();
    XrXirProgramProof proof = xr_xir_compile_program_proof(lowered);
    if (mode) CHECK(native);
    TransportOwner *owner = NULL;
    CHECK(xr_compile_resources_calloc(context->resources, 1, sizeof(*owner), (void **)&owner) == XR_COMPILE_RESOURCE_OK);
    owner->lowered = lowered;
    for (uint32_t f = 0; f < 4; ++f) {
        owner->native[f] = mode == 1 || (mode == 2 && f == 0) || (mode == 3 && f == 1);
        if (owner->native[f]) owner->actual[f] = native[f];
        else CHECK(xr_xir_compile_vm_bind(lowered, f, &owner->bindings[f], &owner->actual[f]) == XR_XIR_OK);
        if (f < 2) {
            CHECK(owner->actual[f].parameter_count == 1 && owner->actual[f].parameters[0] == XR_XIR_F64);
            CHECK(owner->actual[f].result == XR_XIR_F64);
        }
        owner->observed[f] = owner->actual[f]; owner->observed[f].resume = observed_resume;
    }
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION},
        owner->observed, 4, xr_xir_compile_artifact_module(lowered)->declarations, {owner, release_code}, NULL, proof};
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(context, &spec, &program) == XR_XIR_OK);
    active_owner = owner;
    XrXirInstanceConfig config = {0};
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    XrXirInstance *instances[2] = {NULL, NULL};
    for (unsigned i = 0; i < 2; ++i) CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
    CHECK(instances[0] != instances[1]);
    xr_xir_compile_program_drop(program);
    XrXirValue escaped[40] = {0}; size_t count = 0;
    for (unsigned i = 0; i < 2; ++i) {
        for (unsigned repeat = 0; repeat < 2; ++repeat) for (unsigned bit = 0; bit < 10; ++bit) {
            uint64_t bits = f64_transport_bits[bit]; int64_t payload;
            memcpy(&payload, &bits, sizeof(payload));
            XrXirValue input = {XR_XIR_F64, 0, payload}, saved = input;
            size_t calls = crossings;
            CHECK(xr_xir_instance_start(instances[i], 1, &input, 1) == XR_XIR_CALL_BAD_ARGUMENT);
            CHECK(xr_xir_instance_start(instances[i], 0, &input, 1) == XR_XIR_CALL_READY);
            XrXirInstanceResult result = {0}; size_t polls = 0;
            do { CHECK(++polls < 32); result = xr_xir_instance_poll_bounded(instances[i], 10000); }
            while (result.outcome.status == XR_XIR_CALL_READY);
            CHECK(result.outcome.status == XR_XIR_CALL_RETURNED && crossings == calls + 1);
            CHECK(xr_xir_instance_take_result(instances[i], &escaped[count]) == XR_XIR_CALL_RETURNED);
            CHECK(escaped[count].type == XR_XIR_F64 && !escaped[count].reserved);
            CHECK((uint64_t)escaped[count].payload == bits && !memcmp(&input, &saved, sizeof(input)));
            printf("f64-transport mode=%u instance=%u repeat=%u pattern=%u bits=%016" PRIx64 " result=PASS\n",
                mode, i, repeat, bit, bits);
            ++count;
        }
        CHECK(!releases && active_owner);
        CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
        CHECK(releases == i);
    }
    CHECK(count == 40 && releases == 1 && !active_owner && crossings == 40);
    if (mode == 0) CHECK(resumed[0] && !resumed[1]);
    else if (mode == 1) CHECK(resumed[1] && !resumed[0]);
    else CHECK(resumed[0] && resumed[1]);
    for (size_t i = 0; i < count; ++i) {
        CHECK(escaped[i].type == XR_XIR_F64 && !escaped[i].reserved);
        CHECK((uint64_t)escaped[i].payload == f64_transport_bits[i % 10]);
        XrXirValue copy = {0};
        CHECK(xr_xir_value_copy(&escaped[i], &copy) == XR_XIR_VALUE_OK);
        CHECK(!memcmp(&escaped[i], &copy, sizeof(copy)));
        xr_xir_value_drop(&copy); xr_xir_value_drop(&escaped[i]);
        CHECK(!copy.type && !copy.payload && !escaped[i].type && !escaped[i].payload);
    }
    printf("f64-transport mode=%u calls=40 retained-results=40 owners-dead=1 vm-resumes=%zu native-resumes=%zu code-release=1 result=PASS\n",
        mode, resumed[0], resumed[1]);
}

int main(int argc, char **argv) {
    CHECK(argc == 2 || argc == 3);
    bool emit = !strcmp(argv[1], "emit");
    unsigned mode = !strcmp(argv[1], "vm") ? 0 : !strcmp(argv[1], "native") ? 1 :
        !strcmp(argv[1], "native-to-vm") ? 2 : !strcmp(argv[1], "vm-to-native") ? 3 : 4;
    CHECK((emit && argc == 3) || (!emit && argc == 2 && mode < 4));
    XrXirCompileContext context = {0}; context.limits = xr_xir_compile_default_limits();
    XrCompileResourceLimits limits = {67108864, 8388608, 128000000};
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    XrXirArtifact *lowered = read_lower(&context);
    XrXirCSource source = {0}; char digest[65]; emit_source(lowered, &source, digest);
    if (emit) {
        FILE *file = fopen(argv[2], "wb"); CHECK(file);
        CHECK(fwrite(source.text, 1, source.length, file) == source.length);
        CHECK(fprintf(file, "\nconst char source_f64_transport_sha[65]=\"%s\";\n", digest) > 0);
        CHECK(fclose(file) == 0); xr_xir_compile_artifact_free(lowered);
    } else {
#if defined(XR_F64_NATIVE)
        if (mode) CHECK(!strcmp(digest, source_f64_transport_sha));
#endif
        execute(&context, lowered, mode);
    }
    xr_xir_compile_c_source_free(&source);
    XrCompileResourceStats stats = {0};
    CHECK(xr_compile_resources_stats(context.resources, &stats) == XR_COMPILE_RESOURCE_OK);
    printf("f64-transport allocated=%" PRIu64 " peak=%" PRIu64 " work=%" PRIu64 "\n",
        stats.allocated_bytes, stats.peak_bytes, stats.work);
    xr_compile_resources_release(context.resources);
    instance_compile_report();
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    puts("f64-transport release compiler=0/0 runtime=0/0 table=0 result=PASS");
    return 0;
}
