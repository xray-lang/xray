/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_context.inc.c - Authentic ordinary environments for effect contexts
 *
 * KEY CONCEPT:
 *   A real instruction resolves through constraints and explicit implementation
 *   evidence. Neither a stored effect kind nor this lookup proves body effects.
 */
#include "xxir_implementation_verify.h"

typedef struct EffectContextRequest {
    const XrXirModule *source;
    const XrXirTypes *actual_types;
    const XrXirType *arguments;
    uint32_t argument_count, function, instruction;
} EffectContextRequest;

typedef struct EffectContextResolved {
    EffectTerms terms;
    XrCompileResources *resources;
    const XrXirType *arguments;
    const XrXirType *origin_arguments;
    uint32_t argument_count, function, instruction, target, family;
    uint32_t origin_argument_count;
    bool known;
} EffectContextResolved;

#ifdef XR_XIR_EFFECT_CONTEXT_TESTS
static void effect_context_free(EffectContextResolved *resolved) {
    if (!resolved) return;
    effect_terms_free(&resolved->terms);
    xr_compile_resources_free(resolved);
}

#endif

static uint32_t effect_context_family(XrXirOp op) {
    if (op==XR_XIR_CALL || op==XR_XIR_INVOKE || op==XR_XIR_GO) return XR_XIR_EFFECT_BINDING_DIRECT;
    if (op==XR_XIR_FUNCTION_REF) return XR_XIR_EFFECT_BINDING_CAPTURE;
    if (op==XR_XIR_CALL_DEFAULT || op==XR_XIR_INVOKE_DEFAULT) return XR_XIR_EFFECT_BINDING_DEFAULT;
    if (op==XR_XIR_CLEANUP_REGISTER) return 5;
    return op==XR_XIR_CALL_REQUIREMENT ? XR_XIR_EFFECT_BINDING_REQUIREMENT : 0;
}

static XrXirStatus effect_context_arguments(EffectContextResolved *resolved,
    const EffectContextRequest *request, XrXirGeneric *environment) {
    const XrXirType *cache=NULL;
    bool local=request->actual_types==request->source->types ||
        (request->actual_types->nodes==resolved->terms.types.nodes &&
         request->actual_types->count<=resolved->terms.types.count);
    XrXirStatus status=local ? XR_XIR_OK : effect_terms_import(&resolved->terms,request->actual_types,&cache);
    if (status!=XR_XIR_OK) return status;
    XrXirType *arguments=request->argument_count ?
        effect_terms_alloc(&resolved->terms,request->argument_count,sizeof(*arguments)) : NULL;
    if (request->argument_count && !arguments) return resolved->terms.status;
    for (uint32_t a=0;a<request->argument_count;++a) {
        if (local) {
            if (!xir_compile_work(resolved->terms.remaining,1)) return XR_XIR_BUDGET;
            arguments[a]=request->arguments[a];
        } else status=effect_terms_import_child(&resolved->terms,cache,request->actual_types->count,
            request->arguments[a],&arguments[a]);
        if (status!=XR_XIR_OK) return status;
        if (xr_xir_type_span(&resolved->terms.types,arguments[a])) resolved->known=false;
    }
    *environment=(XrXirGeneric){.arguments=arguments,.argument_count=request->argument_count};
    resolved->origin_arguments=arguments;resolved->origin_argument_count=request->argument_count;
    return XR_XIR_OK;
}

static XrXirStatus effect_context_call_arguments(EffectContextResolved *resolved,
    const EffectContextRequest *request, const XrXirGeneric *environment, XrXirGeneric *call) {
    const XrXirGeneric *generic=request->source->generics ?
        &request->source->generics[request->function] : NULL;
    const XrXirInstruction *op=&request->source->functions[request->function].instructions[request->instruction];
    uint32_t stored=generic ? generic->argument_count : 0, count=op->type_arguments[1];
    if (op->type_arguments[0]>stored || count>stored-op->type_arguments[0]) return XR_XIR_BAD_STRUCTURE;
    XrXirType *arguments=count ? effect_terms_alloc(&resolved->terms,count,sizeof(*arguments)) : NULL;
    if (count && !arguments) return resolved->terms.status;
    for (uint32_t a=0;a<count;++a) {
        XrXirStatus status=effect_terms_substitute(&resolved->terms,
            generic->arguments[op->type_arguments[0]+a],environment,&arguments[a]);
        if (status!=XR_XIR_OK) return status;
        if (xr_xir_type_span(&resolved->terms.types,arguments[a])) resolved->known=false;
    }
    *call=(XrXirGeneric){.arguments=arguments,.argument_count=count};
    return XR_XIR_OK;
}

static XrXirStatus effect_context_target_constraints(EffectContextResolved *resolved,
    const EffectContextRequest *request, const XrXirProofContext *proof) {
    const XrXirGeneric *generic=request->source->generics ?
        &request->source->generics[resolved->target] : NULL;
    uint32_t count=generic ? generic->parameter_count : 0;
    if (resolved->argument_count!=count) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t p=0;p<count;++p) {
        XrXirConstraintUse use={request->source,{XR_XIR_CONTEXT_FUNCTION,resolved->target,0},
            p,resolved->arguments,count};
        XrXirStatus status=xr_xir_compile_constraints_prove(resolved->terms.remaining,proof,&use);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_context_requirement(EffectContextResolved *resolved,
    const EffectContextRequest *request, const XrXirGeneric *environment, const XrXirGeneric *call) {
    const XrXirModule *source=request->source;
    const XrXirFunction *function=&source->functions[request->function];
    const XrXirInstruction *op=&function->instructions[request->instruction];
    const XrXirInterfaceTable *interfaces=source->types->interfaces;
    if (!interfaces || op->targets[0]>=interfaces->count) return XR_XIR_BAD_STRUCTURE;
    const XrXirInterfaceDeclaration *interface=&interfaces->declarations[op->targets[0]];
    if (op->targets[1]>=interface->method_count || !op->args[1] || !function->operands ||
        op->args[0]>function->operand_count || op->args[1]>function->operand_count-op->args[0])
        return XR_XIR_BAD_STRUCTURE;
    const XrXirInterfaceMethod *method=&interface->methods[op->targets[1]];
    uint32_t parent=interface->parameter_count, own=method->own_parameter_count;
    if (parent>65536 || own>65536-parent || call->argument_count!=parent+own)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t value=function->operands[op->args[0]];
    if ((uint64_t)value>=(uint64_t)function->parameter_count+function->instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    XrXirType receiver=XR_XIR_UNIT;
    XrXirStatus status=effect_terms_substitute(&resolved->terms,xr_xir_operand_type(function,value),
        environment,&receiver);
    if (status!=XR_XIR_OK) return status;
    if (xr_xir_type_span(&resolved->terms.types,receiver)) resolved->known=false;
    if (!resolved->known) return XR_XIR_OK;
    XrXirModule actual=*source;actual.types=&resolved->terms.types;
    XrXirProofContext proof={&actual,{XR_XIR_CONTEXT_CLOSED,0,0}};
    for (uint32_t a=0;a<own;++a) {
        XrXirConstraintUse use={source,{XR_XIR_CONTEXT_INTERFACE_METHOD,op->targets[0],op->targets[1]},
            parent+a,call->arguments,call->argument_count};
        status=xr_xir_compile_constraints_prove(resolved->terms.remaining,&proof,&use);
        if (status!=XR_XIR_OK) return status;
    }
    XrXirWitnessRequest witness_request={source,receiver,
        {op->targets[0],parent ? call->arguments : NULL,parent},op->targets[1]};
    XrXirWitness witness={0};
    status=xr_xir_compile_witness_resolve(resolved->terms.remaining,&proof,&witness_request,&witness);
    if (status!=XR_XIR_OK) return status;
    if (witness.function>=source->function_count || witness.argument_count>65536 ||
        own>65536-witness.argument_count) return XR_XIR_BAD_STRUCTURE;
    uint32_t count=witness.argument_count+own;
    XrXirType *complete=count ? effect_terms_alloc(&resolved->terms,count,sizeof(*complete)) : NULL;
    if (count && !complete) return resolved->terms.status;
    for (uint32_t a=0;a<count;++a) {
        if (!xir_compile_work(resolved->terms.remaining,1)) return XR_XIR_BUDGET;
        complete[a]=a<witness.argument_count ? witness.arguments[a] :
            call->arguments[parent+a-witness.argument_count];
    }
    resolved->target=witness.function;resolved->arguments=complete;resolved->argument_count=count;
    return effect_context_target_constraints(resolved,request,&proof);
}

static XrXirStatus effect_context_direct(EffectContextResolved *resolved,
    const EffectContextRequest *request, const XrXirGeneric *call) {
    const XrXirInstruction *op=&request->source->functions[request->function].instructions[request->instruction];
    uint32_t target=(uint32_t)op->immediate;
    if (resolved->family==XR_XIR_EFFECT_BINDING_DEFAULT) {
        const XrXirDefaultBinding *binding=NULL;
        const uint32_t *identity=xr_xir_default_identity(op);
        XrXirStatus status=xr_xir_compile_default_lookup(resolved->terms.remaining,request->source,
            identity[0],identity[1],&binding);
        if (status!=XR_XIR_OK) return status;
        if (!binding) return XR_XIR_BAD_STRUCTURE;
        target=binding->function;
    } else if (op->immediate<0) return XR_XIR_BAD_STRUCTURE;
    if (target>=request->source->function_count) return XR_XIR_BAD_STRUCTURE;
    if (resolved->family==5 && (!request->source->declarations ||
        !request->source->declarations->functions ||
        request->source->declarations->functions[target].cleanup_owner!=request->function+1 ||
        request->source->functions[target].parameter_count!=op->args[1])) return XR_XIR_BAD_STRUCTURE;
    const XrXirGeneric *generic=request->source->generics ? &request->source->generics[target] : NULL;
    if (call->argument_count!=(generic ? generic->parameter_count : 0)) return XR_XIR_BAD_STRUCTURE;
    if (!resolved->known) return XR_XIR_OK;
    resolved->target=target;resolved->arguments=call->arguments;resolved->argument_count=call->argument_count;
    XrXirModule actual=*request->source;actual.types=&resolved->terms.types;
    XrXirProofContext proof={&actual,{XR_XIR_CONTEXT_CLOSED,0,0}};
    return effect_context_target_constraints(resolved,request,&proof);
}

/* Requires completed descriptor/declaration admission. Resolution owns its
 * structural result and grants no Checked, worker, or execution permission. */
#ifdef XR_XIR_EFFECT_CONTEXT_TESTS
static XrXirStatus effect_context_resolve(const XrXirCompileContext *context,
    const EffectContextRequest *request, EffectContextResolved **output) {
    if (!xir_compile_context_valid(context) || !request || !request->source ||
        !request->source->types || !request->actual_types || !request->source->functions ||
        request->function>=request->source->function_count || !output || *output ||
        !!request->arguments!=!!request->argument_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *function=&request->source->functions[request->function];
    if (!function->instructions || request->instruction>=function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    uint32_t family=effect_context_family(function->instructions[request->instruction].op);
    const XrXirGeneric *generic=request->source->generics ? &request->source->generics[request->function] : NULL;
    if (!family || request->argument_count!=(generic ? generic->parameter_count : 0))
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=XR_XIR_OK;
    EffectContextResolved *resolved=xir_compile_calloc(context,1,sizeof(*resolved),&status);
    if (!resolved) return status;
    resolved->terms.remaining=context;resolved->resources=context->resources;resolved->function=request->function;
    resolved->instruction=request->instruction;resolved->target=UINT32_MAX;
    resolved->family=family;resolved->known=true;
    status=effect_terms_seed(&resolved->terms,request->source->types);
    XrXirGeneric environment={0}, call={0};
    if (status==XR_XIR_OK) status=effect_context_arguments(resolved,request,&environment);
    if (status==XR_XIR_OK) status=effect_context_call_arguments(resolved,request,&environment,&call);
    if (status==XR_XIR_OK) status=family==XR_XIR_EFFECT_BINDING_REQUIREMENT ?
        effect_context_requirement(resolved,request,&environment,&call) :
        effect_context_direct(resolved,request,&call);
    if (status!=XR_XIR_OK) { effect_context_free(resolved);return status; }
    resolved->terms.remaining=NULL;
    *output=resolved;return XR_XIR_OK;
}

#endif

/* Internal expansion shares one structural owner. Temporary environments stay
 * in that owner's allocation list on both success and failure; no per-call
 * source pool or independent permission summary is created. */
static XrXirStatus effect_context_resolve_in(EffectTerms *pool,
    const EffectContextRequest *request, EffectContextResolved *output) {
    if (!pool || !pool->remaining || !request || !request->source || !request->source->types ||
        !request->actual_types || !request->source->functions ||
        request->function>=request->source->function_count || !output ||
        !!request->arguments!=!!request->argument_count || pool->types.count<request->source->types->count)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *function=&request->source->functions[request->function];
    if (!function->instructions || request->instruction>=function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    uint32_t family=effect_context_family(function->instructions[request->instruction].op);
    const XrXirGeneric *generic=request->source->generics ? &request->source->generics[request->function] : NULL;
    if (!family || request->argument_count!=(generic ? generic->parameter_count : 0))
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(pool->remaining,sizeof(*output))) return XR_XIR_BUDGET;
    EffectContextResolved resolved={.terms=*pool,.resources=pool->remaining->resources,
        .function=request->function,.instruction=request->instruction,.target=UINT32_MAX,
        .family=family,.known=true};
    XrXirGeneric environment={0}, call={0};
    XrXirStatus status=effect_context_arguments(&resolved,request,&environment);
    if (status==XR_XIR_OK) status=effect_context_call_arguments(&resolved,request,&environment,&call);
    if (status==XR_XIR_OK) status=family==XR_XIR_EFFECT_BINDING_REQUIREMENT ?
        effect_context_requirement(&resolved,request,&environment,&call) :
        effect_context_direct(&resolved,request,&call);
    *pool=resolved.terms;
    if (status==XR_XIR_OK) *output=resolved;
    return status;
}
