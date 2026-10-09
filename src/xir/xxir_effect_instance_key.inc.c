/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_instance_key.inc.c - Actual-pool callable vectors for instances
 *
 * KEY CONCEPT:
 *   Ordinary arguments and physical callable arguments are distinct vectors.
 *   These pool-local comparisons are not persistent cache identities.
 */
typedef struct SpecEffectInput {
    const XrXirFunction *function;
    const XrXirInstruction *instruction;
    uint32_t caller, ordinal;
} SpecEffectInput;
typedef struct SpecEffectVector {
    const XrXirEffectArgument *arguments;
    uint32_t count;
} SpecEffectVector;

static bool spec_effect_same(SpecContext *c, const SpecInstance *instance,
    const XrXirEffectArgument *arguments, uint32_t count) {
    if (!spec_work(c,1)) return false;
    if (instance->effect_count != count) return false;
    for (uint32_t a=0;a<count;++a) {
        if (!spec_work(c,2)) return false;
        if (instance->effects[a].parameter != arguments[a].parameter ||
            instance->effects[a].type != arguments[a].type) return false;
    }
    return true;
}

static XrXirType spec_effect_child(SpecContext *c, const XrXirType *cache,
    uint32_t before, XrXirType input) {
    uint32_t id=(uint32_t)input;
    if (!spec_work(c,3)) return XR_XIR_UNIT;
    if (id>=XR_XIR_TYPE_PARAMETER_BASE) {
        c->diagnostic.status=XR_XIR_BAD_TYPE;return XR_XIR_UNIT;
    }
    if (id<XR_XIR_CONSTRUCTED_TYPE_BASE) return input;
    uint32_t index=id-XR_XIR_CONSTRUCTED_TYPE_BASE;
    if (index>=before || !cache) { c->diagnostic.status=XR_XIR_BAD_TYPE;return XR_XIR_UNIT; }
    uint32_t mapped=(uint32_t)cache[index];
    if (mapped<XR_XIR_CONSTRUCTED_TYPE_BASE || mapped>=XR_XIR_CONSTRUCTED_TYPE_LIMIT ||
        mapped-XR_XIR_CONSTRUCTED_TYPE_BASE>=c->types.count) {
        c->diagnostic.status=XR_XIR_BAD_TYPE;return XR_XIR_UNIT;
    }
    return cache[index];
}

/* Rehome complete closed structures in descriptor order. The authentic
 * context retains its open template descriptors, which are not executable
 * instance nodes. Every requested skipped child fails instead of becoming Unit.
 * Semantic declaration identities were checked by the context query. */
static const XrXirType *spec_effect_import(SpecContext *c, const XrXirTypes *types) {
    XrXirType *cache=spec_alloc(c,types->count,sizeof(*cache));
    if (types->count && !cache) return NULL;
    for (uint32_t i=0;i<types->count;++i) {
        if (!spec_work(c,sizeof(XrXirTypeNode))) return NULL;
        XrXirTypeNode node=types->nodes[i];
        if (node.parameter_span) continue;
        if (node.kind==XR_XIR_TYPE_CALLABLE || node.kind==XR_XIR_TYPE_TUPLE) {
            XrXirCallableParameter *parameters=spec_alloc(c,node.parameter_count,sizeof(*parameters));
            if (node.parameter_count && !parameters) return NULL;
            for (uint32_t p=0;p<node.parameter_count;++p) {
                if (!spec_work(c,1)) return NULL;
                parameters[p].mode=node.parameters[p].mode;
                parameters[p].type=spec_effect_child(c,cache,i,node.parameters[p].type);
            }
            node.parameters=parameters;
            if (node.kind==XR_XIR_TYPE_CALLABLE) node.result=spec_effect_child(c,cache,i,node.result);
        } else if (node.kind==XR_XIR_TYPE_NOMINAL) {
            XrXirType *arguments=spec_alloc(c,node.nominal.argument_count,sizeof(*arguments));
            if (node.nominal.argument_count && !arguments) return NULL;
            for (uint32_t a=0;a<node.nominal.argument_count;++a)
                arguments[a]=spec_effect_child(c,cache,i,node.nominal.arguments[a]);
            node.nominal.arguments=arguments;node.nominal.fields=NULL;node.nominal.field_count=0;
        } else node.element=spec_effect_child(c,cache,i,node.element);
        if (c->diagnostic.status!=XR_XIR_OK) return NULL;
        cache[i]=spec_node(c,node);
        if (c->diagnostic.status!=XR_XIR_OK) return NULL;
    }
    return cache;
}

static bool spec_effect_owners(SpecContext *c, uint32_t caller,
    XrXirOrigin **output, uint32_t *count) {
    *output=NULL;*count=0;
    uint32_t owner=c->instances[caller].owner;
    while (owner!=UINT32_MAX) {
        if (!spec_work(c,1)) return false;
        if (owner>=c->count || *count>=c->count) { c->diagnostic.status=XR_XIR_BAD_STRUCTURE;return false; }
        ++*count;owner=c->instances[owner].owner;
    }
    XrXirOrigin *origins=spec_alloc(c,*count,sizeof(*origins));
    if (*count && !origins) return false;
    owner=c->instances[caller].owner;
    for (uint32_t a=0;a<*count;++a) {
        if (!spec_work(c,sizeof(*origins))) return false;
        const SpecInstance *instance=&c->instances[owner];
        origins[a]=(XrXirOrigin){instance->declaration,instance->arguments,instance->count,
            instance->effects,instance->effect_count};
        owner=instance->owner;
    }
    *output=origins;return true;
}

static bool spec_effect_vector(SpecContext *c, uint32_t declaration,
    const XrXirType *ordinary, uint32_t ordinary_count,
    const SpecEffectInput *input, SpecEffectVector *output) {
    *output=(SpecEffectVector){0};
    if (!c->effects) return true;
    const XrXirFunction *definition=&c->source->functions[declaration];
    uint32_t count=definition->parameter_count;
    XrXirEffectArgument *arguments=spec_alloc(c,count,sizeof(*arguments));
    if (count && !arguments) return false;
    if (!input) {
        uint32_t type_count=c->source->types ? c->source->types->count : 0;
        XrXirType *cache=spec_alloc(c,type_count,sizeof(*cache));
        if (type_count && !cache) return false;
        SpecInstance temporary={declaration,ordinary,ordinary_count,cache,NULL,0,UINT32_MAX};
        for (uint32_t p=0;p<count;++p) {
            if (!spec_work(c,2)) return false;
            arguments[p]=(XrXirEffectArgument){p,spec_type(c,&temporary,definition->parameters[p])};
        }
    } else {
        if (input->caller>=c->count) { c->diagnostic.status=XR_XIR_BAD_STRUCTURE;return false; }
        const SpecInstance *instance=&c->instances[input->caller];
        XrXirOrigin origin={instance->declaration,instance->arguments,instance->count,
            instance->effects,instance->effect_count};
        XrXirOrigin *owners=NULL;uint32_t owner_count=0;
        if (!spec_effect_owners(c,input->caller,&owners,&owner_count)) return false;
        XirEffectContextInput request={c->source,&c->types,&origin,input->ordinal,owners,owner_count};
        XirEffectContextView selected={0};
        c->diagnostic.status=xir_effects_context_select(&c->remaining,c->effects,&request,&selected);
        if (c->diagnostic.status!=XR_XIR_OK || selected.declaration!=declaration ||
            selected.parameter_count!=count || selected.argument_count!=ordinary_count) {
            if (c->diagnostic.status==XR_XIR_OK) c->diagnostic.status=XR_XIR_BAD_STRUCTURE;
            return false;
        }
        XrXirTypeMatchScratch scratch={c->remaining.resources,NULL};
        for (uint32_t a=0;a<ordinary_count && c->diagnostic.status==XR_XIR_OK;++a)
            c->diagnostic.status=xr_xir_compile_type_substitution_matches_between_scratch(&c->remaining,
                selected.types,&c->types,NULL,0,selected.arguments[a],ordinary[a],&scratch);
        xr_xir_type_match_scratch_free(&scratch);
        if (c->diagnostic.status!=XR_XIR_OK) return false;
        const XrXirType *cache=spec_effect_import(c,selected.types);
        if (selected.types->count && !cache) return false;
        for (uint32_t p=0;p<count;++p) {
            if (!spec_work(c,2)) return false;
            arguments[p]=(XrXirEffectArgument){p,spec_effect_child(c,cache,selected.types->count,
                selected.physical_types[p])};
        }
    }
    if (c->diagnostic.status!=XR_XIR_OK) return false;
    *output=(SpecEffectVector){arguments,count};return true;
}

#include "xxir_specialize_capture.inc.c"

/* Finish actual scalar dependencies before interning any dependent callee. */
static bool spec_effect_flow(SpecContext *c, uint32_t index) {
    if (!c->effects) return true;
    return spec_capture_flow(c,index);
}

static bool spec_effect_default(SpecContext *c, const SpecInstance *instance, bool *result) {
    *result=instance->count==0;
    for (uint32_t a=0;a<instance->effect_count;++a) {
        if (!spec_work(c,1)) return false;
        const XrXirEffectArgument *argument=&instance->effects[a];
        XrXirType declared=c->source->functions[instance->declaration].parameters[argument->parameter];
        XrXirTypeMatchScratch scratch={c->remaining.resources,NULL};
        XrXirStatus status=xr_xir_compile_type_substitution_matches_between_scratch(&c->remaining,
            c->source->types,&c->types,instance->arguments,instance->count,declared,argument->type,&scratch);
        xr_xir_type_match_scratch_free(&scratch);
        if (status==XR_XIR_BAD_TYPE) *result=false;
        else if (status!=XR_XIR_OK) { c->diagnostic.status=status;return false; }
    }
    return true;
}

static uint32_t spec_effect_family(XrXirOp op) {
    if (op==XR_XIR_CALL || op==XR_XIR_INVOKE || op==XR_XIR_GO) return XR_XIR_EFFECT_BINDING_DIRECT;
    if (op==XR_XIR_FUNCTION_REF) return XR_XIR_EFFECT_BINDING_CAPTURE;
    if (op==XR_XIR_CALL_DEFAULT || op==XR_XIR_INVOKE_DEFAULT) return XR_XIR_EFFECT_BINDING_DEFAULT;
    return op==XR_XIR_CALL_REQUIREMENT ? XR_XIR_EFFECT_BINDING_REQUIREMENT : 0;
}

static bool spec_effect_proofs(SpecContext *c, XrXirEffectBindingProof **output, uint32_t *count) {
    *output=NULL;*count=0;
    if (!c->effects) return true;
    uint32_t total=0;
    for (uint32_t f=0;f<c->count;++f) {
        const XrXirFunction *function=&c->functions[f];
        const XrXirFunction *original=&c->source->functions[c->instances[f].declaration];
        for (uint32_t i=0;i<function->instruction_count;++i) {
            if (!spec_work(c,1)) return false;
            if (!spec_effect_family(original->instructions[i].op)) continue;
            const XrXirInstruction *op=&function->instructions[i];
            if (op->immediate<0 || (uint64_t)op->immediate>=c->count) {
                c->diagnostic.status=XR_XIR_BAD_STRUCTURE;return false;
            }
            const SpecInstance *target=&c->instances[op->immediate];
            for (uint32_t a=0;a<target->effect_count;++a) {
                if (!spec_work(c,1)) return false;
                if (target->effects[a].parameter>=op->args[1]) continue;
                if (total==UINT32_MAX) { c->diagnostic.status=XR_XIR_BUDGET;return false; }
                ++total;
            }
        }
    }
    XrXirEffectBindingProof *proofs=spec_alloc(c,total,sizeof(*proofs));
    if (total && !proofs) return false;
    uint32_t at=0;
    for (uint32_t f=0;f<c->count;++f) {
        const XrXirFunction *function=&c->functions[f];
        const XrXirFunction *original=&c->source->functions[c->instances[f].declaration];
        for (uint32_t family=1;family<=4;++family) {
            for (uint32_t i=0;i<function->instruction_count;++i) {
                if (!spec_work(c,1)) return false;
                if (spec_effect_family(original->instructions[i].op)!=family) continue;
                const XrXirInstruction *op=&function->instructions[i];
                const SpecInstance *target=&c->instances[op->immediate];
                for (uint32_t a=0;a<target->effect_count;++a) {
                    if (!spec_work(c,1)) return false;
                    uint32_t physical=target->effects[a].parameter;
                    if (physical>=op->args[1]) continue;
                    if (!function->operands || op->args[0]>function->operand_count ||
                        op->args[1]>function->operand_count-op->args[0] || at>=total) {
                        c->diagnostic.status=XR_XIR_BAD_STRUCTURE;return false;
                    }
                    if (!spec_work(c,6)) return false;
                    proofs[at++]=(XrXirEffectBindingProof){family,f,i,(uint32_t)op->immediate,
                        physical,function->operands[op->args[0]+physical]};
                }
            }
        }
    }
    if (at!=total) { c->diagnostic.status=XR_XIR_BAD_STRUCTURE;return false; }
    if (!spec_work(c,2)) return false;
    *output=proofs;*count=total;return true;
}
