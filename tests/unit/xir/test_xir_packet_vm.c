/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_packet_vm.c - Source-free Checked consumer and native fixture producer
 *
 * KEY CONCEPT:
 *   The consumer links neither parser nor source compiler and needs only a packet.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_compile_owner.h"
#include "xir_source_runtime_allocations.h"
#include "xir_source_method_value_execution.h"
#include "xir_source_late_result_execution.h"
#include "xir_source_method_value_selection.h"
#include "xir_source_late_result_selection.h"
#include "xir_source_inference_execution.h"
#include "xir_source_inference_selection.h"
/* Lowering frees abstract type payloads allocated by the counted type clone. */
#include "xir_source_cases.h"
static XrXirArtifact *source_packet_lower(const XrXirCompileContext *context,const char *packet_path) {
    FILE *file = fopen(packet_path, "rb"); CHECK(file);
    CHECK(fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file); CHECK(size > 0 && (uint64_t)size <= UINT64_C(8)*1024*1024);
    CHECK(fseek(file, 0, SEEK_SET) == 0);
    uint8_t *bytes=NULL;CHECK(xr_compile_resources_alloc(context->resources,(size_t)size,(void **)&bytes)==XR_COMPILE_RESOURCE_OK && bytes);
    CHECK(fread(bytes, 1, (size_t) size, file) == (size_t) size && fclose(file) == 0);
    XrXirArtifact *checked = NULL, *lowered = NULL;
    source_fixture_compile_phase="Checked read";
    CHECK(xr_xir_compile_checked_read(context,bytes, (size_t) size, &checked, NULL) == XR_XIR_OK);
    memset(bytes, 0xCC, (size_t) size); xr_compile_resources_free(bytes);
    XrXirArtifact *specialized = NULL;
    source_fixture_compile_phase="Checked specialize";
    CHECK(xr_xir_compile_specialize(checked, &specialized, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked); checked = specialized;
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    source_fixture_compile_phase="Lower";
    CHECK(xr_xir_compile_lower(checked, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    return lowered;
}
int main(int argc, char **argv) {
    CHECK(argc >= 1 && argc <= 3);
    const XrXirCompileContext context=*source_fixture_source_owner(UINT64_C(64)*1024*1024,UINT64_C(128000000));
    const char *packet_path=argc==3 ? argv[2] : XR_CHECKED_FIXTURE;
    XrXirArtifact *lowered=source_packet_lower(&context,packet_path);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    SourceMethodValueEntries method_value_entries = source_method_value_select(module);
    SourceLateResultEntries late_result_entries = source_late_result_select(module);
    SourceInferenceEntries inference_entries = source_inference_select(module);
    uint32_t result = UINT32_MAX, advance = UINT32_MAX, update = UINT32_MAX, calculate = UINT32_MAX, resume_text = UINT32_MAX, stack_depth = UINT32_MAX, numeric_pause = UINT32_MAX, bound_result = UINT32_MAX, witness_result = UINT32_MAX, enum_witness_result = UINT32_MAX, enum_generic_witness_result = UINT32_MAX, generic_method_number = UINT32_MAX, generic_method_text = UINT32_MAX, generic_method_array = UINT32_MAX;
    for (uint32_t i = 0; i < module->function_count; ++i) {
        if (module->functions[i].name_length == 6 && !memcmp(module->functions[i].name, "result", 6)) result = i;
        if (module->functions[i].name_length == 9 && !memcmp(module->functions[i].name, "calculate", 9)) calculate = i;
        if (module->functions[i].name_length == 6 && !memcmp(module->functions[i].name, "update", 6)) update = i;
        if (module->functions[i].name_length == 10 && !memcmp(module->functions[i].name, "resumeText", 10)) resume_text = i;
        if (module->functions[i].name_length == 10 && !memcmp(module->functions[i].name, "stackDepth", 10)) stack_depth = i;
        if (module->functions[i].name_length == 12 && !memcmp(module->functions[i].name, "numericPause", 12)) numeric_pause = i;
        if (module->functions[i].name_length == 11 && !memcmp(module->functions[i].name, "boundResult", 11)) bound_result = i;
        if (module->functions[i].name_length == 13 && !memcmp(module->functions[i].name, "witnessResult", 13)) witness_result = i;
        if (module->functions[i].name_length == 17 && !memcmp(module->functions[i].name, "enumWitnessResult", 17)) enum_witness_result = i;
        if (module->functions[i].name_length == 24 && !memcmp(module->functions[i].name, "enumGenericWitnessResult", 24)) enum_generic_witness_result = i;
        if (module->functions[i].name_length == 19 && !memcmp(module->functions[i].name, "genericMethodNumber", 19)) generic_method_number = i;
        if (module->functions[i].name_length == 17 && !memcmp(module->functions[i].name, "genericMethodText", 17)) generic_method_text = i;
        if (module->functions[i].name_length == 18 && !memcmp(module->functions[i].name, "genericMethodArray", 18)) generic_method_array = i;
        if (module->functions[i].name_length == 7 && !memcmp(module->functions[i].name, "advance", 7)) advance = i;
    }
    CHECK(result != UINT32_MAX && advance != UINT32_MAX && update != UINT32_MAX && calculate != UINT32_MAX && resume_text != UINT32_MAX && stack_depth != UINT32_MAX && numeric_pause != UINT32_MAX && bound_result != UINT32_MAX && witness_result != UINT32_MAX && enum_witness_result != UINT32_MAX && enum_generic_witness_result != UINT32_MAX && generic_method_number != UINT32_MAX && generic_method_text != UINT32_MAX && generic_method_array != UINT32_MAX);
    uint32_t entry = module->declarations->entry_function;
    XrXirCSource source={0};
    size_t negative_physical_blocks=source_fixture_compile_live,negative_physical_bytes=source_fixture_compile_bytes;
    const XrXirCompileContext negative=*source_fixture_source_owner(UINT64_C(64)*1024*1024,UINT64_C(128000000));
    XrXirArtifact *negative_lowered=source_packet_lower(&negative,packet_path);
    XrXirProgramProof normal_proof=xr_xir_compile_program_proof(lowered),negative_proof=xr_xir_compile_program_proof(negative_lowered);
    CHECK(normal_proof.length==negative_proof.length && !memcmp(normal_proof.bytes,negative_proof.bytes,normal_proof.length));
    XrXirArtifact *negative_sentinel=negative_lowered;XrXirCSource rejected={0};
    CHECK(xr_xir_compile_emit_c(negative_lowered,"fixture_source",524288,&rejected)==XR_XIR_BUDGET);
    CHECK(!rejected.text && !rejected.length && negative_lowered==negative_sentinel);
    CHECK(xr_xir_compile_artifact_verify(negative_lowered,NULL)==XR_XIR_OK);
    XrXirProgram *negative_program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&negative_lowered,&negative_program)==XR_XIR_OK && !negative_lowered);
    xr_xir_compile_program_drop(negative_program);
    XrCompileResourceStats negative_stats={0};CHECK(xr_compile_resources_stats(negative.resources,&negative_stats)==XR_COMPILE_RESOURCE_OK);
    CHECK(negative_stats.live_bytes==source_fixture_source_owners[source_fixture_source_owner_count-1].baseline.live_bytes);
    fprintf(stderr,"packet negative bytecap: allocated=%llu peak=%llu work=%llu live=%llu; artifact verified/sealed/dropped\n",
        (unsigned long long)negative_stats.allocated_bytes,(unsigned long long)negative_stats.peak_bytes,
        (unsigned long long)negative_stats.work,(unsigned long long)negative_stats.live_bytes);
    source_fixture_source_owner_close(&negative);
    CHECK(source_fixture_compile_live==negative_physical_blocks && source_fixture_compile_bytes==negative_physical_bytes);
    XrXirStatus emitted = xr_xir_compile_emit_c(lowered, "fixture_source", 8388608, &source);
    if (emitted != XR_XIR_OK) fprintf(stderr, "Native source emission failed: %u, functions: %u\n", (unsigned)emitted, module->function_count);
    CHECK(emitted == XR_XIR_OK);
    CHECK(source.length > 524288);
    printf("Native source fixture: %zu bytes for %u functions\n",source.length,module->function_count);
    if (argc >= 2) {
        FILE *file = fopen(argv[1], "wb"); CHECK(file);
        CHECK(fwrite(source.text, 1, source.length, file) == source.length);
        CHECK(fprintf(file, "\nconst uint32_t fixture_source_result = %uu;\nconst uint32_t fixture_source_advance = %uu;\nconst uint32_t fixture_source_update = %uu;\nconst uint32_t fixture_source_calculate = %uu;\n", result, advance, update, calculate) > 0);
        CHECK(fprintf(file, "const uint32_t fixture_source_resume_text = %uu;\n", resume_text) > 0);
        CHECK(fprintf(file, "const uint32_t fixture_source_stack_depth = %uu;\n", stack_depth) > 0);
        CHECK(fprintf(file, "const uint32_t fixture_source_numeric_pause = %uu;\n", numeric_pause) > 0);
        CHECK(fprintf(file, "const uint32_t fixture_source_bound_result = %uu;\n", bound_result) > 0);
        CHECK(fprintf(file, "const uint32_t fixture_source_witness_result = %uu;\n", witness_result) > 0);
        CHECK(fprintf(file, "const uint32_t fixture_source_enum_witness_result = %uu;\n", enum_witness_result) > 0);
        CHECK(fprintf(file, "const uint32_t fixture_source_enum_generic_witness_result = %uu;\n", enum_generic_witness_result) > 0);
        CHECK(fprintf(file, "const uint32_t fixture_source_generic_method_number = %uu;\n", generic_method_number) > 0);
        CHECK(fprintf(file, "const uint32_t fixture_source_generic_method_text = %uu;\n", generic_method_text) > 0);
        CHECK(fprintf(file, "const uint32_t fixture_source_generic_method_array = %uu;\n", generic_method_array) > 0);
        CHECK(fprintf(file,
            "\nconst uint32_t fixture_source_inference_values[6] = {%uu,%uu,%uu,%uu,%uu,%uu};\n"
            "const uint32_t fixture_source_inference_count = %uu;\n",
            inference_entries.values[0],inference_entries.values[1],inference_entries.values[2],
            inference_entries.values[3],inference_entries.values[4],inference_entries.values[5],
            inference_entries.count) > 0);
        CHECK(fprintf(file,
            "\nconst uint32_t fixture_source_method_value_values[6] = {%uu,%uu,%uu,%uu,%uu,%uu};\n"
            "const uint32_t fixture_source_method_value_count = %uu;\n",
            method_value_entries.values[0],method_value_entries.values[1],method_value_entries.values[2],
            method_value_entries.values[3],method_value_entries.values[4],method_value_entries.values[5],
            method_value_entries.count) > 0);
        CHECK(fprintf(file,
            "\nconst uint32_t fixture_source_late_result_values[3] = {%uu,%uu,%uu};\n"
            "const uint32_t fixture_source_late_result_count = %uu;\n",
            late_result_entries.values[0],late_result_entries.values[1],late_result_entries.values[2],late_result_entries.count) > 0);
        CHECK(fclose(file) == 0);
    }
    xr_xir_compile_c_source_free(&source);
    XrXirProgram *program = NULL;
    XrCompileResourceStats before_take={0},after_take={0};
    CHECK(xr_compile_resources_stats(context.resources,&before_take)==XR_COMPILE_RESOURCE_OK);
    XrXirStatus take_status=xr_xir_compile_vm_program_take(&lowered,&program);
    CHECK(xr_compile_resources_stats(context.resources,&after_take)==XR_COMPILE_RESOURCE_OK);
    fprintf(stderr,"packet seal status=%u: allocated %llu->%llu live %llu->%llu peak %llu work %llu->%llu; functions=%u\n",(unsigned)take_status,
        (unsigned long long)before_take.allocated_bytes,(unsigned long long)after_take.allocated_bytes,
        (unsigned long long)before_take.live_bytes,(unsigned long long)after_take.live_bytes,(unsigned long long)after_take.peak_bytes,
        (unsigned long long)before_take.work,(unsigned long long)after_take.work,module->function_count);
    CHECK(take_status==XR_XIR_OK && !lowered);
    XrXirValue results[2] = {{0}, {0}};
    source_pair(program, entry, (SourceFunctions) {result, advance, update, calculate, resume_text, stack_depth, numeric_pause, bound_result, witness_result, enum_witness_result, enum_generic_witness_result, generic_method_number, generic_method_text, generic_method_array}, results);
    runtime_source_failures(program, (RuntimeSourceEntries){entry, resume_text, numeric_pause, enum_witness_result, enum_generic_witness_result, generic_method_number, generic_method_text, generic_method_array});
    XrXirValue inference_retained[2][6] = {{{0}}};
    source_inference_pair(program,inference_entries,inference_retained);
    source_inference_runtime_failures(program,inference_entries);
    XrXirValue method_value_retained[2][6] = {{{0}}};
    source_method_value_pair(program,method_value_entries,method_value_retained);
    source_method_value_runtime_failures(program,method_value_entries);
    XrXirValue late_result_retained[2][3] = {{{0}}};
    source_late_result_pair(program,late_result_entries,late_result_retained);
    source_late_result_runtime_failures(program,late_result_entries);
    xr_xir_compile_program_drop(program);
    source_late_result_retained_drop(late_result_retained);
    source_method_value_retained_drop(method_value_retained);
    source_inference_retained_drop(inference_retained);
    source_result_drop(&results[0]); source_result_drop(&results[1]);
    puts("Source-free Checked consumer matched independent VM expectations and emitted native C");
    CHECK(!runtime_live && !runtime_bytes);
    source_fixture_source_owners_free();
    return 0;
}
