/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_requirement_value_provenance.h - Bound helper origin poisoning
 *
 * KEY CONCEPT:
 *   Callable helper tuples and original requirement tuples have distinct authority.
 */
#ifndef XIR_SOURCE_REQUIREMENT_VALUE_PROVENANCE_H
#define XIR_SOURCE_REQUIREMENT_VALUE_PROVENANCE_H
#include "xir/xxir_internal.h"
static bool requirement_value_named(const XrXirFunction *function,const char *name) {
    return function->name_length==strlen(name) && !memcmp(function->name,name,function->name_length);
}
static void requirement_value_same_signature(const XrXirFunction *left,const XrXirFunction *right) {
    CHECK(left->parameter_count==right->parameter_count && left->result==right->result);
    for (uint32_t p=0;p<left->parameter_count;++p) CHECK(left->parameters[p]==right->parameters[p]);
}
static void requirement_value_poison(XrXirArtifact *artifact) {
    XrXirModule *module=&artifact->module;
    XrXirProvenance *proof=(XrXirProvenance *)module->provenance;
    CHECK(proof && proof->source && proof->count==module->function_count);
    XrXirModule *source=&proof->source->module;
    XrXirInstruction *references[2]={NULL,NULL}; uint32_t helpers[2]={UINT32_MAX,UINT32_MAX};
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *original=&source->functions[proof->origins[f].function];
        uint32_t slot=requirement_value_named(original,"bind") ? 0 : requirement_value_named(original,"bindOther") ? 1 : 2;
        if (slot==2) continue;
        for (uint32_t i=0;i<original->instruction_count;++i) {
            if (original->instructions[i].op!=XR_XIR_FUNCTION_REF) continue;
            CHECK(!references[slot]); references[slot]=&((XrXirInstruction *)module->functions[f].instructions)[i];
            CHECK(references[slot]->immediate>=0 && (uint64_t)references[slot]->immediate<module->function_count);
            helpers[slot]=(uint32_t)references[slot]->immediate;
        }
    }
    CHECK(references[0] && references[1] && helpers[0]!=helpers[1]);
    requirement_value_same_signature(&module->functions[helpers[0]],&module->functions[helpers[1]]);
    CHECK(proof->origins[helpers[0]].argument_count==4 && proof->origins[helpers[1]].argument_count==4);
    int64_t original_target=references[0]->immediate;
    references[0]->immediate=helpers[1]; witness_reject_derived(artifact);
    references[0]->immediate=original_target; CHECK(xr_xir_artifact_verify(artifact,NULL,NULL)==XR_XIR_OK);
    XrXirOrigin *origin=&proof->origins[helpers[0]];
    XrXirType *tuple=(XrXirType *)origin->arguments;
    XrXirType original_key=tuple[2],other_key=proof->origins[helpers[1]].arguments[2];
    CHECK(original_key!=other_key);
    /* Both K arguments implement Evidence<A> and do not occur in the physical
     * helper signature. Only original caller-to-helper correspondence fixes K. */
    tuple[2]=other_key; witness_reject_derived(artifact); tuple[2]=original_key;
    CHECK(xr_xir_artifact_verify(artifact,NULL,NULL)==XR_XIR_OK);
    uint32_t helper_source=origin->function;
    XrXirFunction *original=&((XrXirFunction *)source->functions)[helper_source];
    XrXirInstruction *requirement=NULL,*call=NULL,*alternate=NULL;
    const XrXirFunction *other_source=&source->functions[proof->origins[helpers[1]].function];
    for (uint32_t i=0;i<original->instruction_count;++i)
        if (original->instructions[i].op==XR_XIR_CALL_REQUIREMENT) {
            CHECK(!requirement); requirement=&((XrXirInstruction *)original->instructions)[i];
            call=&((XrXirInstruction *)module->functions[helpers[0]].instructions)[i];
        }
    for (uint32_t i=0;i<other_source->instruction_count;++i)
        if (other_source->instructions[i].op==XR_XIR_CALL_REQUIREMENT)
            alternate=&((XrXirInstruction *)module->functions[helpers[1]].instructions)[i];
    CHECK(requirement && call && alternate && call->op==XR_XIR_CALL && alternate->op==XR_XIR_CALL);
    CHECK(requirement->type_arguments[1]==2 && source->generics[helper_source].parameter_count==4);
    CHECK(proof->origins[call->immediate].argument_count==3);
    XrXirType *application=(XrXirType *)source->generics[helper_source].arguments+requirement->type_arguments[0];
    CHECK(application[0]==XR_XIR_TYPE_PARAMETER_BASE && application[1]==XR_XIR_TYPE_PARAMETER_BASE+3);
    requirement_value_same_signature(&module->functions[call->immediate],&module->functions[alternate->immediate]);
    int64_t method=call->immediate;
    call->immediate=alternate->immediate; witness_reject_derived(artifact); call->immediate=method;
    CHECK(xr_xir_artifact_verify(artifact,NULL,NULL)==XR_XIR_OK);
    uint32_t member=requirement->targets[1];
    CHECK(member==0); requirement->targets[1]=1;
    /* Changing map to identically typed other remains a legal original program,
     * but no longer justifies the already-derived map implementation target. */
    CHECK(xr_xir_artifact_verify(proof->source,NULL,NULL)==XR_XIR_OK);
    witness_reject_derived(artifact); requirement->targets[1]=member;
    CHECK(xr_xir_artifact_verify(artifact,NULL,NULL)==XR_XIR_OK);
    XrXirType own=application[1]; application[1]=application[0];
    /* A and U happen to close to i64, but only U has a definition-site Sendable
     * premise. Concrete equality cannot repair the poisoned symbolic origin. */
    CHECK(xr_xir_artifact_verify(proof->source,NULL,NULL)==XR_XIR_BAD_TYPE);
    witness_reject_derived(artifact); application[1]=own;
    CHECK(xr_xir_artifact_verify(artifact,NULL,NULL)==XR_XIR_OK);
}
static void source_requirement_value_provenance(XrXirSourceRequest *request) {
    write_source(request->entry_path,
        "interface Evidence<A>{}\n"
        "interface I<A>{map<U:Sendable>(seed:A,value:U)->U;other<V:Sendable>(seed:A,value:V)->V}\n"
        "struct Token implements Evidence<i64>{}\n"
        "struct OtherToken implements Evidence<i64>{}\n"
        "struct Adapter<X,Y> implements I<Y>{map<U:Sendable>(seed:Y,value:U)->U{return value};other<V:Sendable>(seed:Y,value:V)->V{return value}}\n"
        "fn bind<A,T:I<A>,K:Evidence<A>,U:Sendable>(receiver:T,key:K)->fn(A,U)->U{return receiver.map<U>}\n"
        "fn bindOther<A,T:I<A>,K:Evidence<A>,U:Sendable>(receiver:T,key:K)->fn(A,U)->U{return receiver.other<U>}\n"
        "export fn number()->i64{const f=bind<i64,Adapter<string,i64>,Token,i64>(Adapter<string,i64>{},Token{});return f(0,41)}\n"
        "export fn otherNumber()->i64{const f=bindOther<i64,Adapter<string,i64>,OtherToken,i64>(Adapter<string,i64>{},OtherToken{});return f(0,41)}\n");
    XrXirSourceResult result={0}; XrXirSourceDiagnostic diagnostic={0};
    CHECK(xr_xir_source_check(request,&result,&diagnostic)==XR_XIR_OK && result.checked);
    XrXirArtifact *closed=NULL,*copy=NULL,*lowered=NULL;
    CHECK(xr_xir_specialize(result.checked,NULL,&closed,NULL)==XR_XIR_OK);
    xr_xir_source_result_free(&result); requirement_value_poison(closed);
    XrXirCheckedPacket packet={0};
    CHECK(xr_xir_checked_write(closed,NULL,&packet,NULL)==XR_XIR_OK); xr_xir_artifact_free(closed);
    CHECK(xr_xir_checked_read(packet.bytes,packet.length,NULL,&copy,NULL)==XR_XIR_OK);
    xr_xir_checked_packet_free(&packet); requirement_value_poison(copy);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(copy,&target,NULL,&lowered,NULL)==XR_XIR_OK); xr_xir_artifact_free(copy);
    requirement_value_poison(lowered); xr_xir_artifact_free(lowered);
}
#endif // XIR_SOURCE_REQUIREMENT_VALUE_PROVENANCE_H
