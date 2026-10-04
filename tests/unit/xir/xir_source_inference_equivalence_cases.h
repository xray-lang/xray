/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_inference_equivalence_cases.h - One instance identity for equal actuals
 *
 * KEY CONCEPT:
 *   Explicit spelling and inference produce the same original requirement tuple.
 */
#ifndef XIR_SOURCE_INFERENCE_EQUIVALENCE_CASES_H
#define XIR_SOURCE_INFERENCE_EQUIVALENCE_CASES_H
#include "xir/xxir_internal.h"
static unsigned inference_equivalence_role(const XrXirFunction *function) {
    const char *names[]={"explicitRead","inferredRead","differentRead"};
    for (unsigned r=0;r<3;++r)
        if (function->name_length==strlen(names[r]) && !memcmp(function->name,names[r],strlen(names[r]))) return r;
    return 3;
}
static void inference_equivalence_original(const XrXirModule *module) {
    unsigned seen[3]={0}; uint32_t declarations[3]={0},members[3]={0};
    for (uint32_t f=0;f<module->function_count;++f) {
        unsigned role=inference_equivalence_role(&module->functions[f]); if (role==3) continue;
        const XrXirGeneric *generic=&module->generics[f];
        for (uint32_t i=0;i<module->functions[f].instruction_count;++i) {
            const XrXirInstruction *call=&module->functions[f].instructions[i];
            if (call->op!=XR_XIR_CALL_REQUIREMENT) continue;
            CHECK(++seen[role]==1 && call->type_arguments[1]==2);
            CHECK(call->type_arguments[0]<=generic->argument_count &&
                2<=generic->argument_count-call->type_arguments[0]);
            const XrXirType *actual=generic->arguments+call->type_arguments[0];
            CHECK(actual[0]==XR_XIR_TYPE_PARAMETER_BASE);
            CHECK(actual[1]==(role==2 ? XR_XIR_BOOL : XR_XIR_I64));
            declarations[role]=call->targets[0]; members[role]=call->targets[1];
        }
    }
    CHECK(seen[0]==1 && seen[1]==1 && seen[2]==1);
    CHECK(declarations[0]==declarations[1] && declarations[1]==declarations[2]);
    CHECK(members[0]==members[1] && members[1]==members[2]);
}
static void inference_equivalence_closed(XrXirArtifact *artifact) {
    XrXirModule *module=&artifact->module;
    const XrXirProvenance *provenance=module->provenance; CHECK(provenance && provenance->source);
    XrXirInstruction *calls[3]={NULL,NULL,NULL};
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *original=&provenance->source->module.functions[provenance->origins[f].function];
        unsigned role=inference_equivalence_role(original); if (role==3) continue;
        for (uint32_t i=0;i<original->instruction_count;++i) {
            if (original->instructions[i].op!=XR_XIR_CALL_REQUIREMENT) continue;
            CHECK(!calls[role]); calls[role]=&((XrXirInstruction *)module->functions[f].instructions)[i];
            CHECK(calls[role]->op==XR_XIR_CALL && calls[role]->immediate>=0 &&
                (uint64_t)calls[role]->immediate<module->function_count);
            const XrXirOrigin *target=&provenance->origins[calls[role]->immediate];
            CHECK(target->argument_count==3 && target->arguments[0]==XR_XIR_I64 &&
                target->arguments[1]==XR_XIR_STRING && target->arguments[2]==(role==2 ? XR_XIR_BOOL : XR_XIR_I64));
        }
    }
    CHECK(calls[0] && calls[1] && calls[2]);
    CHECK(calls[0]->immediate==calls[1]->immediate && calls[1]->immediate!=calls[2]->immediate);
    CHECK(provenance->origins[calls[0]->immediate].function==provenance->origins[calls[2]->immediate].function);
    int64_t saved=calls[1]->immediate; calls[1]->immediate=calls[2]->immediate;
    CHECK(xr_xir_compile_artifact_verify(artifact, NULL)==XR_XIR_BAD_TYPE);
    calls[1]->immediate=saved;
    CHECK(xr_xir_compile_artifact_verify(artifact, NULL)==XR_XIR_OK);
}
static void source_inference_equivalence_cases(XrXirSourceRequest *request) {
    write_source(request->entry_path,
        "interface Probe<A> { measure<U>(value:U)->i64 }\n"
        "struct Box<X,Y> implements Probe<X> { value:i64; measure<V>(ignored:V)->i64{return this.value} }\n"
        "fn explicitRead<A,T:Probe<A>>(box:T)->i64{return box.measure<i64>(41)}\n"
        "fn inferredRead<A,T:Probe<A>>(box:T)->i64{return box.measure(41)}\n"
        "fn differentRead<A,T:Probe<A>>(box:T)->i64{return box.measure(true)}\n"
        "export fn first()->i64{return explicitRead<i64,Box<i64,string>>(Box<i64,string>{value:41})}\n"
        "export fn second()->i64{return inferredRead<i64,Box<i64,string>>(Box<i64,string>{value:41})}\n"
        "export fn third()->i64{return differentRead<i64,Box<i64,string>>(Box<i64,string>{value:41})}\n");
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(request->context->resources, &session) == XR_COMPILER_SESSION_OK);
    XrXirSourceRequest local=*request; local.session=session;
    XrXirSourceResult result={0}; XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check(&local, &result, &diagnostic, NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"inference equivalence: %u %s\n",status,diagnostic.message);
    CHECK(status==XR_XIR_OK && result.checked);
    xr_compile_session_free(session);
    inference_equivalence_original(xr_xir_compile_artifact_module(result.checked));
    XrXirArtifact *closed=NULL,*copy=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_specialize(result.checked, &closed, NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&result); inference_equivalence_closed(closed);
    XrXirCheckedPacket packet={0};
    const XrXirCompileContext packet_context = *xr_xir_compile_artifact_context(closed);
    CHECK(xr_xir_compile_checked_write(closed, &packet, NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    CHECK(xr_xir_compile_checked_read(&packet_context, packet.bytes, packet.length, &copy, NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet); inference_equivalence_closed(copy);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(copy, &target, &lowered, NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(copy); inference_equivalence_closed(lowered); xr_xir_compile_artifact_free(lowered);
}
static void source_inference_phantom_cases(XrXirSourceRequest *request) {
    const char *sources[]={
        "interface Evidence<A>{}\ninterface Probe{measure<U:Evidence<i64>>()->i64}\n"
        "struct Only implements Evidence<i64>{}\nstruct Box implements Probe{measure<V:Evidence<i64>>()->i64{return 41}}\n"
        "fn explicitRead<T:Probe>(box:T)->i64{return box.measure<Only>()}\n"
        "export fn answer()->i64{return explicitRead<Box>(Box{})}\n",
        "interface Evidence<A>{}\ninterface Probe{measure<U:Evidence<i64>>()->i64}\n"
        "struct Only implements Evidence<i64>{}\nstruct Box implements Probe{measure<V:Evidence<i64>>()->i64{return 41}}\n"
        "fn inferredRead<T:Probe>(box:T)->i64{return box.measure()}\n"
        "export fn answer()->i64{return inferredRead<Box>(Box{})}\n"
    };
    XrXirSourceResult described={0};
    for (unsigned mode=0;mode<2;++mode) {
        write_source(request->entry_path,sources[mode]);
        XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(request->context->resources, &session) == XR_COMPILER_SESSION_OK);
        XrXirSourceRequest local=*request; local.session=session;
        XrXirSourceResult result={0}; XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_compile_source_check(&local, &result, &diagnostic, NULL);
        xr_compile_session_free(session);
        if (!mode) {
            CHECK(status==XR_XIR_OK && result.checked && result.snapshot);
            xr_xir_compile_artifact_free(result.checked); result.checked=NULL; described=result;
        } else {
            CHECK(status==XR_XIR_BAD_TYPE && !result.checked && !result.snapshot);
            CHECK(strstr(diagnostic.message,"cannot infer all method type arguments; supply an explicit list"));
            CHECK(!result.snapshot);
            CHECK(xr_xir_compile_source_snapshot_view(described.snapshot)->complete);
            xr_xir_compile_source_result_free(&result);
        }
    }
    xr_xir_compile_source_result_free(&described);
}
#endif // XIR_SOURCE_INFERENCE_EQUIVALENCE_CASES_H

