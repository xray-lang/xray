/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_checked_allocations.c - Packet allocation rollback and physical release
 *
 * KEY CONCEPT:
 *   Fail every allocation in the actual reader, writer and semantic verifier.
 */
#include "base/xmalloc.h"
#include "xir/xxir_checked.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
static size_t calls, live, fail_at = SIZE_MAX, live_bytes, peak_bytes;
static struct { void *pointer; size_t bytes; } packet_owned[16384];
static void packet_record(void *pointer, size_t bytes) {
    if (!pointer) return;
    CHECK(live < 16384 && bytes <= SIZE_MAX - live_bytes);
    packet_owned[live].pointer = pointer;
    packet_owned[live++].bytes = bytes;
    live_bytes += bytes;
    if (live_bytes > peak_bytes) peak_bytes = live_bytes;
}
static void *packet_malloc(size_t size) {
    if (calls++ == fail_at) return NULL;
    void *p = xr_malloc(size); packet_record(p, size); return p;
}
static void *packet_calloc(size_t count, size_t size) {
    if (calls++ == fail_at) return NULL;
    CHECK(!size || count <= SIZE_MAX / size);
    void *p = xr_calloc(count, size); packet_record(p, count * size); return p;
}
static void packet_free(void *p) {
    if (p) {
        size_t index = 0;
        while (index < live && packet_owned[index].pointer != p) ++index;
        CHECK(index < live && packet_owned[index].bytes <= live_bytes);
        live_bytes -= packet_owned[index].bytes;
        packet_owned[index] = packet_owned[--live];
    }
    xr_free(p);
}
#undef xr_malloc
#undef xr_calloc
#undef xr_free
#define xr_malloc(size) packet_malloc(size)
#define xr_calloc(count, size) packet_calloc(count, size)
#define xr_free(p) packet_free(p)
#include "xir/xxir_types.c"
#include "xir/xxir_type_layout.c"
#include "xir/xxir_generic.c"
#include "xir/xxir.c"
#include "xir/xxir_declarations.c"
#include "xir/xxir_verify.c"
#include "xir/xxir_layout.c"
#include "xir/xxir_checked.c"
#include "xir/xxir_specialize.c"
#include "xir/xxir_program_match.c"
#include "xir_checked_fixture.h"
#include "xir_generic_fixture.h"
#include "xir_local_fixture.h"
#include "xir_types_fixture.h"
#include "xir_array_metadata_fixture.h"
#include "xir_array_generic_fixture.h"
#include "xir_nominal_checked_fixture.h"
#include "xir_nominal_generic_fixture.h"
#include "xir_nominal_expression_fixture.h"
#include "xir_nominal_field_closure_fixture.h"
#include "xir_nominal_chain_fixture.h"
#include "xir_struct_ops_fixture.h"
#include "xir_struct_set_fixture.h"
static XrXirArtifact *array_packet_fixture(void) {
    XirArrayMetadataFixture f; xir_array_metadata_init(&f);
    XrXirArtifact *checked = NULL;
    CHECK(xr_xir_check(&f.module, NULL, &checked, NULL) == XR_XIR_OK);
    return checked;
}
static void packet_failures(unsigned kind) {
    XrXirArtifact *checked = kind >= 17 ? nominal_expression_fixture() : kind == 16 ? struct_set_checked(0) : kind == 15 ? struct_ops_checked(0) : kind == 14 ? nominal_chain_fixture(3, 2) : kind >= 9 ? nominal_checked_fixture(kind >= 12 ? 3 : kind == 11 ? 2 : kind == 10 ? 1 : 0) : kind == 8 ? array_generic_fixture() : kind == 7 ? array_packet_fixture() :
        kind == 6 ? constructed_fixture() : kind == 5 ? generic_callable_fixture() : kind == 4 ? function_ir_fixture() : kind == 3 ? callable_fixture() : kind == 2 ? local_fixture() : kind == 1 ? generic_fixture() : checked_fixture();
    if (kind == 13 || kind == 18) {
        XrXirArtifact *closed = NULL;
        CHECK(xr_xir_specialize(checked, NULL, &closed, NULL) == XR_XIR_OK);
        xr_xir_artifact_free(checked); checked = closed;
    }
    size_t baseline = live;
    XrXirCheckedPacket packet = {0};
    calls = 0;
    CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OK);
    size_t write_sites = calls;
    xr_xir_checked_packet_free(&packet); CHECK(live == baseline);
    for (size_t i = 0; i < write_sites; ++i) {
        calls = 0; fail_at = i;
        CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!packet.bytes && !packet.length && live == baseline);
    }
    fail_at = SIZE_MAX;
    CHECK(xr_xir_checked_write(checked, NULL, &packet, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked); CHECK(live == 1);
    XrXirArtifact *decoded = NULL;
    calls = 0;
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OK);
    size_t read_sites = calls;
    xr_xir_artifact_free(decoded); CHECK(live == 1);
    for (size_t i = 0; i < read_sites; ++i) {
        calls = 0; fail_at = i;
        CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!decoded && live == 1);
    }
    fail_at = SIZE_MAX;
    /* Exercise partial metadata teardown after valid integrity checks too. */
    for (size_t i = 64; i < packet.length; ++i) {
        packet.bytes[i] ^= 0xFF;
        checked_digest(packet.bytes, packet.length, packet.bytes + 32);
        XrXirStatus status = xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL);
        CHECK((status == XR_XIR_OK) == (decoded != NULL));
        xr_xir_artifact_free(decoded); CHECK(live == 1);
        packet.bytes[i] ^= 0xFF;
    }
    xr_xir_checked_packet_free(&packet); CHECK(!live);
    printf("%s packet physical release: %zu writer and %zu reader allocation sites\n",
        kind == 18 ? "Specialized nominal expressions" : kind == 17 ? "Abstract nominal expressions" : kind == 14 ? "Nominal field graph" : kind == 13 ? "Closed nominal fields" : kind == 12 ? "Nominal field expression" : kind == 11 ? "Nominal instance" : kind == 10 ? "Nominal Array field" : kind == 9 ? "Nominal declaration" : kind == 8 ? "Generic Array" : kind == 7 ? "Array operations" : kind == 6 ? "Constructed" : kind == 5 ? "Generic callable" : kind == 4 ? "Function" : kind == 3 ? "Callable" : kind == 2 ? "Local" : kind == 1 ? "Generic" : "Closed", write_sites, read_sites);
}

static void specialization_failures(unsigned callable) {
    XrXirArtifact *checked = callable == 8 ? nominal_expression_fixture() : callable == 7 ? nominal_chain_fixture(3, 2) : callable == 6 ? nominal_generic_fixture(true) : callable == 5 ? nominal_checked_fixture(3) : callable == 4 ? array_generic_fixture() : callable == 3 ? constructed_fixture() : callable == 2 ? generic_callable_fixture() : callable ? callable_fixture() : generic_fixture(), *output = NULL;
    XrXirModule built = *xr_xir_artifact_module(checked); built.stage = XR_XIR_BUILT;
    size_t baseline = live, sites[2] = {0};
    for (unsigned mode = 0; mode < 2; ++mode) {
        for (size_t attempt = 0; attempt <= sites[mode]; ++attempt) {
            calls = 0; fail_at = attempt ? attempt - 1 : SIZE_MAX;
            XrXirStatus status = mode ? xr_xir_specialize(checked, NULL, &output, NULL) :
                xr_xir_check(&built, NULL, &output, NULL);
            if (!attempt) { CHECK(status == XR_XIR_OK && output); sites[mode] = calls; }
            else CHECK(status == XR_XIR_OUT_OF_MEMORY && !output);
            xr_xir_artifact_free(output); CHECK(live == baseline);
        }
    }
    fail_at = SIZE_MAX; xr_xir_artifact_free(checked); CHECK(!live);
    printf("%s physical release: %zu checking and %zu specialization allocation sites\n", callable == 8 ? "Nominal expressions" : callable == 7 ? "Nominal field graph" : callable == 6 ? "Combined nominal and function" : callable == 5 ? "Nominal field substitution" : callable == 4 ? "Generic Array" : callable == 3 ? "Constructed" : callable == 2 ? "Generic callable" : callable ? "Callable" : "Generic", sites[0], sites[1]);
}
static void nominal_field_closure_failures(void) {
    for (unsigned mode = 0; mode < 3; ++mode) {
        XrXirArtifact *checked = nominal_field_closure_fixture(mode), *output = NULL;
        XrXirBudget budget = xr_xir_default_budget();
        if (mode == 2) budget.work = 10000;
        size_t baseline = live; calls = 0; fail_at = SIZE_MAX;
        XrXirStatus expected = !mode ? XR_XIR_OK : mode == 1 ? XR_XIR_BAD_TYPE : XR_XIR_BUDGET;
        CHECK(xr_xir_specialize(checked,&budget,&output,NULL) == expected);
        size_t sites = calls;
        if (mode) CHECK(!output);
        xr_xir_artifact_free(output); output = NULL; CHECK(live == baseline);
        for (size_t attempt = 0; attempt < sites; ++attempt) {
            calls = 0; fail_at = attempt;
            CHECK(xr_xir_specialize(checked,&budget,&output,NULL) == XR_XIR_OUT_OF_MEMORY && !output);
            CHECK(live == baseline);
        }
        fail_at = SIZE_MAX; xr_xir_artifact_free(checked); CHECK(!live);
        printf("Nominal field closure mode %u: %zu failure sites, no retained metadata\n",mode,sites);
    }
}
static void nominal_lowering_failures(void) {
    XrXirArtifact *checked = nominal_checked_fixture(3), *closed = NULL, *lowered = NULL;
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_specialize(checked, NULL, &closed, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    size_t baseline = live; calls = 0; fail_at = SIZE_MAX;
    CHECK(xr_xir_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    size_t sites = calls; xr_xir_artifact_free(lowered); CHECK(live == baseline);
    for (size_t i = 0; i < sites; ++i) {
        calls = 0; fail_at = i;
        CHECK(xr_xir_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!lowered && live == baseline);
    }
    fail_at = SIZE_MAX; xr_xir_artifact_free(closed); CHECK(!live);
    printf("Nominal lowering: %zu allocation sites physically released\n", sites);
}

static void nominal_layout_failures(void) {
    XrXirArtifact *checked = nominal_chain_fixture(3, 2), *closed = NULL;
    CHECK(xr_xir_specialize(checked, NULL, &closed, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    const XrXirTypes *types = xr_xir_artifact_module(closed)->types;
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    XrXirBudget original = xr_xir_default_budget(), budget = original;
    XrXirLayout layout = {0}; uint32_t offsets[] = {99, 99};
    size_t baseline = live; calls = 0;
    CHECK(xr_xir_nominal_layout(types, (XrXirType)256, &target, &budget, &layout, offsets, 2) == XR_XIR_OK);
    CHECK(layout.size == 64 && layout.alignment == 8 && offsets[0] == 0 && offsets[1] == 32 && live == baseline);
    size_t sites = calls;
    uint64_t work = original.work - budget.work;
    for (size_t i = 0; i < sites; ++i) {
        calls = 0; fail_at = i; budget = original; offsets[0] = offsets[1] = 99;
        CHECK(xr_xir_nominal_layout(types, (XrXirType)256, &target, &budget, &layout, offsets, 2) == XR_XIR_OUT_OF_MEMORY);
        CHECK(live == baseline && !layout.size && !layout.alignment && offsets[0] == 99 && offsets[1] == 99 &&
            !memcmp(&budget, &original, sizeof(budget)));
    }
    fail_at = SIZE_MAX; budget = original; budget.work = work - 1;
    XrXirBudget short_budget = budget;
    CHECK(xr_xir_nominal_layout(types, (XrXirType)256, &target, &budget, &layout, offsets, 2) == XR_XIR_BUDGET);
    CHECK(live == baseline && !layout.size && !layout.alignment && offsets[0] == 99 && offsets[1] == 99 &&
        !memcmp(&budget, &short_budget, sizeof(budget)));
    xr_xir_artifact_free(closed); CHECK(!live);
}
static void provenance_ownership(void) {
    XrXirArtifact *source = generic_fixture();
    XrXirArtifact *closed = NULL;
    CHECK(xr_xir_specialize(source, NULL, &closed, NULL) == XR_XIR_OK);
    XrXirType integer = XR_XIR_I64, string = XR_XIR_STRING;
    XrXirOrigin origins[] = {{0,NULL,0}, {1,&integer,1}, {1,&string,1}};
    XrXirBudget initial = xr_xir_default_budget(), remaining = initial;
    XrXirProvenance *copy = NULL;
    size_t baseline = live; calls = 0;
    CHECK(xr_xir_provenance_copy(&source->module, origins, 3, &remaining, &copy) == XR_XIR_OK);
    size_t sites = calls;
    uint64_t bytes = initial.metadata_bytes - remaining.metadata_bytes, work = initial.work - remaining.work;
    CHECK(copy && copy->count == 3 && copy->source != source);
    CHECK(copy->origins != origins && copy->origins[1].arguments != &integer);
    CHECK(copy->source->module.functions != source->module.functions);
    xr_xir_provenance_free(copy); CHECK(live == baseline);
    for (size_t i = 0; i < sites; ++i) {
        calls = 0; fail_at = i; remaining = initial; copy = NULL;
        CHECK(xr_xir_provenance_copy(&source->module, origins, 3, &remaining, &copy) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!copy && live == baseline);
    }
    fail_at = SIZE_MAX;
    for (unsigned mode = 0; mode < 4; ++mode) {
        remaining = initial; remaining.functions = 2;
        remaining.metadata_bytes = bytes; remaining.work = work;
        if (mode == 1) --remaining.functions;
        if (mode == 2) --remaining.metadata_bytes;
        if (mode == 3) --remaining.work;
        CHECK(xr_xir_provenance_copy(&source->module, origins, 3, &remaining, &copy) ==
              (mode ? XR_XIR_BUDGET : XR_XIR_OK));
        if (!mode) CHECK(!remaining.functions && !remaining.metadata_bytes && !remaining.work);
        else CHECK(!copy);
        xr_xir_provenance_free(copy); CHECK(live == baseline);
    }
    for (unsigned mode = 0; mode < 3; ++mode) {
        XrXirOrigin saved = origins[1]; remaining = initial;
        if (mode == 0) origins[1].function = 2;
        if (mode == 1) origins[1].argument_count = 0;
        if (mode == 2) origins[1].arguments = NULL;
        CHECK(xr_xir_provenance_copy(&source->module, origins, 3, &remaining, &copy) == XR_XIR_BAD_STRUCTURE);
        CHECK(!copy && live == baseline); origins[1] = saved;
    }
    remaining = initial;
    CHECK(xr_xir_provenance_copy(&source->module, origins, 3, &remaining, &copy) == XR_XIR_OK);
    integer = XR_XIR_BOOL; string = XR_XIR_I64;
    memset(origins, 0xa5, sizeof(origins)); xr_xir_artifact_free(source);
    CHECK(copy->origins[1].arguments[0] == XR_XIR_I64 && copy->origins[2].arguments[0] == XR_XIR_STRING);
    CHECK(xr_xir_artifact_verify(copy->source, NULL, NULL) == XR_XIR_OK);
    remaining = initial;
    CHECK(xr_xir_provenance_functions_match(&copy->source->module, &closed->module,
        copy->origins, &remaining, NULL) == XR_XIR_OK);
    const XrXirType *saved_arguments = copy->origins[1].arguments;
    copy->origins[1].arguments = NULL; remaining = initial;
    CHECK(xr_xir_provenance_functions_match(&copy->source->module, &closed->module,
        copy->origins, &remaining, NULL) == XR_XIR_BAD_STRUCTURE);
    copy->origins[1].arguments = saved_arguments;
    xr_xir_artifact_free(closed);
    xr_xir_provenance_free(copy); CHECK(!live);
    printf("provenance ownership: %zu allocation failures released\n", sites);
}

static void provenance_packet_roundtrip(XrXirArtifact *closed) {
    size_t baseline = live;
    XrXirCheckedPacket packet = {0}; XrXirArtifact *decoded = NULL;
    calls = 0;
    CHECK(xr_xir_checked_write(closed, NULL, &packet, NULL) == XR_XIR_OK);
    size_t writes = calls;
    xr_xir_checked_packet_free(&packet);
    for (size_t i = 0; i < writes; ++i) {
        calls = 0; fail_at = i;
        CHECK(xr_xir_checked_write(closed, NULL, &packet, NULL) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!packet.bytes && live == baseline);
    }
    fail_at = SIZE_MAX;
    CHECK(xr_xir_checked_write(closed, NULL, &packet, NULL) == XR_XIR_OK);
    calls = 0;
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OK);
    size_t reads = calls;
    CHECK(decoded->module.provenance && decoded->module.provenance->count == 3);
    xr_xir_artifact_free(decoded);
    for (size_t i = 0; i < reads; ++i) {
        calls = 0; fail_at = i;
        CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!decoded && live == baseline + 1);
    }
    fail_at = SIZE_MAX;
    XrXirArtifact plain = *closed; plain.module.provenance = NULL;
    XrXirCheckedPacket plain_packet = {0}, source_packet = {0};
    CHECK(xr_xir_checked_write(&plain, NULL, &plain_packet, NULL) == XR_XIR_OK);
    CHECK(xr_xir_checked_write(closed->module.provenance->source, NULL, &source_packet, NULL) == XR_XIR_OK);
    size_t nested_presence = plain_packet.length + source_packet.length - 64 - 4;
    xr_xir_checked_packet_free(&plain_packet); xr_xir_checked_packet_free(&source_packet);
    CHECK(nested_presence + 4 < packet.length && packet.bytes[nested_presence] == 0);
    packet.bytes[nested_presence] = 1; checked_digest(packet.bytes, packet.length, packet.bytes + 32);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(!decoded && live == baseline + 1);
    packet.bytes[nested_presence] = 0;
    for (size_t i = 64; i < packet.length; ++i) {
        packet.bytes[i] ^= 0xff; checked_digest(packet.bytes, packet.length, packet.bytes + 32);
        XrXirStatus status = xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL);
        CHECK((status == XR_XIR_OK) == (decoded != NULL));
        xr_xir_artifact_free(decoded); CHECK(live == baseline + 1);
        packet.bytes[i] ^= 0xff;
    }
    checked_digest(packet.bytes, packet.length, packet.bytes + 32);
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &decoded, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xa5, packet.length); xr_xir_checked_packet_free(&packet);
    CHECK(xr_xir_artifact_verify(decoded, NULL, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(decoded); CHECK(live == baseline);
    printf("provenance packet: %zu write and %zu read allocation failures released\n", writes, reads);
}

static void native_packet_proof(void) {
    XrXirArtifact *artifact = nominal_expression_lowered();
    const XrXirModule *module = xr_xir_artifact_module(artifact);
    CHECK(module->function_count == 9);
    XrXirCallEntry entries[9] = {0};
    for (uint32_t i = 0; i < 9; ++i) {
        entries[i].parameter_count = module->functions[i].parameter_count;
        entries[i].parameters = module->functions[i].parameters;
        entries[i].result = module->functions[i].result;
    }
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, *xr_xir_artifact_target(artifact),
        entries, 9, module->declarations, {0}, module->types, xr_xir_program_proof(artifact)};
    XrXirProgramProof proof = {artifact->checked_packet.bytes, artifact->checked_packet.length,
        artifact->checked_identity, artifact->layouts};
    XrXirBudget limits = xr_xir_default_budget();
    size_t baseline = live, baseline_bytes = live_bytes;
    calls = 0; peak_bytes = live_bytes; uint64_t work = 16000000;
    CHECK(xr_xir_program_proof_verify(&spec, &proof, &limits, &limits, UINT64_MAX, &work) == XR_XIR_OK);
    size_t sites = calls, successful_peak = peak_bytes - baseline_bytes;
    CHECK(live == baseline && live_bytes == baseline_bytes);
    for (size_t i = 0; i < sites; ++i) {
        calls = 0; fail_at = i; work = 16000000;
        CHECK(xr_xir_program_proof_verify(&spec, &proof, &limits, &limits, UINT64_MAX, &work) == XR_XIR_OUT_OF_MEMORY);
        CHECK(live == baseline && live_bytes == baseline_bytes);
    }
    fail_at = SIZE_MAX;
    for (uint32_t attack = 0; attack < 5; ++attack) {
        XrXirBudget decode = limits, lower = limits; work = 16000000;
        if (attack == 0) artifact->checked_packet.bytes[0] ^= 1;
        if (attack == 1) artifact->checked_identity[0] ^= 1;
        if (attack == 2) decode.metadata_bytes = 1;
        if (attack == 3) lower.metadata_bytes = 1;
        if (attack == 4) entries[0].result = XR_XIR_BOOL;
        XrXirStatus expected = attack == 2 || attack == 3 ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;
        CHECK(xr_xir_program_proof_verify(&spec, &proof, &decode, &lower, UINT64_MAX, &work) == expected);
        CHECK(live == baseline && live_bytes == baseline_bytes);
        if (attack == 0) artifact->checked_packet.bytes[0] ^= 1;
        if (attack == 1) artifact->checked_identity[0] ^= 1;
        entries[0].result = module->functions[0].result;
    }
    for (uint64_t cap = 16384; cap <= 131072; cap *= 2) {
        XrXirBudget phase = limits; phase.metadata_bytes = cap; phase.scratch_bytes = cap;
        work = 16000000; peak_bytes = live_bytes;
        XrXirStatus status = xr_xir_program_proof_verify(&spec, &proof, &phase, &phase, cap * 7, &work);
        CHECK(status == XR_XIR_OK || status == XR_XIR_BUDGET);
        CHECK(live == baseline && live_bytes == baseline_bytes);
        CHECK(peak_bytes - baseline_bytes <= cap * 7);
        calls = 0; work = 16000000;
        CHECK(xr_xir_program_proof_verify(&spec, &proof, &phase, &phase, cap * 7 - 1, &work) == XR_XIR_BUDGET);
        CHECK(!calls && work == 16000000 && live == baseline && live_bytes == baseline_bytes);
        printf("native proof phase cap %llu: status %u, peak requested bytes %zu\n",
            (unsigned long long)cap, (unsigned)status, peak_bytes - baseline_bytes);
    }
    XrXirBudget overflowing = limits; overflowing.metadata_bytes = UINT64_MAX;
    calls = 0; work = 16000000;
    CHECK(xr_xir_program_proof_verify(&spec, &proof, &overflowing, &limits, UINT64_MAX, &work) == XR_XIR_BUDGET);
    CHECK(!calls && work == 16000000);
    work = proof.length - 1;
    CHECK(xr_xir_program_proof_verify(&spec, &proof, &limits, &limits, UINT64_MAX, &work) == XR_XIR_BUDGET);
    CHECK(live == baseline && live_bytes == baseline_bytes); xr_xir_artifact_free(artifact); CHECK(!live && !live_bytes);
    printf("native packet proof: %zu allocation failures released; peak requested bytes %zu\n",
        sites, successful_peak);
}

static void provenance_lowering(XrXirArtifact *closed) {
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    XrXirArtifact *lowered = NULL;
    size_t baseline = live; calls = 0;
    CHECK(xr_xir_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    size_t sites = calls;
    CHECK(lowered->checked_packet.bytes && lowered->checked_packet.length >= 64);
    XrXirCheckedPacket expected = {0};
    CHECK(xr_xir_checked_write(closed, NULL, &expected, NULL) == XR_XIR_OK);
    CHECK(expected.length == lowered->checked_packet.length);
    CHECK(!memcmp(expected.bytes, lowered->checked_packet.bytes, expected.length));
    xr_xir_checked_packet_free(&expected);
    lowered->checked_packet.bytes[0] ^= 1;
    CHECK(xr_xir_artifact_verify(lowered, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    lowered->checked_packet.bytes[0] ^= 1;
    lowered->checked_identity[0] ^= 1;
    CHECK(xr_xir_artifact_verify(lowered, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    lowered->checked_identity[0] ^= 1;
    XrXirBudget tight = xr_xir_default_budget();
    tight.metadata_bytes = lowered->checked_packet.length - 1;
    CHECK(xr_xir_artifact_verify(lowered, &tight, NULL) == XR_XIR_BUDGET);
    tight = xr_xir_default_budget(); tight.work = lowered->checked_packet.length - 1;
    CHECK(xr_xir_artifact_verify(lowered, &tight, NULL) == XR_XIR_BUDGET);
    CHECK(lowered->module.provenance && lowered->module.provenance != closed->module.provenance);
    CHECK(lowered->module.provenance->source->module.stage == XR_XIR_CHECKED);
    if (lowered->module.types && lowered->module.types->nominals) {
        CHECK(lowered->module.types->nominals->identities && !lowered->module.types->nominals->declarations);
        XrXirTypeNode *node = (XrXirTypeNode *)&lowered->module.types->nodes[0];
        CHECK(node->kind == XR_XIR_TYPE_NOMINAL && node->nominal.field_count);
        XrXirType *field = (XrXirType *)node->nominal.fields, saved = *field;
        *field = saved == XR_XIR_I64 ? XR_XIR_U8 : XR_XIR_I64;
        CHECK(xr_xir_artifact_verify(lowered, NULL, NULL) != XR_XIR_OK);
        *field = saved;
    }
    CHECK(xr_xir_artifact_verify(lowered, NULL, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(lowered); CHECK(live == baseline);
    for (size_t i = 0; i < sites; ++i) {
        calls = 0; fail_at = i; lowered = NULL;
        CHECK(xr_xir_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!lowered && live == baseline);
    }
    fail_at = SIZE_MAX;
    printf("provenance lowering: %zu allocation failures released\n", sites);
}

static void provenance_artifact_ownership(void) {
    XrXirArtifact *source = generic_fixture(), *closed = NULL, *copy = NULL;
    CHECK(xr_xir_specialize(source, NULL, &closed, NULL) == XR_XIR_OK);
    XrXirType integer = XR_XIR_I64, string = XR_XIR_STRING;
    XrXirOrigin origins[] = {{0,NULL,0}, {1,&integer,1}, {1,&string,1}};
    XrXirBudget remaining = xr_xir_default_budget();
    XrXirProvenance *proof = NULL;
    CHECK(xr_xir_provenance_copy(&source->module, origins, 3, &remaining, &proof) == XR_XIR_OK);
    xr_xir_provenance_free((XrXirProvenance *)closed->module.provenance);
    closed->module.provenance = proof;
    xr_xir_artifact_free(source);
    CHECK(xr_xir_artifact_verify(closed, NULL, NULL) == XR_XIR_OK);
    size_t baseline = live; calls = 0;
    CHECK(xr_xir_recheck(&closed->module, NULL, &copy, NULL) == XR_XIR_OK);
    size_t sites = calls;
    CHECK(copy->module.provenance != proof && copy->module.provenance->source != proof->source);
    xr_xir_artifact_free(copy); CHECK(live == baseline);
    for (size_t i = 0; i < sites; ++i) {
        calls = 0; fail_at = i; copy = NULL;
        CHECK(xr_xir_recheck(&closed->module, NULL, &copy, NULL) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!copy && live == baseline);
    }
    fail_at = SIZE_MAX;
    remaining = xr_xir_default_budget(); remaining.functions = 4;
    CHECK(xr_xir_artifact_verify(closed, &remaining, NULL) == XR_XIR_BUDGET);
    remaining.functions = 5;
    CHECK(xr_xir_artifact_verify(closed, &remaining, NULL) == XR_XIR_OK);
    proof->source->module.provenance = proof;
    CHECK(xr_xir_artifact_verify(closed, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    proof->source->module.provenance = NULL;
    --proof->count;
    CHECK(xr_xir_artifact_verify(closed, NULL, NULL) == XR_XIR_BAD_STRUCTURE);
    ++proof->count;
    provenance_packet_roundtrip(closed);
    provenance_lowering(closed);
    CHECK(xr_xir_specialize(closed, NULL, &copy, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(closed);
    CHECK(xr_xir_artifact_verify(copy, NULL, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(copy); CHECK(!live);
    printf("provenance artifact: %zu allocation failures released\n", sites);
}

static void provenance_nominal_attacks(void) {
    XrXirArtifact *source = nominal_expression_fixture(), *closed = NULL;
    CHECK(xr_xir_specialize(source, NULL, &closed, NULL) == XR_XIR_OK);
    CHECK(closed->module.function_count == 6);
    XrXirType arguments[] = {XR_XIR_I64, XR_XIR_U8, XR_XIR_STRING};
    XrXirOrigin origins[] = {{0,NULL,0}, {2,NULL,0}, {3,NULL,0},
        {1,arguments,1}, {1,arguments+1,1}, {1,arguments+2,1}};
    XrXirNominalTable *table = (XrXirNominalTable *)closed->module.types->nominals;
    XrXirNominalDeclaration *d = (XrXirNominalDeclaration *)&table->declarations[0];
    XrXirNominalField *field = (XrXirNominalField *)&d->fields[0];
    char *module = (char *)d->module.bytes, *name = (char *)d->name.bytes;
    char *field_name = (char *)field->name.bytes;
    char *function_name = (char *)closed->module.functions[3].name;
    uint32_t *constraint = (uint32_t *)d->constraints;
    size_t baseline = live;
    for (unsigned attack = 0; attack < 10; ++attack) {
        XrXirNominalDeclaration saved = *d; XrXirNominalField saved_field = *field;
        char old_module = *module, old_name = *name, old_field = *field_name, old_function = *function_name;
        uint32_t old_constraint = *constraint, old_count = table->count;
        if (attack == 1) *module ^= 1;
        if (attack == 2) *name ^= 1;
        if (attack == 3) d->exported ^= 1;
        if (attack == 4) *constraint ^= XR_XIR_CONSTRAINT_SENDABLE;
        if (attack == 5) *field_name ^= 1;
        if (attack == 6) field->flags ^= XR_XIR_FIELD_PRIVATE;
        if (attack == 7) field->type = XR_XIR_I64;
        if (attack == 8) --table->count;
        if (attack == 9) *function_name ^= 1;
        XrXirBudget remaining = xr_xir_default_budget();
        CHECK(xr_xir_provenance_functions_match(&source->module, &closed->module, origins, &remaining, NULL) ==
            (!attack ? XR_XIR_OK : attack == 7 ? XR_XIR_BAD_TYPE : XR_XIR_BAD_STRUCTURE));
        CHECK(live == baseline);
        *module = old_module; *name = old_name; *field_name = old_field; *function_name = old_function;
        *constraint = old_constraint; table->count = old_count; *d = saved; *field = saved_field;
    }
    XrXirBudget remaining = xr_xir_default_budget();
    XrXirProvenance *proof = NULL;
    CHECK(xr_xir_provenance_copy(&source->module, origins, 6, &remaining, &proof) == XR_XIR_OK);
    xr_xir_provenance_free((XrXirProvenance *)closed->module.provenance);
    closed->module.provenance = proof;
    provenance_lowering(closed);
    xr_xir_artifact_free(closed); xr_xir_artifact_free(source); CHECK(!live);
}

static void provenance_declaration_attacks(void) {
    XrXirArtifact *source = checked_fixture(), *closed = NULL;
    CHECK(xr_xir_specialize(source, NULL, &closed, NULL) == XR_XIR_OK);
    CHECK(closed->module.function_count == 9);
    XrXirOrigin origins[9];
    for (uint32_t i = 0; i < 9; ++i) origins[i] = (XrXirOrigin) {i,NULL,0};
    XrXirDeclarations *d = (XrXirDeclarations *)closed->module.declarations;
    CHECK(d->module_count > 1 && d->slot_count && d->literal_count);
    uint32_t dependency_module = 0;
    while (dependency_module < d->module_count && !d->modules[dependency_module].dependency_count) ++dependency_module;
    CHECK(dependency_module < d->module_count);
    XrXirSourceModule *module = (XrXirSourceModule *)&d->modules[dependency_module];
    XrXirSlot *slot = (XrXirSlot *)&d->slots[0];
    XrXirLiteral *literal = (XrXirLiteral *)&d->literals[0];
    CHECK(module->name_length && literal->length);
    size_t baseline = live;
    for (unsigned attack = 0; attack < 10; ++attack) {
        XrXirDeclarations saved_d = *d;
        XrXirSourceModule saved_m = *module;
        XrXirSlot saved_s = *slot;
        char *name = (char *)module->name, *bytes = (char *)literal->bytes;
        uint32_t *dependency = (uint32_t *)module->dependencies, saved_dependency = *dependency;
        char saved_name = name[0], saved_byte = bytes[0];
        if (attack == 1) name[0] ^= 1;
        if (attack == 2) *dependency = (*dependency + 1) % d->module_count;
        if (attack == 3) module->initializer = (module->initializer + 1) % 9;
        if (attack == 4) d->entry_function = (d->entry_function + 1) % 9;
        if (attack == 5) bytes[0] ^= 1;
        if (attack == 6) slot->module = (slot->module + 1) % d->module_count;
        if (attack == 7) slot->mutable ^= 1;
        if (attack == 8) slot->type = slot->type == XR_XIR_I64 ? XR_XIR_BOOL : XR_XIR_I64;
        if (attack == 9) d->root_module = (d->root_module + 1) % d->module_count;
        XrXirBudget remaining = xr_xir_default_budget();
        CHECK(xr_xir_provenance_functions_match(&source->module, &closed->module, origins, &remaining, NULL) ==
            (!attack ? XR_XIR_OK : attack == 8 ? XR_XIR_BAD_TYPE : XR_XIR_BAD_STRUCTURE));
        CHECK(live == baseline);
        name[0] = saved_name; bytes[0] = saved_byte; *dependency = saved_dependency;
        *d = saved_d; *module = saved_m; *slot = saved_s;
    }
    xr_xir_artifact_free(closed); xr_xir_artifact_free(source); CHECK(!live);
}

static void provenance_closure_attacks(void) {
    XrXirArtifact *source = generic_fixture(), *closed = NULL;
    CHECK(xr_xir_specialize(source, NULL, &closed, NULL) == XR_XIR_OK);
    XrXirType integer = XR_XIR_I64, string = XR_XIR_STRING, boolean = XR_XIR_BOOL;
    XrXirFunction functions[4];
    memcpy(functions, closed->module.functions, 3 * sizeof(*functions));
    XrXirOrigin origins[] = {{0,NULL,0}, {1,&integer,1}, {1,&string,1}, {1,&boolean,1}};
    XrXirModule destination = closed->module; destination.functions = functions;
    XrXirInstruction orphan[2];
    char orphan_name[32];
    CHECK(snprintf(orphan_name, sizeof(orphan_name), "id$1:%u", (unsigned)boolean) > 0);
    memcpy(orphan, functions[1].instructions, sizeof(orphan)); orphan[0].type = XR_XIR_BOOL;
    size_t baseline = live;
    for (unsigned attack = 0; attack < 5; ++attack) {
        destination.functions = functions; destination.function_count = attack ? 4 : 3;
        functions[3] = functions[1]; origins[3] = origins[1];
        if (attack == 1) { functions[3] = functions[0]; origins[3] = origins[0]; }
        if (attack == 3) {
            origins[3].arguments = &boolean; functions[3].parameters = &boolean;
            functions[3].result = XR_XIR_BOOL; functions[3].instructions = orphan;
            functions[3].name = orphan_name; functions[3].name_length = (uint32_t)strlen(orphan_name);
        }
        if (attack == 4) { destination.functions = functions + 1; destination.function_count = 1; }
        XrXirBudget remaining = xr_xir_default_budget();
        CHECK(xr_xir_provenance_functions_match(&source->module, &destination,
            attack == 4 ? origins + 1 : origins, &remaining, NULL) == (attack ? XR_XIR_BAD_STRUCTURE : XR_XIR_OK));
        CHECK(live == baseline);
    }
    xr_xir_artifact_free(closed); xr_xir_artifact_free(source); CHECK(!live);
}

static void specialization_correspondence_attacks(void) {
    XrXirArtifact *source = generic_fixture(), *closed = NULL;
    CHECK(xr_xir_specialize(source,NULL,&closed,NULL) == XR_XIR_OK);
    const XrXirModule *module = xr_xir_artifact_module(closed);
    CHECK(module->function_count == 3);
    XrXirType integer = XR_XIR_I64, string = XR_XIR_STRING;
    XrXirOrigin origins[] = {{0,NULL,0}, {1,&integer,1}, {1,&string,1}};
    XrXirFunction *functions = (XrXirFunction *)module->functions;
    for (unsigned attack = 0; attack < 7; ++attack) {
        XrXirInstruction *op = (XrXirInstruction *)functions[0].instructions;
        XrXirInstruction saved = *op;
        uint32_t *operand = (uint32_t *)functions[0].operands, old_operand = *operand;
        XrXirBlock *block = (XrXirBlock *)functions[0].blocks; XrXirBlock old_block = *block;
        if (attack == 1) op->op = XR_XIR_CONST_INT;
        if (attack == 2) op->immediate = 2;
        if (attack == 3) ++*operand;
        if (attack == 4) ++block->first;
        if (attack == 5) op->targets[1] = 1;
        if (attack == 6) origins[1].function = 0;
        XrXirBudget remaining = xr_xir_default_budget();
        XrXirStatus status = xr_xir_provenance_functions_match(&source->module, module, origins, &remaining, NULL);
        CHECK(status == (!attack ? XR_XIR_OK : attack == 2 ? XR_XIR_BAD_TYPE : XR_XIR_BAD_STRUCTURE));
        *op = saved; *operand = old_operand; *block = old_block; origins[1].function = 1;
    }
    xr_xir_artifact_free(closed); xr_xir_artifact_free(source); CHECK(!live);
}
int main(void) {
    native_packet_proof();
    XrXirArtifact *forwarding = nominal_forwarding_checked();
    provenance_lowering(forwarding);
    xr_xir_artifact_free(forwarding); CHECK(!live);
    provenance_artifact_ownership();
    provenance_nominal_attacks();
    provenance_declaration_attacks();
    provenance_closure_attacks();
    provenance_ownership();
    specialization_correspondence_attacks();
    nominal_layout_failures();
    packet_failures(17); packet_failures(18);
    packet_failures(16);
    packet_failures(15);
    packet_failures(14);
    specialization_failures(7);
    specialization_failures(8);
    nominal_field_closure_failures();
    specialization_failures(6);
    nominal_lowering_failures();
    packet_failures(9); packet_failures(10); packet_failures(11); packet_failures(12); packet_failures(13); specialization_failures(5);
    packet_failures(7); packet_failures(8); specialization_failures(4);
    packet_failures(6); specialization_failures(3);
    packet_failures(false); packet_failures(true); packet_failures(2); packet_failures(3); packet_failures(4); specialization_failures(false); specialization_failures(true); packet_failures(5); specialization_failures(2);
    return 0;
}
