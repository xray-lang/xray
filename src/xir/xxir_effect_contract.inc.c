/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_contract.inc.c - Bounded shape of owned effect contracts
 *
 * KEY CONCEPT:
 *   Canonical records reference actual instructions and physical parameters.
 *   Shape alone never proves their derived formula or permission.
 */
#include "xxir_generic.h"
#include "xxir_type_match_internal.h"

/* Matching keeps capacity only. Every result and physical parameter is
 * traversed again; nested invocations claim distinct live stack blocks. */
static XrXirStatus effect_callable_bound_compare_scratch(const XrXirCompileContext *context,
    const XirEffectCallableBound *request,uint32_t actual_scope,XrXirTypeMatchScratch *scratch) {
    if (!xir_compile_context_valid(context) || !request || !scratch ||
        scratch->resources!=context->resources ||
        !!request->arguments != !!request->argument_count) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
    const XrXirTypeNode *declared=xr_xir_callable_signature(request->source,request->declared);
    const XrXirTypeNode *actual=xr_xir_callable_signature(request->destination,request->actual);
    if (!declared || !actual || actual->parameter_span>actual_scope ||
        !!declared->parameters != !!declared->parameter_count ||
        !!actual->parameters != !!actual->parameter_count ||
        declared->parameter_count != actual->parameter_count ||
        !xr_xir_callable_flags_compatible(actual->flags,declared->flags)) return XR_XIR_BAD_TYPE;
    XrXirStatus status=xr_xir_compile_type_substitution_matches_between_scratch(context,
        request->source,request->destination,request->arguments,request->argument_count,
        declared->result,actual->result,scratch);
    for (uint32_t p=0;status==XR_XIR_OK && p<declared->parameter_count;++p) {
        if (!xir_compile_work(context,1)) { status=XR_XIR_BUDGET;break; }
        if (declared->parameters[p].mode != actual->parameters[p].mode) { status=XR_XIR_BAD_TYPE;break; }
        status=xr_xir_compile_type_substitution_matches_between_scratch(context,
            request->source,request->destination,request->arguments,request->argument_count,
            declared->parameters[p].type,actual->parameters[p].type,scratch);
    }
    return status;
}

static XrXirStatus effect_callable_bound_matches_scratch(const XrXirCompileContext *context,
    const XirEffectCallableBound *request,XrXirTypeMatchScratch *scratch) {
    return effect_callable_bound_compare_scratch(context,request,0,scratch);
}

/* An open actual Fn belongs to one authentic definition context. This
 * entry never accepts a prepared scope count or substitutes a callee's real
 * call arguments. The current definition proves all nested type obligations
 * before the common outer-bound relation is evaluated. */
static XrXirStatus effect_callable_bound_definition_scratch(const XrXirCompileContext *context,
    const XirEffectCallableBound *request,const XrXirModule *module,uint32_t function,
    XrXirTypeMatchScratch *scratch) {
    if (!xir_compile_context_valid(context) || !request || !scratch ||
        scratch->resources!=context->resources || !module || !module->types ||
        !module->functions || function>=module->function_count ||
        request->source!=module->types || request->destination!=module->types ||
        request->arguments || request->argument_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirGeneric *generic=module->generics?&module->generics[function]:NULL;
    uint32_t count=generic?generic->parameter_count:0;
    if (count>XR_XIR_TYPE_PARAMETER_LIMIT-XR_XIR_TYPE_PARAMETER_BASE ||
        (uint64_t)count*sizeof(XrXirType)>SIZE_MAX) return XR_XIR_BUDGET;
    if (generic && (!!generic->constraints!=!!count ||
        !!generic->arguments!=!!generic->argument_count || (!count && generic->parameter_kinds)))
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t p=0;p<count;++p) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        if (xr_xir_binder_kind(generic,p)>XR_XIR_BINDER_RESULT_VARIABLE) return XR_XIR_BAD_STRUCTURE;
    }
    XrXirProofContext proof={module,{XR_XIR_CONTEXT_FUNCTION,function,0}};
    XrXirStatus status=xr_xir_compile_context_constraints_verify(context,&proof);
    if (status==XR_XIR_OK) status=xr_xir_compile_type_expression_shape(context,module->types,request->declared,count);
    if (status==XR_XIR_OK) status=xr_xir_compile_type_expression_shape(context,module->types,request->actual,count);
    if (status==XR_XIR_OK) status=xr_xir_compile_type_use_verify(context,&proof,request->declared);
    if (status==XR_XIR_OK) status=xr_xir_compile_type_use_verify(context,&proof,request->actual);
    XrXirType *arguments=status==XR_XIR_OK && count?xir_compile_alloc(context,(size_t)count*sizeof(*arguments),&status):NULL;
    for (uint32_t p=0;p<count && status==XR_XIR_OK;++p) {
        if (!xir_compile_work(context,sizeof(*arguments)+1)) { status=XR_XIR_BUDGET;break; }
        arguments[p]=(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+p);
    }
    XirEffectCallableBound bound=*request;bound.arguments=arguments;bound.argument_count=count;
    if (status==XR_XIR_OK) status=effect_callable_bound_compare_scratch(context,&bound,count,scratch);
    xr_compile_resources_free(arguments);return status;
}

XR_FUNC XrXirStatus xir_effect_callable_bound_matches_definition(const XrXirCompileContext *context,
    const XrXirModule *module,uint32_t function,const XirEffectCallableBound *request) {
    if (!xir_compile_context_valid(context)) return XR_XIR_BAD_STRUCTURE;
    XrXirTypeMatchScratch scratch={context->resources,NULL};
    XrXirStatus status=effect_callable_bound_definition_scratch(context,request,module,function,&scratch);
    xr_xir_type_match_scratch_free(&scratch);return status;
}

XR_FUNC XrXirStatus xir_effect_callable_bound_matches(const XrXirCompileContext *context,
    const XirEffectCallableBound *request) {
    if (!xir_compile_context_valid(context)) return XR_XIR_BAD_STRUCTURE;
    XrXirTypeMatchScratch scratch={context->resources,NULL};
    XrXirStatus status=effect_callable_bound_matches_scratch(context,request,&scratch);
    xr_xir_type_match_scratch_free(&scratch);return status;
}

static XrXirStatus effect_contract_vector(const XrXirCompileContext *context,
    const void *data, uint32_t count, size_t size) {
    if ((uint64_t)count > SIZE_MAX / size) return XR_XIR_BUDGET;
    if (!xir_compile_work(context, 3)) return XR_XIR_BUDGET;
    return !!data == !!count ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
}

static bool effect_contract_context_op(XrXirOp op) {
    return op == XR_XIR_CALL || op == XR_XIR_INVOKE || op == XR_XIR_CALL_DEFAULT ||
        op == XR_XIR_INVOKE_DEFAULT || op == XR_XIR_CALL_REQUIREMENT ||
        op == XR_XIR_CALL_INDIRECT || op == XR_XIR_INVOKE_INDIRECT;
}

static XrXirStatus effect_contract_formula(const XrXirCompileContext *context,
    const XrXirTypes *types, const XrXirFunction *function, const XrXirFunctionEffectContract *contract) {
    const XrXirRootFormula *formula = &contract->formula;
    if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
    if (formula->constant_mask & ~(XR_XIR_CALLABLE_ROOT_REQUIRED | XR_XIR_CALLABLE_ROOT_UNRESOLVED))
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = effect_contract_vector(context, formula->terms,
        formula->term_count, sizeof(*formula->terms));
    if (status != XR_XIR_OK) return status;
    if ((uint64_t)formula->term_count > (uint64_t)function->parameter_count + function->instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t t = 0; t < formula->term_count; ++t) {
        if (!xir_compile_work(context, 6)) return XR_XIR_BUDGET;
        const XrXirRootTerm *term = &formula->terms[t];
        if (t && (formula->terms[t - 1].kind > term->kind ||
            (formula->terms[t - 1].kind == term->kind && formula->terms[t - 1].index >= term->index)))
            return XR_XIR_BAD_STRUCTURE;
        if (term->kind == XR_XIR_ROOT_TERM_PARAMETER) {
            if (term->index >= function->parameter_count ||
                contract->parameters[term->index].kind == XR_XIR_EFFECT_PARAMETER_FIXED)
                return XR_XIR_BAD_STRUCTURE;
        } else if (term->kind == XR_XIR_ROOT_TERM_CONTEXT_CALL) {
            if (term->index >= function->instruction_count ||
                !effect_contract_context_op(function->instructions[term->index].op))
                return XR_XIR_BAD_STRUCTURE;
        } else if (term->kind == XR_XIR_ROOT_TERM_CELL_PARAMETER) {
            if (term->index>=function->parameter_count ||
                !xr_xir_type_is_cell(types,function->parameters[term->index]))
                return XR_XIR_BAD_STRUCTURE;
        } else return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_contract_parameters(const XrXirCompileContext *context,
    const XrXirModule *module, uint32_t function, const XrXirFunctionEffectContract *contract) {
    const XrXirFunction *definition = &module->functions[function];
    if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
    if (contract->parameter_count != definition->parameter_count) return XR_XIR_BAD_STRUCTURE;
    if (!definition->parameters && definition->parameter_count) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = effect_contract_vector(context, contract->parameters,
        contract->parameter_count, sizeof(*contract->parameters));
    if (status != XR_XIR_OK) return status;
    for (uint32_t p = 0; p < contract->parameter_count; ++p) {
        if (!xir_compile_work(context, 3)) return XR_XIR_BUDGET;
        const XrXirEffectParameter *parameter = &contract->parameters[p];
        if (parameter->kind > XR_XIR_EFFECT_PARAMETER_CONDITIONAL_STATIC || parameter->uses & ~511u)
            return XR_XIR_BAD_STRUCTURE;
        if (parameter->kind != XR_XIR_EFFECT_PARAMETER_FIXED &&
            !xr_xir_callable_signature(module->types, definition->parameters[p])) return XR_XIR_BAD_TYPE;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_contract_values(const XrXirCompileContext *context,
    const XrXirModule *module, uint32_t function, const XrXirFunctionEffectContract *contract) {
    const XrXirFunction *definition = &module->functions[function];
    XrXirStatus status = effect_contract_vector(context, contract->values,
        contract->value_count, sizeof(*contract->values));
    if (status != XR_XIR_OK) return status;
    if (contract->value_count > definition->instruction_count) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t v = 0; v < contract->value_count; ++v) {
        if (!xir_compile_work(context, 5)) return XR_XIR_BUDGET;
        const XrXirRootValueIdentity *value = &contract->values[v];
        if (value->instruction >= definition->instruction_count ||
            (v && contract->values[v - 1].instruction >= value->instruction) ||
            value->mode < XR_XIR_EFFECT_VALUE_FIXED || value->mode > XR_XIR_EFFECT_VALUE_AUTHENTIC_REF)
            return XR_XIR_BAD_STRUCTURE;
        if (!xr_xir_callable_signature(module->types, value->declared_type)) return XR_XIR_BAD_TYPE;
        const XrXirInstruction *op = &definition->instructions[value->instruction];
        if (!xr_xir_callable_signature(module->types, op->type)) return XR_XIR_BAD_TYPE;
        if ((value->mode == XR_XIR_EFFECT_VALUE_FIXED && op->op != XR_XIR_FUNCTION_WEAKEN) ||
            (value->mode == XR_XIR_EFFECT_VALUE_AUTHENTIC_REF && op->op != XR_XIR_FUNCTION_REF) ||
            (value->mode == XR_XIR_EFFECT_VALUE_CALL_BIND && op->op != XR_XIR_COPY) ||
            (value->mode == XR_XIR_EFFECT_VALUE_PROPAGATE && op->op == XR_XIR_FUNCTION_WEAKEN))
            return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

static int effect_contract_binding_order(const XrXirEffectCallBinding *left,
    const XrXirEffectCallBinding *right) {
    if (left->family != right->family) return left->family < right->family ? -1 : 1;
    if (left->instruction != right->instruction) return left->instruction < right->instruction ? -1 : 1;
    if (left->parameter != right->parameter) return left->parameter < right->parameter ? -1 : 1;
    return 0;
}

static XrXirStatus effect_contract_bindings(const XrXirCompileContext *context,
    const XrXirFunction *function, const XrXirFunctionEffectContract *contract) {
    XrXirStatus status = effect_contract_vector(context, contract->bindings,
        contract->binding_count, sizeof(*contract->bindings));
    if (status != XR_XIR_OK) return status;
    if (contract->binding_count > function->operand_count) return XR_XIR_BAD_STRUCTURE;
    uint64_t values = (uint64_t)function->parameter_count + function->instruction_count;
    for (uint32_t b = 0; b < contract->binding_count; ++b) {
        if (!xir_compile_work(context, 10)) return XR_XIR_BUDGET;
        const XrXirEffectCallBinding *binding = &contract->bindings[b];
        if (binding->family < XR_XIR_EFFECT_BINDING_DIRECT || binding->family > XR_XIR_EFFECT_BINDING_REQUIREMENT ||
            binding->instruction >= function->instruction_count || binding->value >= values ||
            (b && effect_contract_binding_order(&contract->bindings[b - 1], binding) >= 0))
            return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_contract_template(const XrXirCompileContext *context,
    const XrXirModule *module, const XrXirProvenance *p) {
    if (p->source || p->origins || p->count || p->bindings || p->binding_count ||
        p->contract_count != module->function_count) return XR_XIR_BAD_STRUCTURE;
    if (module->stage == XR_XIR_LOWERED) return XR_XIR_BAD_STAGE;
    XrXirStatus status = effect_contract_vector(context, p->contracts,
        p->contract_count, sizeof(*p->contracts));
    if (status != XR_XIR_OK) return status;
    for (uint32_t f = 0; f < p->contract_count; ++f) {
        if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
        const XrXirFunctionEffectContract *contract = &p->contracts[f];
        status = effect_contract_parameters(context, module, f, contract);
        if (status == XR_XIR_OK) status = effect_contract_formula(context, module->types, &module->functions[f], contract);
        if (status == XR_XIR_OK) status = effect_contract_values(context, module, f, contract);
        if (status == XR_XIR_OK) status = effect_contract_bindings(context, &module->functions[f], contract);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

static int effect_contract_proof_order(const XrXirEffectBindingProof *left,
    const XrXirEffectBindingProof *right) {
    if (left->caller != right->caller) return left->caller < right->caller ? -1 : 1;
    if (left->family != right->family) return left->family < right->family ? -1 : 1;
    if (left->instruction != right->instruction) return left->instruction < right->instruction ? -1 : 1;
    if (left->parameter != right->parameter) return left->parameter < right->parameter ? -1 : 1;
    return 0;
}

static XrXirStatus effect_contract_proofs(const XrXirCompileContext *context,
    const XrXirModule *module, const XrXirProvenance *p) {
    for (uint32_t b = 0; b < p->binding_count; ++b) {
        if (!xir_compile_work(context, 14)) return XR_XIR_BUDGET;
        const XrXirEffectBindingProof *proof = &p->bindings[b];
        if (proof->family < XR_XIR_EFFECT_BINDING_DIRECT || proof->family > XR_XIR_EFFECT_BINDING_REQUIREMENT ||
            proof->caller >= module->function_count || proof->callee >= module->function_count ||
            (b && effect_contract_proof_order(&p->bindings[b - 1], proof) >= 0))
            return XR_XIR_BAD_STRUCTURE;
        const XrXirFunction *caller = &module->functions[proof->caller];
        if (proof->instruction >= caller->instruction_count ||
            proof->parameter >= module->functions[proof->callee].parameter_count ||
            proof->actual_value >= (uint64_t)caller->parameter_count + caller->instruction_count)
            return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_contract_instance(const XrXirCompileContext *context,
    const XrXirModule *module, const XrXirProvenance *p) {
    if (!p->source || p->contracts || p->contract_count || p->count != module->function_count)
        return XR_XIR_BAD_STRUCTURE;
    if (module->stage == XR_XIR_BUILT || module->linkage_kind != XR_XIR_PROGRAM ||
        p->source->module.stage != XR_XIR_CHECKED || p->source->module.linkage_kind != XR_XIR_PROGRAM)
        return XR_XIR_BAD_STAGE;
    if (p->source->module.provenance && p->source->module.provenance->kind != XR_XIR_EVIDENCE_TEMPLATE)
        return XR_XIR_BAD_STRUCTURE;
    if (!p->source->module.functions || !p->source->module.function_count) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = effect_contract_vector(context, p->origins, p->count, sizeof(*p->origins));
    if (status == XR_XIR_OK) status = effect_contract_vector(context, p->bindings,
        p->binding_count, sizeof(*p->bindings));
    if (status != XR_XIR_OK) return status;
    for (uint32_t f = 0; f < p->count; ++f) {
        if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
        const XrXirOrigin *origin = &p->origins[f];
        if (origin->function >= p->source->module.function_count) return XR_XIR_BAD_STRUCTURE;
        status = effect_contract_vector(context, origin->arguments, origin->argument_count,
            sizeof(*origin->arguments));
        if (status == XR_XIR_OK) status = effect_contract_vector(context, origin->effect_arguments,
            origin->effect_argument_count, sizeof(*origin->effect_arguments));
        if (status != XR_XIR_OK) return status;
        uint32_t physical=p->source->module.functions[origin->function].parameter_count;
        if (p->source->module.provenance && origin->effect_argument_count!=physical)
            return XR_XIR_BAD_STRUCTURE;
        for (uint32_t a = 0; a < origin->effect_argument_count; ++a) {
            if (!xir_compile_work(context, 3)) return XR_XIR_BUDGET;
            const XrXirEffectArgument *argument = &origin->effect_arguments[a];
            if (argument->parameter >= p->source->module.functions[origin->function].parameter_count ||
                (a && origin->effect_arguments[a - 1].parameter >= argument->parameter))
                return XR_XIR_BAD_STRUCTURE;
            if (argument->parameter!=a || xr_xir_type_span(module->types,argument->type))
                return XR_XIR_BAD_TYPE;
        }
    }
    return effect_contract_proofs(context, module, p);
}

XR_FUNC XrXirStatus xir_effect_contract_shape_verify(const XrXirCompileContext *context,
    const XrXirModule *module) {
    if (!xir_compile_context_valid(context) || !module || !module->functions || !module->function_count)
        return XR_XIR_BAD_STRUCTURE;
    if (!module->provenance) return XR_XIR_OK;
    if (!xir_compile_work(context, 8)) return XR_XIR_BUDGET;
    const XrXirProvenance *p = module->provenance;
    if (p->kind == XR_XIR_EVIDENCE_TEMPLATE) return effect_contract_template(context, module, p);
    if (p->kind == XR_XIR_EVIDENCE_INSTANCE) return effect_contract_instance(context, module, p);
    return XR_XIR_BAD_STRUCTURE;
}

/* A requirement candidate uses the already admitted interface application.
 * Its physical zero is the receiver; it cannot become a callable binding. */
static XrXirStatus effect_contract_requirement_bound(const XrXirCompileContext *context,
    const XrXirModule *module, uint32_t caller, const XrXirInstruction *op,
    uint32_t parameter, XrXirType declared) {
    const XrXirInterfaceTable *table=module->types->interfaces;
    if (!table || !table->declarations || op->targets[0]>=table->count || op->immediate || !parameter)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirInterfaceDeclaration *interface=&table->declarations[op->targets[0]];
    if (op->targets[1]>=interface->method_count || !interface->methods) return XR_XIR_BAD_STRUCTURE;
    const XrXirInterfaceMethod *method=&interface->methods[op->targets[1]];
    const XrXirTypeNode *signature=xr_xir_callable_signature(module->types,method->signature);
    if (!signature || method->receiver || parameter>signature->parameter_count ||
        !signature->parameters || signature->parameters[parameter-1].mode ||
        interface->parameter_count>65536 ||
        method->own_parameter_count>65536-interface->parameter_count) return XR_XIR_BAD_TYPE;
    uint32_t total=interface->parameter_count+method->own_parameter_count;
    const XrXirGeneric *generic=module->generics ? &module->generics[caller] : NULL;
    uint32_t stored=generic ? generic->argument_count : 0;
    if (op->type_arguments[1]!=total || op->type_arguments[0]>stored ||
        total>stored-op->type_arguments[0] || (total && !generic->arguments))
        return XR_XIR_BAD_STRUCTURE;
    const XrXirType *arguments=total ? generic->arguments+op->type_arguments[0] : NULL;
    return xr_xir_compile_type_substitution_matches(context,module->types,arguments,total,
        signature->parameters[parameter-1].type,declared);
}

/* Cleanup captures have a real lexical registration instead of a public
 * call-binding family. Final source correspondence reconstructs this same edge. */
static XrXirStatus effect_contract_cleanup_candidate(const XrXirCompileContext *context,
    const XrXirModule *module, uint32_t caller, uint32_t instruction, uint32_t parameter) {
    const XrXirProvenance *p=module->provenance;
    const XrXirModule *source=&p->source->module;
    const XrXirOrigin *origin=&p->origins[caller];
    const XrXirFunction *function=&module->functions[caller];
    const XrXirInstruction *op=&function->instructions[instruction];
    const XrXirInstruction *before=&source->functions[origin->function].instructions[instruction];
    uint32_t target=(uint32_t)op->immediate;
    const XrXirOrigin *selected=&p->origins[target];
    if (before->op!=XR_XIR_CLEANUP_REGISTER || op->op!=XR_XIR_CLEANUP_REGISTER ||
        before->immediate<0 || (uint64_t)before->immediate>=source->function_count ||
        selected->function!=(uint32_t)before->immediate || !source->declarations ||
        !module->declarations || !source->declarations->functions || !module->declarations->functions ||
        source->declarations->functions[selected->function].cleanup_owner!=origin->function+1 ||
        module->declarations->functions[target].cleanup_owner!=caller+1 ||
        parameter>=module->functions[target].parameter_count) return XR_XIR_BAD_STRUCTURE;
    uint32_t value=function->operands[op->args[0]+parameter];
    if (value>=(uint64_t)function->parameter_count+function->instruction_count) return XR_XIR_BAD_VALUE;
    XirEffectCallableBound original={source->types,module->types,selected->arguments,
        selected->argument_count,source->functions[selected->function].parameters[parameter],
        module->functions[target].parameters[parameter]};
    XrXirStatus status=xir_effect_callable_bound_matches(context,&original);
    XirEffectCallableBound candidate={module->types,module->types,NULL,0,
        module->functions[target].parameters[parameter],xr_xir_operand_type(function,value)};
    return status==XR_XIR_OK ? xir_effect_callable_bound_matches(context,&candidate) : status;
}

/* Structural admission retains an authentic precise COPY at a canonical
 * instance bound. Final source correspondence independently decides that bound. */
static XrXirStatus effect_contract_instance_candidate(const XrXirCompileContext *context,
    const XrXirModule *module, uint32_t caller, uint32_t instruction, uint32_t parameter) {
    const XrXirProvenance *p=module->provenance;
    XrXirStatus status=effect_contract_instance(context,module,p);
    if (status!=XR_XIR_OK) return status;
    const XrXirModule *source=&p->source->module;
    const XrXirOrigin *origin=&p->origins[caller];
    const XrXirFunction *function=&module->functions[caller];
    const XrXirInstruction *op=&function->instructions[instruction];
    const XrXirFunction *original=&source->functions[origin->function];
    if (instruction>=original->instruction_count || parameter>=op->args[1] ||
        op->args[0]>function->operand_count || op->args[1]>function->operand_count-op->args[0] ||
        !function->operands || op->immediate<0 || (uint64_t)op->immediate>=module->function_count)
        return XR_XIR_BAD_STRUCTURE;
    XrXirOp before=original->instructions[instruction].op;
    if (before==XR_XIR_CLEANUP_REGISTER)
        return effect_contract_cleanup_candidate(context,module,caller,instruction,parameter);
    uint32_t family=before==XR_XIR_CALL || before==XR_XIR_INVOKE ? XR_XIR_EFFECT_BINDING_DIRECT :
        before==XR_XIR_FUNCTION_REF ? XR_XIR_EFFECT_BINDING_CAPTURE :
        before==XR_XIR_CALL_DEFAULT || before==XR_XIR_INVOKE_DEFAULT ? XR_XIR_EFFECT_BINDING_DEFAULT :
        before==XR_XIR_CALL_REQUIREMENT ? XR_XIR_EFFECT_BINDING_REQUIREMENT : 0;
    if (!family || (op->op!=XR_XIR_CALL && op->op!=XR_XIR_INVOKE && op->op!=XR_XIR_FUNCTION_REF))
        return XR_XIR_BAD_TYPE;
    uint32_t target=(uint32_t)op->immediate;
    if (parameter>=module->functions[target].parameter_count) return XR_XIR_BAD_STRUCTURE;
    uint32_t value=function->operands[op->args[0]+parameter];
    uint64_t values=(uint64_t)function->parameter_count+function->instruction_count;
    if (value<function->parameter_count || value>=values ||
        function->parameter_count!=original->parameter_count) return XR_XIR_BAD_TYPE;
    uint32_t producer=value-function->parameter_count;
    if (producer>=original->instruction_count || (function->instructions[producer].op!=XR_XIR_COPY &&
         !(module->stage==XR_XIR_LOWERED && function->instructions[producer].op==XR_XIR_OWNED_RETAIN)) ||
        original->instructions[producer].op!=XR_XIR_COPY ||
        function->instructions[producer].type!=xr_xir_operand_type(function,function->instructions[producer].args[0]))
        return XR_XIR_BAD_TYPE;
    const XrXirProvenance *template=source->provenance;
    if (!template || template->kind!=XR_XIR_EVIDENCE_TEMPLATE || !template->contracts ||
        template->contract_count!=source->function_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunctionEffectContract *consumer=&template->contracts[origin->function];
    if (!!consumer->values!=!!consumer->value_count || !!consumer->bindings!=!!consumer->binding_count)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirRootValueIdentity *identity=NULL;
    uint32_t matches=0;
    for (uint32_t v=0;v<consumer->value_count;++v) {
        if (!xir_compile_work(context,2)) return XR_XIR_BUDGET;
        if (consumer->values[v].instruction==producer) identity=&consumer->values[v];
    }
    if (!identity || identity->mode!=XR_XIR_EFFECT_VALUE_CALL_BIND) return XR_XIR_BAD_TYPE;
    for (uint32_t b=0;b<consumer->binding_count;++b) {
        if (!xir_compile_work(context,4)) return XR_XIR_BUDGET;
        const XrXirEffectCallBinding *binding=&consumer->bindings[b];
        if (binding->family==family && binding->instruction==instruction &&
            binding->parameter==parameter && binding->value==value) ++matches;
    }
    if (matches!=1) return XR_XIR_BAD_STRUCTURE;
    matches=0;
    for (uint32_t b=0;b<p->binding_count;++b) {
        if (!xir_compile_work(context,6)) return XR_XIR_BUDGET;
        const XrXirEffectBindingProof *proof=&p->bindings[b];
        if (proof->family==family && proof->caller==caller && proof->instruction==instruction &&
            proof->callee==target && proof->parameter==parameter && proof->actual_value==value) ++matches;
    }
    if (matches!=1) return XR_XIR_BAD_STRUCTURE;
    XirEffectCallableBound declared={source->types,module->types,origin->arguments,
        origin->argument_count,identity->declared_type,module->functions[target].parameters[parameter]};
    status=xir_effect_callable_bound_matches(context,&declared);
    XirEffectCallableBound candidate={module->types,module->types,NULL,0,
        module->functions[target].parameters[parameter],xr_xir_operand_type(function,value)};
    return status==XR_XIR_OK ? xir_effect_callable_bound_matches(context,&candidate) : status;
}

/* Only this candidate edge may differ from its declared outer Fn bound. The
 * caller has already passed ordinary instruction/type shape; this helper still
 * admits all metadata views before accessing their records. */
XR_FUNC XrXirStatus xir_effect_call_binding_candidate(const XrXirCompileContext *context,
    const XrXirModule *module, uint32_t caller, uint32_t instruction, uint32_t parameter) {
    if (!xir_compile_context_valid(context) || !module || !module->types || !module->functions ||
        caller >= module->function_count || instruction >= module->functions[caller].instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirProvenance *p = module->provenance;
    if (!p) return XR_XIR_BAD_TYPE;
    if (p->kind==XR_XIR_EVIDENCE_INSTANCE)
        return effect_contract_instance_candidate(context,module,caller,instruction,parameter);
    if (p->kind!=XR_XIR_EVIDENCE_TEMPLATE) return XR_XIR_BAD_TYPE;
    if (!xir_compile_work(context, 8)) return XR_XIR_BUDGET;
    if ((module->stage != XR_XIR_BUILT && module->stage != XR_XIR_CHECKED) ||
        p->source || p->origins || p->count || p->bindings || p->binding_count ||
        p->contract_count != module->function_count) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = effect_contract_vector(context,p->contracts,p->contract_count,sizeof(*p->contracts));
    if (status != XR_XIR_OK) return status;
    const XrXirFunction *function = &module->functions[caller];
    if (!function->instructions || (function->parameter_count && !function->parameters))
        return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *op = &function->instructions[instruction];
    uint32_t family = op->op == XR_XIR_CALL || op->op == XR_XIR_INVOKE ? XR_XIR_EFFECT_BINDING_DIRECT :
        op->op == XR_XIR_FUNCTION_REF ? XR_XIR_EFFECT_BINDING_CAPTURE :
        op->op == XR_XIR_CALL_REQUIREMENT ? XR_XIR_EFFECT_BINDING_REQUIREMENT : 0;
    if (!family) return XR_XIR_BAD_TYPE;
    bool requirement=family==XR_XIR_EFFECT_BINDING_REQUIREMENT;
    if ((!requirement && (op->immediate<0 || (uint64_t)op->immediate>=module->function_count)) ||
        parameter >= op->args[1] || op->args[0] > function->operand_count ||
        op->args[1] > function->operand_count - op->args[0] || !function->operands)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t callee = requirement ? UINT32_MAX : (uint32_t)op->immediate;
    const XrXirFunctionEffectContract *definition = requirement ? NULL : &p->contracts[callee];
    const XrXirFunctionEffectContract *consumer = &p->contracts[caller];
    status=effect_contract_parameters(context,module,caller,consumer);
    if (status==XR_XIR_OK && !requirement && caller!=callee)
        status=effect_contract_parameters(context,module,callee,definition);
    if (status == XR_XIR_OK) status = effect_contract_values(context,module,caller,consumer);
    if (status == XR_XIR_OK) status = effect_contract_bindings(context,function,consumer);
    if (status != XR_XIR_OK) return status;
    if (!requirement && (parameter>=definition->parameter_count ||
        definition->parameters[parameter].kind==XR_XIR_EFFECT_PARAMETER_FIXED)) return XR_XIR_BAD_TYPE;
    if (!requirement && module->declarations &&
        module->declarations->functions[callee].method_kind==XR_XIR_READ_METHOD && !parameter)
        return XR_XIR_BAD_TYPE;
    if (requirement && !parameter) return XR_XIR_BAD_TYPE;
    const XrXirEffectCallBinding *binding = NULL;
    for (uint32_t b = 0; b < consumer->binding_count; ++b) {
        if (!xir_compile_work(context, 3)) return XR_XIR_BUDGET;
        const XrXirEffectCallBinding *current = &consumer->bindings[b];
        if (current->family == family && current->instruction == instruction && current->parameter == parameter)
            binding = current;
    }
    if (!binding) return XR_XIR_BAD_TYPE;
    uint32_t actual = function->operands[op->args[0] + parameter];
    uint64_t values = (uint64_t)function->parameter_count + function->instruction_count;
    if (binding->value != actual || actual < function->parameter_count || actual >= values)
        return XR_XIR_BAD_TYPE;
    uint32_t producer = actual - function->parameter_count;
    const XrXirInstruction *copy = &function->instructions[producer];
    if (copy->op != XR_XIR_COPY || copy->args[0] >= values) return XR_XIR_BAD_TYPE;
    const XrXirRootValueIdentity *identity = NULL;
    for (uint32_t v = 0; v < consumer->value_count; ++v) {
        if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
        if (consumer->values[v].instruction == producer) identity = &consumer->values[v];
    }
    if (!identity || identity->mode != XR_XIR_EFFECT_VALUE_CALL_BIND ||
        copy->type != xr_xir_operand_type(function,copy->args[0])) return XR_XIR_BAD_TYPE;
    status=requirement ? effect_contract_requirement_bound(context,module,caller,op,parameter,
        identity->declared_type) : xr_xir_compile_call_type_matches(context,module,caller,op,
        module->functions[callee].parameters[parameter],identity->declared_type);
    if (status != XR_XIR_OK) return status;
    return xr_xir_compile_callable_weakening(context,module->types,copy->type,identity->declared_type);
}
