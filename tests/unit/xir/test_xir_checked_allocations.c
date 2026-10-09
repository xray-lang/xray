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
#include "xir_construction_fixture.h"
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

#define xr_free(p) packet_free(p)
#include "base/xcompile_resources.c"
#include "xir/xxir_types.c"
#include "xir/xxir_constraints.c"
#include "xir/xxir_constraint_proof.c"
#include "xir/xxir_implementation.c"
#include "xir/xxir_implementation_verify.c"
#include "xir/xxir_interface.c"
#include "xir/xxir_interface_members.c"
#include "xir/xxir_type_layout.c"
#include "xir/xxir_generic.c"
#include "xir/xxir_defaults.c"
#include "xir/xxir.c"
#include "xir/xxir_declarations.c"
#include "xir/xxir_verify.c"
#include "xir/xxir_effects.c"
#include "xir/xxir_layout.c"
#include "xir/xxir_checked.c"
#include "xir/xxir_specialize.c"
#include "xir/xxir_program_match.c"
#define XIR_CONSUMER_COUNTED_RESOURCES
#include "xir_consumer_context_owner.h"
#include "xir_checked_fixture.h"
#include "xir_generic_fixture.h"
#include "xir_source_fixture_owner.h"
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
#include "xir_enum_checked_fixture.h"
#include "xir_cleanup_role_fixture.h"
static XrXirArtifact *array_packet_fixture(const XrXirCompileContext *context) {
    XirArrayMetadataFixture f; xir_array_metadata_init(&f);
    XrXirArtifact *checked = NULL;
    CHECK(xir_fixture_check(context, &f.module, &checked, NULL) == XR_XIR_OK);
    return checked;
}
static void packet_failures(unsigned kind) {
    consumer_context=consumer_context_default();
    XrXirArtifact *checked = kind >= 20 ? cleanup_role_fixture(suite_context) : kind == 19 ? enum_checked_fixture(suite_context) : kind >= 17 ? nominal_expression_fixture(suite_context) : kind == 16 ? struct_set_checked(suite_context, 0) : kind == 15 ? struct_ops_checked(suite_context, 0) : kind == 14 ? nominal_chain_fixture(suite_context, 3, 2) : kind >= 9 ? nominal_checked_fixture(suite_context, kind >= 12 ? 3 : kind == 11 ? 2 : kind == 10 ? 1 : 0) : kind == 8 ? array_generic_fixture(suite_context) : kind == 7 ? array_packet_fixture(suite_context) :
        kind == 6 ? constructed_fixture(suite_context) : kind == 5 ? generic_callable_fixture(suite_context) : kind == 4 ? function_ir_fixture(suite_context) : kind == 3 ? callable_fixture(suite_context) : kind == 2 ? local_fixture(suite_context) : kind == 1 ? generic_fixture(suite_context) : checked_fixture(suite_context);
    if (kind == 13 || kind == 18 || kind == 21) {
        XrXirArtifact *closed = NULL;
        CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_OK);
        xr_xir_compile_artifact_free(checked);checked=NULL; checked = closed;
    }
    size_t baseline = live;
    XrXirCheckedPacket packet = {0};
    calls = 0;
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    size_t write_sites = calls;
    xr_xir_compile_checked_packet_free(&packet); CHECK(live == baseline);
    for (size_t i = 0; i < write_sites; ++i) {
        calls = 0; fail_at = i;
        CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!packet.bytes && !packet.length && live == baseline);
    }
    fail_at = SIZE_MAX;
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);checked=NULL; CHECK(live==consumer_context_owner_count+1);
    XrXirArtifact *decoded = NULL;
    calls = 0;
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
    size_t read_sites = calls;
    xr_xir_compile_artifact_free(decoded); decoded=NULL; CHECK(live==consumer_context_owner_count+1);
    for (size_t i = 0; i < read_sites; ++i) {
        calls = 0; fail_at = i;
        CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!decoded && live==consumer_context_owner_count+1);
    }
    fail_at = SIZE_MAX;
    /* Exercise partial metadata teardown after valid integrity checks too. */
    for (size_t i = 64; i < packet.length; ++i) {
        packet.bytes[i] ^= 0xFF;
        checked_digest(packet.bytes, packet.length, packet.bytes + 32);
        XrXirStatus status = xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL);
        CHECK((status == XR_XIR_OK) == (decoded != NULL));
        xr_xir_compile_artifact_free(decoded); decoded=NULL; CHECK(live==consumer_context_owner_count+1);
        packet.bytes[i] ^= 0xFF;
    }
    xr_xir_compile_checked_packet_free(&packet);
    CHECK(live==consumer_context_owner_count && live_bytes==consumer_context_ledger_bytes());
    printf("%s packet physical release: %zu writer and %zu reader allocation sites\n",
        kind == 18 ? "Specialized nominal expressions" : kind == 17 ? "Abstract nominal expressions" : kind == 14 ? "Nominal field graph" : kind == 13 ? "Closed nominal fields" : kind == 12 ? "Nominal field expression" : kind == 11 ? "Nominal instance" : kind == 10 ? "Nominal Array field" : kind == 9 ? "Nominal declaration" : kind == 8 ? "Generic Array" : kind == 7 ? "Array operations" : kind == 6 ? "Constructed" : kind == 5 ? "Generic callable" : kind == 4 ? "Function" : kind == 3 ? "Callable" : kind == 2 ? "Local" : kind == 1 ? "Generic" : "Closed", write_sites, read_sites);
}

static void specialization_failures(unsigned callable) {
    XrXirArtifact *checked = callable == 9 ? enum_checked_fixture(suite_context) : callable == 8 ? nominal_expression_fixture(suite_context) : callable == 7 ? nominal_chain_fixture(suite_context, 3, 2) : callable == 6 ? nominal_generic_fixture(suite_context, true) : callable == 5 ? nominal_checked_fixture(suite_context, 3) : callable == 4 ? array_generic_fixture(suite_context) : callable == 3 ? constructed_fixture(suite_context) : callable == 2 ? generic_callable_fixture(suite_context) : callable ? callable_fixture(suite_context) : generic_fixture(suite_context), *output = NULL;
    XrXirModule built = *xr_xir_compile_artifact_module(checked); built.stage = XR_XIR_BUILT;
    size_t baseline=live,baseline_bytes=live_bytes,sites[2]={0};
    for(unsigned mode=0;mode<2;++mode){
        for(size_t attempt=0;attempt<=sites[mode];++attempt){
            XrXirCompileContext probe=consumer_context_ephemeral((XrCompileResourceLimits){67108864,8388608,128000000});
            XrCompileResourceStats owner_baseline=consumer_context_stats(&probe);
            XrXirArtifact *producer=NULL;
            if(mode)CHECK(xir_fixture_check(&probe, &built, &producer, NULL)==XR_XIR_OK);
            size_t stage_baseline=live,stage_bytes=live_bytes;
            calls=0;fail_at=attempt?attempt-1:SIZE_MAX;
            XrXirStatus status=mode?xr_xir_compile_specialize(producer,&output,NULL):xir_fixture_check(&probe, &built, &output, NULL);
            if(!attempt){CHECK(status==XR_XIR_OK && output);sites[mode]=calls;}
            else CHECK(status==XR_XIR_OUT_OF_MEMORY && !output);
            xr_xir_compile_artifact_free(output);output=NULL;
            CHECK(live==stage_baseline && live_bytes==stage_bytes);
            fail_at=SIZE_MAX;xr_xir_compile_artifact_free(producer);
            consumer_context_ephemeral_free(&probe,owner_baseline);
            CHECK(live==baseline && live_bytes==baseline_bytes);
        }
    }
    fail_at = SIZE_MAX; xr_xir_compile_artifact_free(checked);checked=NULL; CHECK(live==consumer_context_owner_count && live_bytes==consumer_context_ledger_bytes());
    printf("%s physical release: %zu checking and %zu specialization allocation sites\n", callable == 8 ? "Nominal expressions" : callable == 7 ? "Nominal field graph" : callable == 6 ? "Combined nominal and function" : callable == 5 ? "Nominal field substitution" : callable == 4 ? "Generic Array" : callable == 3 ? "Constructed" : callable == 2 ? "Generic callable" : callable ? "Callable" : "Generic", sites[0], sites[1]);
}
static void nominal_field_closure_failures(void) {
    for(unsigned mode=0;mode<3;++mode){
        XrXirCompileContext measure=consumer_context_ephemeral((XrCompileResourceLimits){67108864,8388608,128000000});
        XrCompileResourceStats measure_baseline=consumer_context_stats(&measure);
        XrXirArtifact *measured=nominal_field_closure_fixture(&measure,mode);
        uint64_t prefix=consumer_context_stats(&measure).work;
        xr_xir_compile_artifact_free(measured);consumer_context_ephemeral_free(&measure,measure_baseline);
        size_t sites=0,physical=live,physical_bytes=live_bytes;
        for(size_t attempt=0;attempt<=sites;++attempt){
            XrXirCompileContext probe=consumer_context_ephemeral((XrCompileResourceLimits){67108864,8388608,mode==2?prefix+10000:128000000});
            XrCompileResourceStats owner_baseline=consumer_context_stats(&probe);
            XrXirArtifact *checked=nominal_field_closure_fixture(&probe,mode),*output=NULL;
            size_t baseline=live,baseline_bytes=live_bytes;
            calls=0;fail_at=attempt?attempt-1:SIZE_MAX;
            XrXirStatus expected=attempt?XR_XIR_OUT_OF_MEMORY:mode==0?XR_XIR_OK:mode==1?XR_XIR_BAD_TYPE:XR_XIR_BUDGET;
            CHECK(xr_xir_compile_specialize(checked,&output,NULL)==expected);
            if(!attempt)sites=calls;
            if(attempt || mode)CHECK(!output);
            xr_xir_compile_artifact_free(output);CHECK(live==baseline && live_bytes==baseline_bytes);
            fail_at=SIZE_MAX;xr_xir_compile_artifact_free(checked);consumer_context_ephemeral_free(&probe,owner_baseline);
            CHECK(live==physical && live_bytes==physical_bytes);
        }
        printf("Nominal field closure mode %u: %zu failure sites, no retained metadata\n",mode,sites);
    }
}
static void nominal_lowering_failures(void) {
    XrXirArtifact *checked = nominal_checked_fixture(suite_context, 3), *closed = NULL, *lowered = NULL;
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);checked=NULL;
    size_t baseline = live; calls = 0; fail_at = SIZE_MAX;
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK);
    size_t sites = calls; xr_xir_compile_artifact_free(lowered); lowered=NULL; CHECK(live == baseline);
    for (size_t i = 0; i < sites; ++i) {
        calls = 0; fail_at = i;
        CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!lowered && live == baseline);
    }
    fail_at = SIZE_MAX; xr_xir_compile_artifact_free(closed);closed=NULL; CHECK(live==consumer_context_owner_count && live_bytes==consumer_context_ledger_bytes());
    printf("Nominal lowering: %zu allocation sites physically released\n", sites);
}

static void nominal_layout_failures(void) {
    XrXirArtifact *checked = nominal_chain_fixture(suite_context, 3, 2), *closed = NULL;
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);checked=NULL;
    const XrXirTypes *types = xr_xir_compile_artifact_module(closed)->types;
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    XrXirCompileContext original = consumer_context_default(), budget = original;
    XrXirLayout layout = {0}; uint32_t offsets[] = {99, 99};
    size_t baseline = live; calls = 0;
    CHECK(xr_xir_compile_nominal_layout(&budget, types, (XrXirType)256, &target, &layout, offsets, 2) == XR_XIR_OK);
    CHECK(layout.size == 64 && layout.alignment == 8 && offsets[0] == 0 && offsets[1] == 32 && live == baseline);
    size_t sites = calls;
    XrCompileResourceStats required=consumer_context_stats(&budget);
    uint64_t work=required.work;
    for (size_t i = 0; i < sites; ++i) {
        calls = 0; fail_at = i; budget = original; layout=(XrXirLayout){0};offsets[0] = offsets[1] = 99;
        CHECK(xr_xir_compile_nominal_layout(&budget, types, (XrXirType)256, &target, &layout, offsets, 2) == XR_XIR_OUT_OF_MEMORY);
        CHECK(live == baseline && !layout.size && !layout.alignment && offsets[0] == 99 && offsets[1] == 99 &&
            !memcmp(&budget, &original, sizeof(budget)));
    }
    fail_at=SIZE_MAX;budget=consumer_context_limits((XrCompileResourceLimits){67108864,8388608,work-1});
    baseline=live;layout=(XrXirLayout){0};offsets[0]=offsets[1]=99;
    XrXirCompileContext short_budget = budget;
    CHECK(xr_xir_compile_nominal_layout(&budget, types, (XrXirType)256, &target, &layout, offsets, 2) == XR_XIR_BUDGET);
    CHECK(live == baseline && !layout.size && !layout.alignment && offsets[0] == 99 && offsets[1] == 99 &&
        !memcmp(&budget, &short_budget, sizeof(budget)));
    xr_xir_compile_artifact_free(closed);closed=NULL; CHECK(live==consumer_context_owner_count && live_bytes==consumer_context_ledger_bytes());
}
static void provenance_ownership(void) {
    XrXirArtifact *source = generic_fixture(suite_context);
    XrXirArtifact *closed = NULL;
    CHECK(xr_xir_compile_specialize(source, &closed, NULL) == XR_XIR_OK);
    XrXirType integer = XR_XIR_I64, string = XR_XIR_STRING;
    XrXirOrigin origins[] = {{.function=0},
        {.function=1,.arguments=&integer,.argument_count=1},
        {.function=1,.arguments=&string,.argument_count=1}};
    CHECK(!source->module.provenance);
    XrXirProvenance instance = {.kind=XR_XIR_EVIDENCE_INSTANCE,
        .source=source,.origins=origins,.count=3};
    XrXirCompileContext initial = consumer_context_default(), remaining = initial;
    XrXirProvenance *copy = NULL;
    size_t baseline = live; calls = 0;
    CHECK(xr_xir_compile_provenance_copy(&remaining, &instance, &copy) == XR_XIR_OK);
    size_t sites = calls;
    XrCompileResourceStats required=consumer_context_stats(&remaining);
    CHECK(copy && copy->kind == XR_XIR_EVIDENCE_INSTANCE && copy->count == 3 && copy->source != source);
    CHECK(!copy->contracts && !copy->contract_count && !copy->bindings && !copy->binding_count);
    CHECK(!copy->origins[1].effect_arguments && !copy->origins[1].effect_argument_count);
    CHECK(copy->origins != origins && copy->origins[1].arguments != &integer);
    CHECK(copy->source->module.functions != source->module.functions);
    xr_xir_compile_provenance_free(copy);copy=NULL; CHECK(live == baseline);
    for (size_t i = 0; i < sites; ++i) {
        calls = 0; fail_at = i; remaining = initial; copy = NULL;
        CHECK(xr_xir_compile_provenance_copy(&remaining, &instance, &copy) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!copy && live == baseline);
    }
    fail_at = SIZE_MAX;
    for (unsigned mode=0;mode<4;++mode) {
        XrCompileResourceLimits limits={required.allocated_bytes,8388608,required.work};
        if(mode==2)--limits.allocated_bytes;if(mode==3)--limits.work;
        remaining=consumer_context_ephemeral(limits);remaining.limits.functions=mode==1?1:2;
        XrCompileResourceStats before=consumer_context_stats(&remaining);copy=NULL;
        CHECK(xr_xir_compile_provenance_copy(&remaining,&instance,&copy)==(mode?XR_XIR_BUDGET:XR_XIR_OK));
        if(mode)CHECK(!copy);
        xr_xir_compile_provenance_free(copy);copy=NULL;
        consumer_context_ephemeral_free(&remaining,before);CHECK(live==baseline);
    }
    for (unsigned mode = 0; mode < 3; ++mode) {
        XrXirOrigin saved = origins[1]; remaining = initial;
        if (mode == 0) origins[1].function = 2;
        if (mode == 1) origins[1].argument_count = 0;
        if (mode == 2) origins[1].arguments = NULL;
        CHECK(xr_xir_compile_provenance_copy(&remaining, &instance, &copy) == XR_XIR_BAD_STRUCTURE);
        CHECK(!copy && live == baseline); origins[1] = saved;
    }
    remaining = initial;
    CHECK(xr_xir_compile_provenance_copy(&remaining, &instance, &copy) == XR_XIR_OK);
    integer = XR_XIR_BOOL; string = XR_XIR_I64;
    memset(origins, 0xa5, sizeof(origins)); xr_xir_compile_artifact_free(source);source=NULL;
    CHECK(copy->origins[1].arguments[0] == XR_XIR_I64 && copy->origins[2].arguments[0] == XR_XIR_STRING);
    CHECK(xr_xir_compile_artifact_verify(copy->source, NULL) == XR_XIR_OK);
    remaining = initial;
    CHECK(xr_xir_compile_provenance_functions_match(&remaining, &copy->source->module, &closed->module, copy, NULL) == XR_XIR_OK);
    const XrXirType *saved_arguments = copy->origins[1].arguments;
    copy->origins[1].arguments = NULL; remaining = initial;
    CHECK(xr_xir_compile_provenance_functions_match(&remaining, &copy->source->module, &closed->module, copy, NULL) == XR_XIR_BAD_STRUCTURE);
    copy->origins[1].arguments = saved_arguments;
    xr_xir_compile_artifact_free(closed);closed=NULL;
    xr_xir_compile_provenance_free(copy); CHECK(live==consumer_context_owner_count && live_bytes==consumer_context_ledger_bytes());
    printf("provenance copy census: sites=%zu allocated=%llu peak=%llu work=%llu\n",sites,
        (unsigned long long)required.allocated_bytes,(unsigned long long)required.peak_bytes,
        (unsigned long long)required.work);
    printf("provenance ownership: %zu allocation failures released\n", sites);
}

static void provenance_packet_roundtrip(XrXirArtifact *closed) {
    size_t baseline = live;
    XrXirCheckedPacket packet = {0}; XrXirArtifact *decoded = NULL;
    calls = 0;
    CHECK(xr_xir_compile_checked_write(closed, &packet, NULL) == XR_XIR_OK);
    size_t writes = calls;
    xr_xir_compile_checked_packet_free(&packet);
    for (size_t i = 0; i < writes; ++i) {
        calls = 0; fail_at = i;
        CHECK(xr_xir_compile_checked_write(closed, &packet, NULL) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!packet.bytes && live == baseline);
    }
    fail_at = SIZE_MAX;
    CHECK(xr_xir_compile_checked_write(closed, &packet, NULL) == XR_XIR_OK);
    calls = 0;
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
    size_t reads = calls;
    CHECK(decoded->module.provenance && decoded->module.provenance->kind == XR_XIR_EVIDENCE_INSTANCE &&
        decoded->module.provenance->count == 3);
    xr_xir_compile_artifact_free(decoded);decoded=NULL;
    for (size_t i = 0; i < reads; ++i) {
        calls = 0; fail_at = i;
        CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!decoded && live == baseline + 1);
    }
    fail_at = SIZE_MAX;
    XrXirArtifact plain = *closed; plain.module.provenance = NULL;
    XrXirCheckedPacket plain_packet = {0}, source_packet = {0};
    CHECK(xr_xir_compile_checked_write(&plain, &plain_packet, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(closed->module.provenance->source, &source_packet, NULL) == XR_XIR_OK);
    /* The outer evidence kind replaces the plain ABSENT field. Its embedded
     * source payload includes an ABSENT kind, but cannot contain INSTANCE. */
    CHECK(!closed->module.provenance->source->module.provenance);
    CHECK(plain_packet.length >= 68 && source_packet.length >= 68);
    size_t source_offset = plain_packet.length;
    size_t source_bytes = source_packet.length - 64;
    CHECK(source_offset <= packet.length && source_bytes < packet.length - source_offset);
    CHECK(!memcmp(packet.bytes + 64, plain_packet.bytes + 64, plain_packet.length - 68));
    CHECK(packet.bytes[source_offset - 4] == XR_XIR_EVIDENCE_INSTANCE &&
        !packet.bytes[source_offset - 3] && !packet.bytes[source_offset - 2] && !packet.bytes[source_offset - 1]);
    CHECK(!memcmp(packet.bytes + source_offset, source_packet.bytes + 64, source_bytes));
    size_t nested_kind = source_offset + source_bytes - 4;
    xr_xir_compile_checked_packet_free(&plain_packet); xr_xir_compile_checked_packet_free(&source_packet);
    CHECK(nested_kind + 4 < packet.length && !packet.bytes[nested_kind] &&
        !packet.bytes[nested_kind + 1] && !packet.bytes[nested_kind + 2] && !packet.bytes[nested_kind + 3]);
    packet.bytes[nested_kind] = XR_XIR_EVIDENCE_INSTANCE;
    checked_digest(packet.bytes, packet.length, packet.bytes + 32);
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(!decoded && live == baseline + 1);
    packet.bytes[nested_kind] = XR_XIR_EVIDENCE_ABSENT;
    for (size_t i = 64; i < packet.length; ++i) {
        packet.bytes[i] ^= 0xff; checked_digest(packet.bytes, packet.length, packet.bytes + 32);
        XrXirStatus status = xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL);
        CHECK((status == XR_XIR_OK) == (decoded != NULL));
        xr_xir_compile_artifact_free(decoded); decoded=NULL; CHECK(live == baseline + 1);
        packet.bytes[i] ^= 0xff;
    }
    checked_digest(packet.bytes, packet.length, packet.bytes + 32);
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xa5, packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_artifact_verify(decoded, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded); decoded=NULL; CHECK(live == baseline);
    printf("provenance packet: %zu write and %zu read allocation failures released\n", writes, reads);
}

static void native_packet_proof(void) {
    XrXirArtifact *artifact=nominal_expression_lowered(suite_context);
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    CHECK(module->function_count==9);
    XrXirCallEntry entries[9]={0};
    for(uint32_t i=0;i<9;++i){entries[i].parameter_count=module->functions[i].parameter_count;
        entries[i].parameters=module->functions[i].parameters;entries[i].result=module->functions[i].result;}
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,*xr_xir_compile_artifact_target(artifact),
        entries,9,module->declarations,{0},module->types,xr_xir_compile_program_proof(artifact)};
    XrXirProgramProof proof=xr_xir_compile_program_proof(artifact);
    size_t baseline=live,baseline_bytes=live_bytes,sites=0;
    XrCompileResourceStats required={0};
    XrXirProgramPermissions *permissions=NULL;
    for(size_t attempt=0;attempt<=sites;++attempt){
        XrXirCompileContext probe=consumer_context_ephemeral((XrCompileResourceLimits){67108864,8388608,16000001});
        XrCompileResourceStats before=consumer_context_stats(&probe);
        calls=0;fail_at=attempt?attempt-1:SIZE_MAX;
        CHECK(xr_xir_compile_program_proof_verify(&probe,&spec,&proof,&permissions)==(attempt?XR_XIR_OUT_OF_MEMORY:XR_XIR_OK));
        CHECK((permissions!=NULL)==!attempt);
        if(!attempt){sites=calls;required=consumer_context_stats(&probe);}
        xr_compile_resources_free(permissions);permissions=NULL;
        fail_at=SIZE_MAX;consumer_context_ephemeral_free(&probe,before);
        CHECK(live==baseline && live_bytes==baseline_bytes);
    }
    for(uint32_t attack=0;attack<5;++attack){
        XrCompileResourceLimits limits={67108864,8388608,16000001};
        if(attack==2)limits.allocated_bytes=consumer_context_owners[0].baseline.live_bytes;
        if(attack==3)limits.live_bytes=consumer_context_owners[0].baseline.live_bytes;
        XrXirCompileContext probe=consumer_context_ephemeral(limits);
        XrCompileResourceStats before=consumer_context_stats(&probe);
        if(!attack)artifact->checked_packet.bytes[0]^=1;
        if(attack==1)artifact->checked_identity[0]^=1;
        if(attack==4)entries[0].result=XR_XIR_BOOL;
        CHECK(xr_xir_compile_program_proof_verify(&probe,&spec,&proof,&permissions)==((attack==2||attack==3)?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE));
        CHECK(!permissions);
        if(!attack)artifact->checked_packet.bytes[0]^=1;
        if(attack==1)artifact->checked_identity[0]^=1;
        entries[0].result=module->functions[0].result;
        consumer_context_ephemeral_free(&probe,before);CHECK(live==baseline && live_bytes==baseline_bytes);
    }
    for(uint64_t cap=16384;cap<=131072;cap*=2){
        for(unsigned minus=0;minus<2;++minus){
            XrXirCompileContext probe=consumer_context_ephemeral((XrCompileResourceLimits){cap*7-minus,cap*7-minus,16000001});
            XrCompileResourceStats before=consumer_context_stats(&probe);
            XrXirStatus status=xr_xir_compile_program_proof_verify(&probe,&spec,&proof,&permissions);
            CHECK(status==XR_XIR_OK || status==XR_XIR_BUDGET);
            CHECK((permissions!=NULL)==(status==XR_XIR_OK));
            XrCompileResourceStats actual=consumer_context_stats(&probe);
            CHECK(actual.peak_bytes<=cap*7-minus && actual.allocated_bytes<=cap*7-minus);
            xr_compile_resources_free(permissions);permissions=NULL;
            consumer_context_ephemeral_free(&probe,before);CHECK(live==baseline && live_bytes==baseline_bytes);
            printf("native proof whole cap %llu minus%u status%u peak%llu\n",(unsigned long long)cap,minus,status,(unsigned long long)actual.peak_bytes);
        }
    }
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus){
        XrCompileResourceLimits limits={67108864,8388608,16000001};
        if(!axis)limits.allocated_bytes=required.allocated_bytes-minus;
        if(axis==1)limits.live_bytes=required.peak_bytes-minus;
        if(axis==2)limits.work=required.work-minus;
        XrXirCompileContext probe=consumer_context_ephemeral(limits);XrCompileResourceStats before=consumer_context_stats(&probe);
        CHECK(xr_xir_compile_program_proof_verify(&probe,&spec,&proof,&permissions)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        CHECK((permissions!=NULL)==!minus);xr_compile_resources_free(permissions);permissions=NULL;
        consumer_context_ephemeral_free(&probe,before);CHECK(live==baseline && live_bytes==baseline_bytes);
    }
    XrXirCompileContext probe=consumer_context_ephemeral((XrCompileResourceLimits){67108864,8388608,16000001});
    XrCompileResourceStats before=consumer_context_stats(&probe);XrXirProgramProof overflowing=proof;overflowing.length=SIZE_MAX;
    calls=0;CHECK(xr_xir_compile_program_proof_verify(&probe,&spec,&overflowing,&permissions)==XR_XIR_BUDGET && !calls && !permissions);
    consumer_context_ephemeral_free(&probe,before);
    probe=consumer_context_ephemeral((XrCompileResourceLimits){67108864,8388608,proof.length});before=consumer_context_stats(&probe);
    CHECK(xr_xir_compile_program_proof_verify(&probe,&spec,&proof,&permissions)==XR_XIR_BUDGET && !permissions);
    consumer_context_ephemeral_free(&probe,before);CHECK(live==baseline && live_bytes==baseline_bytes);
    xr_xir_compile_artifact_free(artifact);artifact=NULL;
    CHECK(live==consumer_context_owner_count && live_bytes==consumer_context_ledger_bytes());
    printf("native packet proof census: sites=%zu allocated=%llu peak=%llu work=%llu\n",sites,
        (unsigned long long)required.allocated_bytes,(unsigned long long)required.peak_bytes,
        (unsigned long long)required.work);
    printf("native packet proof: %zu allocation failures released, 3 axes exact/minus\n",sites);
}

static void provenance_lowering(XrXirArtifact *closed) {
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    XrXirArtifact *lowered = NULL;
    size_t baseline = live; calls = 0;
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK);
    size_t sites = calls;
    CHECK(lowered->checked_packet.bytes && lowered->checked_packet.length >= 64);
    XrXirCheckedPacket expected = {0};
    CHECK(xr_xir_compile_checked_write(closed, &expected, NULL) == XR_XIR_OK);
    CHECK(expected.length == lowered->checked_packet.length);
    CHECK(!memcmp(expected.bytes, lowered->checked_packet.bytes, expected.length));
    xr_xir_compile_checked_packet_free(&expected);
    lowered->checked_packet.bytes[0] ^= 1;
    CHECK(xr_xir_compile_artifact_verify(lowered, NULL) == XR_XIR_BAD_STRUCTURE);
    lowered->checked_packet.bytes[0] ^= 1;
    lowered->checked_identity[0] ^= 1;
    CHECK(xr_xir_compile_artifact_verify(lowered, NULL) == XR_XIR_BAD_STRUCTURE);
    lowered->checked_identity[0] ^= 1;
    XrXirCompileContext tight=consumer_context_ephemeral((XrCompileResourceLimits){lowered->checked_packet.length+consumer_context_owners[0].baseline.live_bytes,8388608,128000000});
    XrCompileResourceStats tight_baseline=consumer_context_stats(&tight);
    CHECK(xr_xir_compile_verify_v2(&tight, &lowered->module, xr_xir_compile_artifact_construction(lowered), NULL)==XR_XIR_BUDGET);
    consumer_context_ephemeral_free(&tight,tight_baseline);
    tight=consumer_context_ephemeral((XrCompileResourceLimits){67108864,8388608,1});tight_baseline=consumer_context_stats(&tight);
    CHECK(xr_xir_compile_verify_v2(&tight, &lowered->module, xr_xir_compile_artifact_construction(lowered), NULL)==XR_XIR_BUDGET);
    consumer_context_ephemeral_free(&tight,tight_baseline);
    CHECK(lowered->module.provenance && lowered->module.provenance != closed->module.provenance);
    CHECK(lowered->module.provenance->source->module.stage == XR_XIR_CHECKED);
    if (lowered->module.types && lowered->module.types->nominals) {
        CHECK(lowered->module.types->nominals->identities && !lowered->module.types->nominals->declarations);
        XrXirTypeNode *node = (XrXirTypeNode *)&lowered->module.types->nodes[0];
        CHECK(node->kind == XR_XIR_TYPE_NOMINAL && node->nominal.field_count);
        XrXirType *field = (XrXirType *)node->nominal.fields, saved = *field;
        *field = saved == XR_XIR_I64 ? XR_XIR_U8 : XR_XIR_I64;
        CHECK(xr_xir_compile_artifact_verify(lowered, NULL) != XR_XIR_OK);
        *field = saved;
    }
    CHECK(xr_xir_compile_artifact_verify(lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(lowered); lowered=NULL; CHECK(live == baseline);
    for (size_t i = 0; i < sites; ++i) {
        calls = 0; fail_at = i; lowered = NULL;
        CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!lowered && live == baseline);
    }
    fail_at = SIZE_MAX;
    printf("provenance lowering: %zu allocation failures released\n", sites);
}

static void provenance_artifact_ownership(void) {
    XrXirArtifact *source = generic_fixture(suite_context), *closed = NULL, *copy = NULL;
    CHECK(xr_xir_compile_specialize(source, &closed, NULL) == XR_XIR_OK);
    XrXirType integer = XR_XIR_I64, string = XR_XIR_STRING;
    XrXirOrigin origins[] = {{.function=0},
        {.function=1,.arguments=&integer,.argument_count=1},
        {.function=1,.arguments=&string,.argument_count=1}};
    CHECK(!source->module.provenance);
    XrXirProvenance instance = {.kind=XR_XIR_EVIDENCE_INSTANCE,
        .source=source,.origins=origins,.count=3};
    XrXirCompileContext remaining = consumer_context_default();
    XrXirProvenance *proof = NULL;
    CHECK(xr_xir_compile_provenance_copy(&remaining, &instance, &proof) == XR_XIR_OK);
    xr_xir_compile_provenance_free((XrXirProvenance *)closed->module.provenance);
    closed->module.provenance = proof;
    xr_xir_compile_artifact_free(source);source=NULL;
    CHECK(xr_xir_compile_artifact_verify(closed, NULL) == XR_XIR_OK);
    size_t baseline = live; calls = 0;
    CHECK(xr_xir_compile_recheck_v2(suite_context, &closed->module, xr_xir_compile_artifact_construction(closed), &copy, NULL) == XR_XIR_OK);
    size_t sites = calls;
    CHECK(copy->module.provenance != proof && copy->module.provenance->source != proof->source);
    xr_xir_compile_artifact_free(copy);copy=NULL; CHECK(live == baseline);
    for (size_t i = 0; i < sites; ++i) {
        calls = 0; fail_at = i; copy = NULL;
        CHECK(xr_xir_compile_recheck_v2(suite_context, &closed->module, xr_xir_compile_artifact_construction(closed), &copy, NULL) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!copy && live == baseline);
    }
    fail_at = SIZE_MAX;
    remaining = consumer_context_default(); remaining.limits.functions = 4;
    CHECK(xr_xir_compile_verify_v2(&remaining, &closed->module, xr_xir_compile_artifact_construction(closed), NULL)==XR_XIR_BUDGET);
    remaining.limits.functions = 5;
    CHECK(xr_xir_compile_verify_v2(&remaining, &closed->module, xr_xir_compile_artifact_construction(closed), NULL)==XR_XIR_OK);
    proof->source->module.provenance = proof;
    CHECK(xr_xir_compile_artifact_verify(closed, NULL) == XR_XIR_BAD_STRUCTURE);
    proof->source->module.provenance = NULL;
    --proof->count;
    CHECK(xr_xir_compile_artifact_verify(closed, NULL) == XR_XIR_BAD_STRUCTURE);
    ++proof->count;
    provenance_packet_roundtrip(closed);
    provenance_lowering(closed);
    CHECK(xr_xir_compile_specialize(closed, &copy, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);closed=NULL;
    CHECK(xr_xir_compile_artifact_verify(copy, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(copy);copy=NULL; CHECK(live==consumer_context_owner_count && live_bytes==consumer_context_ledger_bytes());
    printf("provenance artifact: %zu allocation failures released\n", sites);
}

static void provenance_nominal_attacks(void) {
    XrXirArtifact *source = nominal_expression_fixture(suite_context), *closed = NULL;
    CHECK(xr_xir_compile_specialize(source, &closed, NULL) == XR_XIR_OK);
    CHECK(closed->module.function_count == 6);
    XrXirType arguments[] = {XR_XIR_I64, XR_XIR_U8, XR_XIR_STRING};
    XrXirOrigin origins[] = {{.function=0}, {.function=2}, {.function=3},
        {.function=1,.arguments=arguments,.argument_count=1},
        {.function=1,.arguments=arguments+1,.argument_count=1},
        {.function=1,.arguments=arguments+2,.argument_count=1}};
    CHECK(!source->module.provenance);
    XrXirProvenance instance = {.kind=XR_XIR_EVIDENCE_INSTANCE,
        .source=source,.origins=origins,.count=6};
    XrXirNominalTable *table = (XrXirNominalTable *)closed->module.types->nominals;
    XrXirNominalDeclaration *d = (XrXirNominalDeclaration *)&table->declarations[0];
    XrXirNominalField *field = (XrXirNominalField *)&d->fields[0];
    char *module = (char *)d->module.bytes, *name = (char *)d->name.bytes;
    char *field_name = (char *)field->name.bytes;
    char *function_name = (char *)closed->module.functions[3].name;
    XrXirConstraint *constraint = (XrXirConstraint *)d->constraints;
    size_t baseline = live;
    for (unsigned attack = 0; attack < 10; ++attack) {
        XrXirNominalDeclaration saved = *d; XrXirNominalField saved_field = *field;
        char old_module = *module, old_name = *name, old_field = *field_name, old_function = *function_name;
        XrXirConstraint old_constraint = *constraint;
        uint32_t old_count = table->count;
        if (attack == 1) *module ^= 1;
        if (attack == 2) *name ^= 1;
        if (attack == 3) d->exported ^= 1;
        if (attack == 4) constraint->markers ^= XR_XIR_CONSTRAINT_SENDABLE;
        if (attack == 5) *field_name ^= 1;
        if (attack == 6) field->flags ^= XR_XIR_FIELD_PRIVATE;
        if (attack == 7) field->type = XR_XIR_I64;
        if (attack == 8) --table->count;
        if (attack == 9) *function_name ^= 1;
        XrXirCompileContext remaining=consumer_context_ephemeral((XrCompileResourceLimits){67108864,8388608,128000000});
        XrCompileResourceStats probe_baseline=consumer_context_stats(&remaining);
        CHECK(xr_xir_compile_provenance_functions_match(&remaining, &source->module, &closed->module, &instance, NULL) ==
            (!attack ? XR_XIR_OK : attack == 7 ? XR_XIR_BAD_TYPE : XR_XIR_BAD_STRUCTURE));
        consumer_context_ephemeral_free(&remaining,probe_baseline);CHECK(live == baseline);
        *module = old_module; *name = old_name; *field_name = old_field; *function_name = old_function;
        *constraint = old_constraint; table->count = old_count; *d = saved; *field = saved_field;
    }
    XrXirCompileContext remaining = consumer_context_default();
    XrXirProvenance *proof = NULL;
    CHECK(xr_xir_compile_provenance_copy(&remaining, &instance, &proof) == XR_XIR_OK);
    xr_xir_compile_provenance_free((XrXirProvenance *)closed->module.provenance);
    closed->module.provenance = proof;
    provenance_lowering(closed);
    xr_xir_compile_artifact_free(closed);closed=NULL; xr_xir_compile_artifact_free(source);source=NULL; CHECK(live==consumer_context_owner_count && live_bytes==consumer_context_ledger_bytes());
}

static void provenance_declaration_attacks(void) {
    XrXirArtifact *source = checked_fixture(suite_context), *closed = NULL;
    CHECK(xr_xir_compile_specialize(source, &closed, NULL) == XR_XIR_OK);
    CHECK(closed->module.function_count == 9);
    XrXirOrigin origins[9];
    for (uint32_t i = 0; i < 9; ++i) origins[i] = (XrXirOrigin) {.function=i};
    CHECK(!source->module.provenance);
    XrXirProvenance instance = {.kind=XR_XIR_EVIDENCE_INSTANCE,
        .source=source,.origins=origins,.count=9};
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
        XrXirCompileContext remaining=consumer_context_ephemeral((XrCompileResourceLimits){67108864,8388608,128000000});
        XrCompileResourceStats probe_baseline=consumer_context_stats(&remaining);
        CHECK(xr_xir_compile_provenance_functions_match(&remaining, &source->module, &closed->module, &instance, NULL) ==
            (!attack ? XR_XIR_OK : attack == 8 ? XR_XIR_BAD_TYPE : XR_XIR_BAD_STRUCTURE));
        consumer_context_ephemeral_free(&remaining,probe_baseline);CHECK(live == baseline);
        name[0] = saved_name; bytes[0] = saved_byte; *dependency = saved_dependency;
        *d = saved_d; *module = saved_m; *slot = saved_s;
    }
    xr_xir_compile_artifact_free(closed);closed=NULL; xr_xir_compile_artifact_free(source);source=NULL; CHECK(live==consumer_context_owner_count && live_bytes==consumer_context_ledger_bytes());
}

static void provenance_closure_attacks(void) {
    XrXirArtifact *source = generic_fixture(suite_context), *closed = NULL;
    CHECK(xr_xir_compile_specialize(source, &closed, NULL) == XR_XIR_OK);
    XrXirType integer = XR_XIR_I64, string = XR_XIR_STRING, boolean = XR_XIR_BOOL;
    XrXirFunction functions[4];
    memcpy(functions, closed->module.functions, 3 * sizeof(*functions));
    XrXirOrigin origins[] = {{.function=0},
        {.function=1,.arguments=&integer,.argument_count=1},
        {.function=1,.arguments=&string,.argument_count=1},
        {.function=1,.arguments=&boolean,.argument_count=1}};
    CHECK(!source->module.provenance);
    XrXirProvenance instance = {.kind=XR_XIR_EVIDENCE_INSTANCE,
        .source=source,.origins=origins,.count=3};
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
        XrXirCompileContext remaining=consumer_context_ephemeral((XrCompileResourceLimits){67108864,8388608,128000000});
        XrCompileResourceStats probe_baseline=consumer_context_stats(&remaining);
        instance.origins = attack == 4 ? origins + 1 : origins;
        instance.count = destination.function_count;
        CHECK(xr_xir_compile_provenance_functions_match(&remaining, &source->module, &destination, &instance, NULL) == (attack ? XR_XIR_BAD_STRUCTURE : XR_XIR_OK));
        consumer_context_ephemeral_free(&remaining,probe_baseline);CHECK(live == baseline);
    }
    xr_xir_compile_artifact_free(closed);closed=NULL; xr_xir_compile_artifact_free(source);source=NULL; CHECK(live==consumer_context_owner_count && live_bytes==consumer_context_ledger_bytes());
}

static void specialization_correspondence_attacks(void) {
    XrXirArtifact *source = generic_fixture(suite_context), *closed = NULL;
    CHECK(xr_xir_compile_specialize(source, &closed, NULL) == XR_XIR_OK);
    const XrXirModule *module = xr_xir_compile_artifact_module(closed);
    CHECK(module->function_count == 3);
    XrXirType integer = XR_XIR_I64, string = XR_XIR_STRING;
    XrXirOrigin origins[] = {{.function=0},
        {.function=1,.arguments=&integer,.argument_count=1},
        {.function=1,.arguments=&string,.argument_count=1}};
    CHECK(!source->module.provenance);
    XrXirProvenance instance = {.kind=XR_XIR_EVIDENCE_INSTANCE,
        .source=source,.origins=origins,.count=3};
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
        XrXirCompileContext remaining = consumer_context_default();
        XrXirStatus status = xr_xir_compile_provenance_functions_match(&remaining, &source->module, module, &instance, NULL);
        CHECK(status == (!attack ? XR_XIR_OK : attack == 2 ? XR_XIR_BAD_TYPE : XR_XIR_BAD_STRUCTURE));
        *op = saved; *operand = old_operand; *block = old_block; origins[1].function = 1;
    }
    xr_xir_compile_artifact_free(closed);closed=NULL; xr_xir_compile_artifact_free(source);source=NULL; CHECK(live==consumer_context_owner_count && live_bytes==consumer_context_ledger_bytes());
}
int main(void) {
    consumer_context=consumer_context_default();
    packet_failures(20); packet_failures(21);
    specialization_failures(9);
    packet_failures(19);
    native_packet_proof();
    XrXirArtifact *forwarding = nominal_forwarding_checked(suite_context);
    provenance_lowering(forwarding);
    xr_xir_compile_artifact_free(forwarding);forwarding=NULL; CHECK(live==consumer_context_owner_count && live_bytes==consumer_context_ledger_bytes());
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
    consumer_contexts_free();
    return 0;
}
