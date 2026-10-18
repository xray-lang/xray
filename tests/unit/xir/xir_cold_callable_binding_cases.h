/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cold_callable_binding_cases.h - Authentic cold argument records and refusals
 */
#ifndef XIR_COLD_CALLABLE_BINDING_CASES_H
#define XIR_COLD_CALLABLE_BINDING_CASES_H
#include "xir/xxir_internal.h"
static void cold_binding_negative(const XrXirArtifact *artifact,uint32_t f,uint32_t b,uint32_t v) {
    const XrXirModule *module=&artifact->module;const XrXirProvenance *original=module->provenance;
    const XrXirFunctionEffectContract *source=&original->contracts[f];
    CHECK(original->contract_count<=64 && source->binding_count<16 && source->value_count<=96);
    const XrXirEffectCallBinding saved=source->bindings[b];
    uint32_t target=(uint32_t)module->functions[f].instructions[saved.instruction].immediate;
    CHECK(target<original->contract_count && original->contracts[target].parameter_count<=8);
    XrXirRootValueIdentity preserved=source->values[v];
    for(uint32_t attack=0;attack<10;++attack) {
        XrXirFunctionEffectContract contracts[64];XrXirRootValueIdentity values[96];
        XrXirEffectCallBinding bindings[16];XrXirEffectParameter parameters[8];
        memcpy(contracts,original->contracts,(size_t)original->contract_count*sizeof(*contracts));
        memcpy(values,source->values,(size_t)source->value_count*sizeof(*values));
        memcpy(bindings,source->bindings,(size_t)source->binding_count*sizeof(*bindings));
        memcpy(parameters,original->contracts[target].parameters,(size_t)original->contracts[target].parameter_count*sizeof(*parameters));
        contracts[f].values=values;contracts[f].bindings=bindings;contracts[target].parameters=parameters;
        if(attack==0){contracts[f].bindings=NULL;contracts[f].binding_count=0;}
        if(attack==1)bindings[b].family=XR_XIR_EFFECT_BINDING_CAPTURE;
        if(attack==2)++bindings[b].instruction;
        if(attack==3)++bindings[b].parameter;
        if(attack==4)--bindings[b].value;
        if(attack==5)values[v].declared_type=module->functions[f].instructions[preserved.instruction].type;
        if(attack==6)values[v].mode=XR_XIR_EFFECT_VALUE_FIXED;
        if(attack==7){bindings[source->binding_count]=saved;++contracts[f].binding_count;}
        if(attack==8)parameters[saved.parameter].kind=XR_XIR_EFFECT_PARAMETER_FIXED;
        if(attack==9)values[v].declared_type=XR_XIR_I64;
        XrXirProvenance provenance=*original;provenance.contracts=contracts;
        XrXirModule changed=*module;changed.provenance=&provenance;
        XrXirStatus candidate=xir_effect_call_binding_candidate(&artifact->context,&changed,f,saved.instruction,saved.parameter);
        if(candidate!=XR_XIR_BAD_TYPE && candidate!=XR_XIR_BAD_STRUCTURE)fprintf(stderr,"cold binding candidate attack%u status%u\n",attack,candidate);
        CHECK(candidate==XR_XIR_BAD_TYPE || candidate==XR_XIR_BAD_STRUCTURE);
        XrXirArtifact invalid=*artifact;invalid.module=changed;
        XrXirStatus verified=xr_xir_compile_artifact_verify(&invalid,NULL);
        if(verified!=XR_XIR_BAD_TYPE && verified!=XR_XIR_BAD_STRUCTURE)fprintf(stderr,"cold binding full attack%u status%u\n",attack,verified);
        CHECK(verified==XR_XIR_BAD_TYPE || verified==XR_XIR_BAD_STRUCTURE);
        CHECK(!memcmp(&preserved,&source->values[v],sizeof(preserved)));
        CHECK(!memcmp(&saved,&source->bindings[b],sizeof(saved)));
    }
    CHECK(xr_xir_compile_artifact_verify(artifact,NULL)==XR_XIR_OK);
    CHECK(xir_effect_call_binding_candidate(&artifact->context,module,f,saved.instruction,saved.parameter)==XR_XIR_OK);
}
static void cold_binding_inspect(const XrXirArtifact *artifact) {
    const XrXirModule *module=&artifact->module;const XrXirProvenance *p=module->provenance;
    CHECK(p && p->kind==XR_XIR_EVIDENCE_TEMPLATE && p->contract_count==module->function_count);
    uint32_t count=0,selected=UINT32_MAX,binding=UINT32_MAX,value=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *function=&module->functions[f];const XrXirFunctionEffectContract *contract=&p->contracts[f];
        for(uint32_t b=0;b<contract->binding_count;++b) {
            const XrXirEffectCallBinding *actual=&contract->bindings[b];
            CHECK(actual->instruction<function->instruction_count);const XrXirInstruction *op=&function->instructions[actual->instruction];
            if(op->op!=XR_XIR_GENERATOR_NEW)continue;
            CHECK(actual->family==XR_XIR_EFFECT_BINDING_DIRECT && actual->parameter<op->args[1]);
            CHECK(actual->value==function->operands[op->args[0]+actual->parameter] && actual->value>=function->parameter_count);
            uint32_t producer=actual->value-function->parameter_count;CHECK(producer<function->instruction_count);
            const XrXirInstruction *copy=&function->instructions[producer];CHECK(copy->op==XR_XIR_COPY);
            const XrXirTypeNode *precise=xr_xir_callable_signature(module->types,copy->type);CHECK(precise);
            CHECK((precise->flags&XR_XIR_CALLABLE_ROOT_MASK)==XR_XIR_CALLABLE_ROOT_NONE);
            uint32_t matches=0;
            for(uint32_t v=0;v<contract->value_count;++v)if(contract->values[v].instruction==producer) {
                const XrXirRootValueIdentity *identity=&contract->values[v];
                const XrXirTypeNode *bound=xr_xir_callable_signature(module->types,identity->declared_type);
                CHECK(identity->mode==XR_XIR_EFFECT_VALUE_CALL_BIND && bound);
                CHECK((bound->flags&XR_XIR_CALLABLE_ROOT_MASK)==XR_XIR_CALLABLE_ROOT_UNRESOLVED);++matches;
                selected=f;binding=b;value=v;
            }
            CHECK(matches==1 && xir_effect_call_binding_candidate(&artifact->context,module,f,actual->instruction,actual->parameter)==XR_XIR_OK);++count;
        }
    }
    CHECK(count==3 && selected!=UINT32_MAX);cold_binding_negative(artifact,selected,binding,value);
    puts("COLD_FN_BIND 3 real cold COPY identities, original UNKNOWN bounds, 10 candidate/full-verifier attacks and restored positives PASS");
}
#endif
