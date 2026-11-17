/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_indirect_callable_binding_cases.h - Receiver-owned argument COPY proofs
 */
#ifndef XIR_INDIRECT_CALLABLE_BINDING_CASES_H
#define XIR_INDIRECT_CALLABLE_BINDING_CASES_H
#include "base/xsha256.h"
#include "xir/xxir_internal.h"
typedef struct IndirectBindingSite {
    uint32_t function,binding,identity,instruction,parameter,value;
} IndirectBindingSite;
static void *indirect_binding_copy(const void *source,size_t count,size_t width) {
    CHECK(count && width && count<=SIZE_MAX/width && source);
    void *result=xr_malloc(count*width);CHECK(result);
    memcpy(result,source,count*width);return result;
}
static bool indirect_binding_rejected(XrXirStatus status) {
    return status==XR_XIR_BAD_STRUCTURE || status==XR_XIR_BAD_TYPE ||
        status==XR_XIR_BAD_VALUE || status==XR_XIR_BAD_DOMINANCE || status==XR_XIR_BAD_LAYOUT;
}
static void indirect_binding_invalid(const XrXirArtifact *original,const XrXirModule *changed,
    const char *label,uint32_t attack) {
    XrXirArtifact invalid=*original;invalid.module=*changed;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_artifact_verify(&invalid,&diagnostic);
    if(!indirect_binding_rejected(status))fprintf(stderr,"indirect %s attack%u verify status%u f%u b%u i%u\n",
        label,attack,(unsigned)status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(indirect_binding_rejected(status));
    XrXirCheckedPacket packet={0};
    status=xr_xir_compile_checked_write(&invalid,&packet,&diagnostic);
    CHECK(indirect_binding_rejected(status) && !packet.bytes && !packet.length);
}
static IndirectBindingSite indirect_binding_site(const XrXirArtifact *artifact,XrXirOp operation) {
    const XrXirModule *module=&artifact->module;const XrXirProvenance *record=module->provenance;
    CHECK(record && record->kind==XR_XIR_EVIDENCE_TEMPLATE && record->contract_count==module->function_count);
    IndirectBindingSite selected={UINT32_MAX,0,0,0,0,0};uint32_t matches=0;
    for(uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *function=module->functions+f;
        const XrXirFunctionEffectContract *contract=record->contracts+f;
        for(uint32_t b=0;b<contract->binding_count;++b) {
            const XrXirEffectCallBinding *binding=contract->bindings+b;
            if(binding->family!=XR_XIR_EFFECT_BINDING_INDIRECT)continue;
            CHECK(binding->instruction<function->instruction_count);
            const XrXirInstruction *op=function->instructions+binding->instruction;
            CHECK(op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_INVOKE_INDIRECT);
            if(op->op!=operation)continue;
            CHECK(binding->parameter<op->args[1] && binding->value==function->operands[op->args[0]+binding->parameter]);
            CHECK(binding->value>=function->parameter_count);
            uint32_t producer=binding->value-function->parameter_count;
            CHECK(producer<function->instruction_count && function->instructions[producer].op==XR_XIR_COPY);
            uint32_t found=UINT32_MAX;
            for(uint32_t v=0;v<contract->value_count;++v)if(contract->values[v].instruction==producer) {
                CHECK(found==UINT32_MAX);found=v;
            }
            CHECK(found!=UINT32_MAX && contract->values[found].mode==XR_XIR_EFFECT_VALUE_CALL_BIND);
            CHECK(function->instructions[producer].type!=contract->values[found].declared_type);
            CHECK(xir_effect_call_binding_candidate(&artifact->context,module,f,binding->instruction,binding->parameter)==XR_XIR_OK);
            selected=(IndirectBindingSite){f,b,found,binding->instruction,binding->parameter,binding->value};++matches;
        }
    }
    CHECK(matches && selected.function!=UINT32_MAX);return selected;
}
static void indirect_binding_template_attacks(const XrXirArtifact *artifact,IndirectBindingSite site) {
    const XrXirModule *module=&artifact->module;const XrXirProvenance *record=module->provenance;
    const XrXirFunction *function=module->functions+site.function;
    const XrXirFunctionEffectContract *contract=record->contracts+site.function;
    const XrXirInstruction *op=function->instructions+site.instruction;
    uint32_t producer=site.value-function->parameter_count;
    const XrXirTypeNode *receiver=xr_xir_callable_signature(module->types,xr_xir_operand_type(function,(uint32_t)op->immediate));
    CHECK(receiver && receiver>=module->types->nodes && receiver<module->types->nodes+module->types->count);
    size_t receiver_index=(size_t)(receiver-module->types->nodes);
    XrXirType rest_array=XR_XIR_UNIT;
    for(uint32_t t=0;t<module->types->count;++t)
        if(module->types->nodes[t].kind==XR_XIR_TYPE_ARRAY && xr_xir_callable_signature(module->types,module->types->nodes[t].element))
            rest_array=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+t);
    CHECK(rest_array!=XR_XIR_UNIT); /* Real collect(...actions: fn()) storage. */
    for(uint32_t attack=0;attack<21;++attack) {
        XrXirFunction *functions=indirect_binding_copy(module->functions,module->function_count,sizeof(*functions));
        XrXirInstruction *instructions=indirect_binding_copy(function->instructions,function->instruction_count,sizeof(*instructions));
        XrXirFunctionEffectContract *contracts=indirect_binding_copy(record->contracts,record->contract_count,sizeof(*contracts));
        XrXirRootValueIdentity *identities=indirect_binding_copy(contract->values,contract->value_count,sizeof(*identities));
        XrXirEffectCallBinding *bindings=xr_malloc(((size_t)contract->binding_count+1)*sizeof(*bindings));CHECK(bindings);
        memcpy(bindings,contract->bindings,(size_t)contract->binding_count*sizeof(*bindings));
        XrXirTypeNode *nodes=indirect_binding_copy(module->types->nodes,module->types->count,sizeof(*nodes));
        XrXirCallableParameter *parameters=indirect_binding_copy(receiver->parameters,receiver->parameter_count,sizeof(*parameters));
        functions[site.function].instructions=instructions;contracts[site.function].values=identities;
        contracts[site.function].bindings=bindings;nodes[receiver_index].parameters=parameters;
        XrXirTypes types=*module->types;types.nodes=nodes;
        XrXirProvenance provenance=*record;provenance.contracts=contracts;
        XrXirModule changed=*module;changed.functions=functions;changed.types=&types;changed.provenance=&provenance;
        switch(attack) {
            case 0:contracts[site.function].bindings=NULL;contracts[site.function].binding_count=0;break;
            case 1:bindings[site.binding].family=XR_XIR_EFFECT_BINDING_DIRECT;break;
            case 2:bindings[site.binding].instruction=function->instruction_count;break;
            case 3:bindings[site.binding].parameter=receiver->parameter_count;break;
            case 4:bindings[site.binding].value=instructions[producer].args[0];break;
            case 5:identities[site.identity].mode=XR_XIR_EFFECT_VALUE_FIXED;break;
            case 6:identities[site.identity].instruction=site.instruction;break;
            case 7:identities[site.identity].declared_type=XR_XIR_I64;break;
            case 8:bindings[contract->binding_count]=bindings[site.binding];++contracts[site.function].binding_count;break;
            case 9:instructions[producer].op=XR_XIR_FUNCTION_WEAKEN;break;
            case 10:instructions[site.instruction].immediate=(int64_t)instructions[producer].args[0];break;
            case 11:parameters[site.parameter].escaping=1;break;
            case 12:parameters[site.parameter].mode=XR_PARAM_REF;break;
            case 13:parameters[site.parameter].mode=XR_PARAM_MOVE;break;
            case 14:nodes[receiver_index].flags|=XR_XIR_CALLABLE_C_ABI|XR_XIR_CALLABLE_NO_SUSPEND;break;
            case 15:parameters[site.parameter].type=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE;break;
            case 16:parameters[site.parameter].type=rest_array;nodes[receiver_index].flags|=XR_XIR_CALLABLE_REST;break;
            case 17:instructions[site.instruction].type=XR_XIR_ERROR;break;
            case 18:instructions[producer].args[0]=site.value;break;
            case 19:instructions[producer].args[0]=function->parameter_count+function->instruction_count;break;
            case 20:{
                const XrXirTypeNode *nested=xr_xir_callable_signature(module->types,parameters[site.parameter].type);
                CHECK(nested);size_t nested_index=(size_t)(nested-module->types->nodes);
                nodes[nested_index].flags|=XR_XIR_CALLABLE_C_ABI|XR_XIR_CALLABLE_NO_SUSPEND;break;
            }
            default:CHECK(false);
        }
        XrXirStatus status=xir_effect_call_binding_candidate(&artifact->context,&changed,site.function,site.instruction,site.parameter);
        if(!indirect_binding_rejected(status))fprintf(stderr,"indirect template attack%u candidate status%u\n",attack,(unsigned)status);
        CHECK(indirect_binding_rejected(status));indirect_binding_invalid(artifact,&changed,"template",attack);
        xr_free(parameters);xr_free(nodes);xr_free(bindings);xr_free(identities);xr_free(contracts);xr_free(instructions);xr_free(functions);
    }
    CHECK(xr_xir_compile_artifact_verify(artifact,NULL)==XR_XIR_OK);
}
static void indirect_binding_invoke_edges(const XrXirArtifact *artifact,IndirectBindingSite site) {
    const XrXirModule *module=&artifact->module;const XrXirFunction *function=module->functions+site.function;
    const XrXirInstruction *invoke=function->instructions+site.instruction;
    CHECK(invoke->op==XR_XIR_INVOKE_INDIRECT && invoke->type==XR_XIR_I64 && invoke->targets[0]!=invoke->targets[1]);
    uint32_t result=UINT32_MAX,error=UINT32_MAX;
    for(uint32_t i=0;i<function->instruction_count;++i) {
        const XrXirInstruction *projection=function->instructions+i;
        if(projection->immediate!=(int64_t)site.instruction)continue;
        if(projection->op==XR_XIR_INVOKE_RESULT){CHECK(result==UINT32_MAX);result=i;}
        if(projection->op==XR_XIR_INVOKE_ERROR){CHECK(error==UINT32_MAX);error=i;}
    }
    CHECK(result!=UINT32_MAX && error!=UINT32_MAX);
    CHECK(function->instructions[result].type==XR_XIR_I64 && function->instructions[error].type==XR_XIR_ERROR);
    for(uint32_t attack=0;attack<7;++attack) {
        XrXirFunction *functions=indirect_binding_copy(module->functions,module->function_count,sizeof(*functions));
        XrXirInstruction *instructions=indirect_binding_copy(function->instructions,function->instruction_count,sizeof(*instructions));
        functions[site.function].instructions=instructions;
        switch(attack) {
            case 0:instructions[result].type=XR_XIR_ERROR;break;
            case 1:instructions[result].immediate=(int64_t)result;break;
            case 2:instructions[error].type=XR_XIR_I64;break;
            case 3:instructions[error].immediate=(int64_t)error;break;
            case 4:instructions[site.instruction].targets[0]=invoke->targets[1];instructions[site.instruction].targets[1]=invoke->targets[0];break;
            case 5:instructions[site.instruction].targets[1]=invoke->targets[0];break;
            case 6:instructions[site.instruction].targets[1]=function->block_count;break;
            default:CHECK(false);
        }
        XrXirModule changed=*module;changed.functions=functions;
        /* A COPY proof is not a proof of the INVOKE control-flow contract. */
        indirect_binding_invalid(artifact,&changed,"invoke projections/edges",attack);
        xr_free(instructions);xr_free(functions);
    }
}
static void indirect_binding_u32(uint8_t bytes[4],uint32_t value) {
    for(uint32_t b=0;b<4;++b)bytes[b]=(uint8_t)(value>>(8*b));
}
static void indirect_binding_packet_attacks(const XrXirArtifact *artifact,IndirectBindingSite site) {
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(artifact,&packet,NULL)==XR_XIR_OK);
    const XrXirEffectCallBinding row=artifact->module.provenance->contracts[site.function].bindings[site.binding];
    uint8_t encoded[16];indirect_binding_u32(encoded,row.family);indirect_binding_u32(encoded+4,row.instruction);
    indirect_binding_u32(encoded+8,row.parameter);indirect_binding_u32(encoded+12,row.value);
    size_t offset=0,matches=0;CHECK(packet.length>=64+sizeof(encoded));
    for(size_t p=64;p<=packet.length-sizeof(encoded);++p)if(!memcmp(packet.bytes+p,encoded,sizeof(encoded))){offset=p;++matches;}
    CHECK(matches==1);
    for(uint32_t attack=0;attack<4;++attack) {
        uint8_t *bytes=indirect_binding_copy(packet.bytes,packet.length,1);
        const uint32_t replacement[]={XR_XIR_EFFECT_BINDING_DIRECT,UINT32_MAX,UINT32_MAX,UINT32_MAX};
        indirect_binding_u32(bytes+offset+4*attack,replacement[attack]);
        /* Authenticate changed bytes, rather than only testing checksum failure. */
        XrSHA256Context hash;xr_sha256_init(&hash);xr_sha256_update(&hash,bytes,32);
        xr_sha256_update(&hash,bytes+64,packet.length-64);xr_sha256_final(&hash,bytes+32);
        XrXirArtifact *received=NULL;
        XrXirStatus status=xr_xir_compile_checked_read(&effect_context,bytes,packet.length,&received,NULL);
        if(!indirect_binding_rejected(status))fprintf(stderr,"indirect packet attack%u status%u\n",attack,(unsigned)status);
        CHECK(indirect_binding_rejected(status) && !received);xr_free(bytes);
    }
    XrXirArtifact *received=NULL;
    CHECK(xr_xir_compile_checked_read(&effect_context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    (void)indirect_binding_site(received,artifact->module.functions[site.function].instructions[site.instruction].op);
    xr_xir_compile_artifact_free(received);xr_xir_compile_checked_packet_free(&packet);
}
static uint32_t indirect_binding_instance_sites(const XrXirArtifact *artifact,XrXirOp operation,bool attack_inputs) {
    const XrXirModule *module=&artifact->module;const XrXirProvenance *record=module->provenance;
    CHECK(record && record->kind==XR_XIR_EVIDENCE_INSTANCE);uint32_t matches=0;
    for(uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *function=module->functions+f;
        for(uint32_t i=0;i<function->instruction_count;++i) {
            const XrXirInstruction *op=function->instructions+i;if(op->op!=operation)continue;
            const XrXirTypeNode *receiver=xr_xir_callable_signature(module->types,xr_xir_operand_type(function,(uint32_t)op->immediate));
            CHECK(receiver);
            for(uint32_t p=0;p<receiver->parameter_count;++p) {
                if(!xr_xir_callable_signature(module->types,receiver->parameters[p].type))continue;
                CHECK(xir_effect_call_binding_candidate(&artifact->context,module,f,i,p)==XR_XIR_OK);++matches;
                if(!attack_inputs)continue;
                uint32_t actual=function->operands[op->args[0]+p];CHECK(actual>=function->parameter_count);
                uint32_t producer=actual-function->parameter_count;CHECK(function->instructions[producer].op==XR_XIR_COPY);
                for(uint32_t attack=0;attack<4;++attack) {
                    XrXirFunction *functions=indirect_binding_copy(module->functions,module->function_count,sizeof(*functions));
                    XrXirInstruction *instructions=indirect_binding_copy(function->instructions,function->instruction_count,sizeof(*instructions));
                    uint32_t *operands=indirect_binding_copy(function->operands,function->operand_count,sizeof(*operands));
                    functions[f].instructions=instructions;functions[f].operands=operands;
                    if(attack==0)instructions[i].immediate=(int64_t)instructions[producer].args[0];
                    if(attack==1)instructions[producer].args[0]=actual;
                    if(attack==2)operands[op->args[0]+p]=instructions[producer].args[0];
                    if(attack==3)instructions[producer].op=XR_XIR_FUNCTION_WEAKEN;
                    XrXirModule changed=*module;changed.functions=functions;
                    CHECK(indirect_binding_rejected(xir_effect_call_binding_candidate(&artifact->context,&changed,f,i,p)));
                    indirect_binding_invalid(artifact,&changed,"instance actual identity",attack);
                    xr_free(operands);xr_free(instructions);xr_free(functions);
                }
            }
        }
    }
    CHECK(matches);return matches;
}
static void indirect_binding_roundtrip(const char *file,XrXirOp operation,bool has_rest) {
    XrXirArtifact *source=root_source(file,XR_XIR_OK);IndirectBindingSite site=indirect_binding_site(source,operation);
    if(has_rest)indirect_binding_template_attacks(source,site);
    if(operation==XR_XIR_INVOKE_INDIRECT)indirect_binding_invoke_edges(source,site);
    indirect_binding_packet_attacks(source,site);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source,&packet,NULL)==XR_XIR_OK);
    XrXirArtifact *decoded=NULL;CHECK(xr_xir_compile_checked_read(&effect_context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(source);xr_xir_compile_checked_packet_free(&packet);
    XrXirArtifact *instance=NULL;CHECK(xr_xir_compile_specialize(decoded,&instance,NULL,NULL)==XR_XIR_OK && instance);
    xr_xir_compile_artifact_free(decoded);CHECK(xr_xir_compile_artifact_verify(instance,NULL)==XR_XIR_OK);
    uint32_t sites=indirect_binding_instance_sites(instance,operation,true);
    CHECK(xr_xir_compile_checked_write(instance,&packet,NULL)==XR_XIR_OK);
    decoded=NULL;CHECK(xr_xir_compile_checked_read(&effect_context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(instance);xr_xir_compile_checked_packet_free(&packet);
    CHECK(indirect_binding_instance_sites(decoded,operation,false)==sites);
    CHECK(xr_xir_compile_artifact_verify(decoded,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(decoded);
}
static void indirect_callable_binding_cases(void) {
    EffectMark baseline=effect_mark();
    XrXirArtifact *opaque=root_source("indirect_binding_opaque.xr",XR_XIR_OK);
    (void)indirect_binding_site(opaque,XR_XIR_CALL_INDIRECT);xr_xir_compile_artifact_free(opaque);
    indirect_binding_roundtrip("indirect_binding_reachable.xr",XR_XIR_CALL_INDIRECT,true);
    indirect_binding_roundtrip("indirect_binding_invoke.xr",XR_XIR_INVOKE_INDIRECT,false);
    XrXirArtifact *rejected=root_source("reject_indirect_binding_generic.xr",XR_XIR_BAD_TYPE);CHECK(!rejected);
    rejected=root_source("reject_indirect_binding_rest.xr",XR_XIR_BAD_TYPE);CHECK(!rejected);
    effect_mark_check(baseline);
    puts("INDIRECT_BIND real COPY/signature, Instance and authenticated-packet refusals, INVOKE result/error edges and storage boundaries PASS");
}
#endif
