/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generic_witness_provenance_cases.h - Complete method instance origins
 *
 * KEY CONCEPT:
 *   Identical physical signatures do not make different method substitutions
 *   interchangeable, including parameters unused by the value signature.
 */
#ifndef XIR_GENERIC_WITNESS_PROVENANCE_CASES_H
#define XIR_GENERIC_WITNESS_PROVENANCE_CASES_H
#include "xir/xxir_internal.h"

static void generic_witness_origin_mutations(XrXirArtifact *artifact) {
    XrXirModule *module = &artifact->module;
    XrXirProvenance *provenance = (XrXirProvenance *)module->provenance;
    CHECK(provenance && provenance->source && !module->types->interfaces &&
        !module->declarations->implementations);
    const XrXirModule *source = &provenance->source->module;
    XrXirInstruction *calls[2] = {NULL,NULL};
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *original = &source->functions[provenance->origins[f].function];
        for (uint32_t i = 0; i < original->instruction_count; ++i) {
            if (original->instructions[i].op != XR_XIR_CALL_REQUIREMENT) continue;
            XrXirInstruction *call = &((XrXirInstruction *)module->functions[f].instructions)[i];
            CHECK(call->op == XR_XIR_CALL && call->immediate >= 0 &&
                (uint64_t)call->immediate < module->function_count);
            const XrXirOrigin *target = &provenance->origins[call->immediate];
            CHECK(target->argument_count == 4 && target->arguments[0] == XR_XIR_I64 &&
                target->arguments[1] == XR_XIR_STRING);
            uint32_t which = target->arguments[2] == XR_XIR_I64 ? 0 : 1;
            CHECK(target->arguments[2] == (which ? XR_XIR_STRING : XR_XIR_I64) &&
                target->arguments[3] == (which ? XR_XIR_I64 : XR_XIR_STRING));
            CHECK(!calls[which]); calls[which] = call;
        }
    }
    CHECK(calls[0] && calls[1] && calls[0]->immediate != calls[1]->immediate);
    const XrXirFunction *a = &module->functions[calls[0]->immediate];
    const XrXirFunction *b = &module->functions[calls[1]->immediate];
    CHECK(a->parameter_count == b->parameter_count && a->result == b->result);
    for (uint32_t p = 0; p < a->parameter_count; ++p) CHECK(a->parameters[p] == b->parameters[p]);
    CHECK(provenance->origins[calls[0]->immediate].function ==
        provenance->origins[calls[1]->immediate].function);
    int64_t saved = calls[0]->immediate;
    calls[0]->immediate = calls[1]->immediate;
    CHECK(xr_xir_compile_artifact_verify(artifact, NULL) == XR_XIR_BAD_TYPE);
    if (module->stage == XR_XIR_CHECKED) {
        XrXirCheckedPacket rejected = {0};
        CHECK(xr_xir_compile_checked_write(artifact, &rejected, NULL) == XR_XIR_BAD_TYPE && !rejected.bytes);
    }
    calls[0]->immediate = saved;
    CHECK(xr_xir_compile_artifact_verify(artifact, NULL) == XR_XIR_OK);
}
static void generic_witness_provenance_cases(XrXirSourceRequest *request) {
    write_source(request->entry_path,
        "interface Probe<A> { measure<U,V>()->i64 }\n"
        "struct Box<X,Y> implements Probe<X> { value:i64; "
        "measure<W,Z>()->i64{return this.value} }\n"
        "fn read<A,T:Probe<A>,U,V>(value:T)->i64{return value.measure<U,V>()}\n"
        "export fn first()->i64{return read<i64,Box<i64,string>,i64,string>(Box<i64,string>{value:41})}\n"
        "export fn second()->i64{return read<i64,Box<i64,string>,string,i64>(Box<i64,string>{value:41})}\n");
    XrXirSourceResult result = {0};
    CHECK(xr_xir_compile_source_check(request, &result, NULL, NULL) == XR_XIR_OK && result.checked);
    XrXirArtifact *closed = NULL, *copy = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_specialize(result.checked, &closed, NULL) == XR_XIR_OK && closed);
    xr_xir_compile_source_result_free(&result);
    generic_witness_origin_mutations(closed);
    XrXirCheckedPacket packet = {0};
    const XrXirCompileContext packet_context = *xr_xir_compile_artifact_context(closed);
    CHECK(xr_xir_compile_checked_write(closed, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    CHECK(xr_xir_compile_checked_read(&packet_context, packet.bytes, packet.length, &copy, NULL) == XR_XIR_OK && copy);
    xr_xir_compile_checked_packet_free(&packet);
    generic_witness_origin_mutations(copy);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(copy, &target, &lowered, NULL) == XR_XIR_OK && lowered);
    xr_xir_compile_artifact_free(copy);
    generic_witness_origin_mutations(lowered);
    xr_xir_compile_artifact_free(lowered);
}
#endif // XIR_GENERIC_WITNESS_PROVENANCE_CASES_H
