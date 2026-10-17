/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_structural_verify.inc.c - Independent declaration-bound field admission
 */
static XrXirStatus structural_field_uses(const Graph *graph,const XrXirFunction *function,
    VerifyContext *context,uint32_t instruction) {
    const XrXirInstruction *op=&function->instructions[instruction];
    if (!xir_compile_work(&context->remaining,1)) return XR_XIR_BUDGET;
    XrXirType receiver=xr_xir_operand_type(function,op->args[0]);uint32_t id=(uint32_t)receiver;
    const XrXirGeneric *generic=context->module->generics ?
        &context->module->generics[context->location.function] : NULL;
    if (!generic || id<XR_XIR_TYPE_PARAMETER_BASE || id>=XR_XIR_TYPE_PARAMETER_LIMIT ||
        id-XR_XIR_TYPE_PARAMETER_BASE>=generic->parameter_count || !generic->constraints || op->immediate<0)
        return XR_XIR_BAD_TYPE;
    const XrXirConstraint *constraint=&generic->constraints[id-XR_XIR_TYPE_PARAMETER_BASE];
    uint64_t selection=(uint64_t)op->immediate;uint32_t requirement=(uint32_t)(selection>>32),field=(uint32_t)selection;
    if (requirement>=constraint->structural_count || !constraint->structural_requirements) return XR_XIR_BAD_STRUCTURE;
    const XrXirStructuralRequirement *bound=&constraint->structural_requirements[requirement];
    const XrXirTypeNode *shape=xr_xir_record_signature(context->module->types,bound->shape);
    if (bound->policy!=XR_XIR_STRUCTURAL_READONLY_RECEIVER || !shape || field>=shape->parameter_count)
        return XR_XIR_BAD_TYPE;
    if (op->type!=shape->parameters[field].type) return XR_XIR_BAD_TYPE;
    XrXirStatus status=type_use_context(context,context->location.function,op->type);
    if (status==XR_XIR_OK) status=role_operand(function,graph,context,instruction,0,receiver);
    return status;
}
