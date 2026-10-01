/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_requirement_verify.inc.c - Definition-bound abstract method calls
 *
 * KEY CONCEPT:
 *   A requirement reference consumes declared facts, never a method name search.
 */
static bool requirement_origin(const XrXirModule *module, uint32_t function, uint32_t instruction) {
    const XrXirProvenance *p = module->provenance;
    if (!p || !p->source || !p->origins || p->count != module->function_count || function >= p->count)
        return false;
    const XrXirModule *source = &p->source->module;
    uint32_t origin = p->origins[function].function;
    if (!source->functions || origin >= source->function_count) return false;
    const XrXirFunction *from = &source->functions[origin];
    return from->instructions && instruction < from->instruction_count &&
        from->instructions[instruction].op == XR_XIR_CALL_REQUIREMENT;
}

static XrXirStatus default_origin(const XrXirModule *m,uint32_t caller,uint32_t instruction,
    uint32_t target,XrXirBudget *budget,bool *authorized) {
    *authorized=false;
    const XrXirProvenance *p=m->provenance;
    if (!p) return XR_XIR_OK;
    if (!p->source || !p->origins || caller>=p->count || target>=p->count) return XR_XIR_BAD_STRUCTURE;
    const XrXirModule *source=&p->source->module;
    if (p->origins[caller].function>=source->function_count || !source->functions) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *f=&source->functions[p->origins[caller].function];
    if (instruction>=f->instruction_count || !f->instructions) return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *op=&f->instructions[instruction];
    XrXirOp destination=m->functions[caller].instructions[instruction].op;
    if ((op->op!=XR_XIR_CALL_DEFAULT || destination!=XR_XIR_CALL) &&
        (op->op!=XR_XIR_INVOKE_DEFAULT || destination!=XR_XIR_INVOKE)) return XR_XIR_OK;
    const XrXirDefaultBinding *binding=NULL;
    const uint32_t *identity=xr_xir_default_identity(op);
    XrXirStatus status=xr_xir_default_lookup(source,identity[0],identity[1],budget,&binding);
    if(status==XR_XIR_OK && binding) *authorized=binding->function==p->origins[target].function;
    return status;
}

static XrXirStatus requirement_application(const XrXirModule *module, uint32_t caller,
    const XrXirInstruction *op, XrXirInterfaceApplication *application) {
    const XrXirInterfaceTable *table = module->types ? module->types->interfaces : NULL;
    if (!table || !table->declarations || op->targets[0] >= table->count || op->immediate)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirInterfaceDeclaration *declaration = &table->declarations[op->targets[0]];
    if (op->targets[1] >= declaration->method_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirInterfaceMethod *method = &declaration->methods[op->targets[1]];
    if (declaration->parameter_count > 65536 || method->own_parameter_count > 65536 - declaration->parameter_count)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t total = declaration->parameter_count + method->own_parameter_count;
    const XrXirGeneric *generic = module->generics ? &module->generics[caller] : NULL;
    uint32_t count = generic ? generic->argument_count : 0;
    if (op->type_arguments[0] > count || op->type_arguments[1] > count - op->type_arguments[0] ||
        op->type_arguments[1] != total ||
        (!op->type_arguments[1] && op->type_arguments[0])) return XR_XIR_BAD_STRUCTURE;
    *application = (XrXirInterfaceApplication) {op->targets[0],
        declaration->parameter_count ? generic->arguments + op->type_arguments[0] : NULL, declaration->parameter_count};
    return XR_XIR_OK;
}

static XrXirStatus requirement_shape(const XrXirFunction *function,
    const XrXirInstruction *op, const XrXirModule *module, XrXirBudget *remaining) {
    uint32_t caller = (uint32_t)(function - module->functions);
    XrXirInterfaceApplication application = {0};
    XrXirStatus status = requirement_application(module, caller, op, &application);
    if (status != XR_XIR_OK) return status;
    const XrXirInterfaceMethod *method = &module->types->interfaces->declarations[application.declaration].methods[op->targets[1]];
    const XrXirTypeNode *signature = xr_xir_callable_signature(module->types, method->signature);
    if (!signature || method->receiver || signature->parameter_count == UINT32_MAX ||
        op->args[1] != signature->parameter_count + 1 || !op->args[1]) return XR_XIR_BAD_TYPE;
    uint64_t values = (uint64_t)function->parameter_count + function->instruction_count;
    for (uint32_t a = 0; a < op->args[1]; ++a)
        if (function->operands[op->args[0] + a] >= values) return XR_XIR_BAD_VALUE;
    XrXirType subject = xr_xir_operand_type(function, function->operands[op->args[0]]);
    XrXirProofContext context = {module, {XR_XIR_CONTEXT_FUNCTION, caller, 0}};
    status = xr_xir_interface_prove(&context, module, subject, application, remaining);
    if (status != XR_XIR_OK) return status;
    uint32_t total = op->type_arguments[1];
    const XrXirType *arguments = total ? module->generics[caller].arguments + op->type_arguments[0] : NULL;
    for (uint32_t a = 0; a < method->own_parameter_count && status == XR_XIR_OK; ++a) {
        XrXirConstraintUse use = {module,
            {XR_XIR_CONTEXT_INTERFACE_METHOD, application.declaration, op->targets[1]},
            application.argument_count + a, arguments, total};
        status = xr_xir_constraints_prove(&context, &use, remaining);
    }
    if (status == XR_XIR_OK) status = xr_xir_type_substitution_matches(module->types, arguments,
        total, signature->result, op->type, remaining);
    for (uint32_t a = 0; a < signature->parameter_count && status == XR_XIR_OK; ++a) {
        if (signature->parameters[a].mode) return XR_XIR_BAD_TYPE;
        XrXirType actual = xr_xir_operand_type(function, function->operands[op->args[0] + a + 1]);
        status = xr_xir_type_substitution_matches(module->types, arguments,
            total, signature->parameters[a].type, actual, remaining);
    }
    return status;
}
