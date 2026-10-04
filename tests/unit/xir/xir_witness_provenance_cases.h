/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_witness_provenance_cases.h - Independent witness substitution evidence
 */
#ifndef XIR_WITNESS_PROVENANCE_CASES_H
#define XIR_WITNESS_PROVENANCE_CASES_H
#include "xir/xxir_internal.h"

static void witness_reject_derived(XrXirArtifact *artifact) {
    CHECK(xr_xir_compile_artifact_verify(artifact, NULL) != XR_XIR_OK);
    if (artifact->module.stage == XR_XIR_CHECKED) {
        XrXirCheckedPacket packet = {0};
        CHECK(xr_xir_compile_checked_write(artifact, &packet, NULL) != XR_XIR_OK && !packet.bytes);
    }
}
static void witness_mutations(XrXirArtifact *artifact) {
    XrXirModule *module = &artifact->module;
    XrXirProvenance *provenance = (XrXirProvenance *)module->provenance;
    CHECK(provenance && provenance->source && !module->types->interfaces &&
        !module->declarations->implementations);
    XrXirModule *source = &provenance->source->module;
    XrXirInstruction *call = NULL;
    uint32_t alternate = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *original = &source->functions[provenance->origins[f].function];
        if (original->name_length == 12 && !memcmp(original->name,"measureOther",12)) alternate = f;
        for (uint32_t i = 0; i < original->instruction_count; ++i)
            if (original->instructions[i].op == XR_XIR_CALL_REQUIREMENT) {
                XrXirInstruction *candidate = &((XrXirInstruction *)module->functions[f].instructions)[i];
                if (!call || provenance->origins[candidate->immediate].argument_count)
                    call = candidate;
            }
    }
    CHECK(call && call->op == XR_XIR_CALL && alternate != UINT32_MAX);
    int64_t selected = call->immediate;
    CHECK(selected != alternate && selected >= 0 && (uint64_t)selected < module->function_count);
    const XrXirFunction *method = &module->functions[selected], *other = &module->functions[alternate];
    CHECK(method->parameter_count == other->parameter_count && method->result == other->result);
    for (uint32_t p = 0; p < method->parameter_count; ++p) CHECK(method->parameters[p] == other->parameters[p]);
    call->immediate = alternate; witness_reject_derived(artifact); call->immediate = selected;
    CHECK(xr_xir_compile_artifact_verify(artifact, NULL) == XR_XIR_OK);
    XrXirDeclarations *declarations = (XrXirDeclarations *)source->declarations;
    const XrXirImplementationTable *implementations = declarations->implementations;
    CHECK(implementations && implementations->count);
    declarations->implementations = NULL; witness_reject_derived(artifact);
    declarations->implementations = implementations;
    CHECK(xr_xir_compile_artifact_verify(artifact, NULL) == XR_XIR_OK);
    uint32_t original_target = provenance->origins[selected].function;
    provenance->origins[selected].function = provenance->origins[alternate].function;
    witness_reject_derived(artifact); provenance->origins[selected].function = original_target;
    CHECK(xr_xir_compile_artifact_verify(artifact, NULL) == XR_XIR_OK);
    XrXirOrigin *origin = &provenance->origins[selected];
    CHECK(origin->argument_count == 1);
    XrXirType *arguments = (XrXirType *)origin->arguments;
    XrXirType saved = arguments[0]; arguments[0] = XR_XIR_I64;
    witness_reject_derived(artifact); arguments[0] = saved;
    CHECK(xr_xir_compile_artifact_verify(artifact, NULL) == XR_XIR_OK);
}
static void witness_provenance_cases(XrXirSourceRequest *request) {
    write_source(request->entry_path,
        "interface Measure { measure()->i64 }\n"
        "struct Meter implements Measure { value:i64; measure()->i64{return this.value} }\n"
        "struct Box<T:Measure> implements Measure { value:T; measure()->i64{return this.value.measure()} "
        "measureOther()->i64{return 99} }\n"
        "fn read<T:Measure>(value:T)->i64{return value.measure()}\n"
        "export fn other(value:Box<Meter>)->i64{return value.measureOther()}\n"
        "export fn measured()->i64{return read<Box<Meter>>(Box<Meter>{value:Meter{value:41}})}\n");
    XrXirSourceResult result = {0};
    CHECK(xr_xir_compile_source_check(request, &result, NULL, NULL) == XR_XIR_OK && result.checked);
    XrXirArtifact *closed = NULL, *copy = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_specialize(result.checked, &closed, NULL) == XR_XIR_OK && closed);
    xr_xir_compile_source_result_free(&result);
    witness_mutations(closed);
    XrXirCheckedPacket packet = {0};
    const XrXirCompileContext packet_context = *xr_xir_compile_artifact_context(closed);
    CHECK(xr_xir_compile_checked_write(closed, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    CHECK(xr_xir_compile_checked_read(&packet_context, packet.bytes, packet.length, &copy, NULL) == XR_XIR_OK && copy);
    xr_xir_compile_checked_packet_free(&packet);
    witness_mutations(copy);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(copy, &target, &lowered, NULL) == XR_XIR_OK && lowered);
    xr_xir_compile_artifact_free(copy);
    witness_mutations(lowered); xr_xir_compile_artifact_free(lowered);
}
#endif // XIR_WITNESS_PROVENANCE_CASES_H
