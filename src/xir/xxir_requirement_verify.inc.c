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

static XrXirStatus requirement_application(const XrXirModule *module, uint32_t caller,
    const XrXirInstruction *op, XrXirInterfaceApplication *application) {
    const XrXirInterfaceTable *table = module->types ? module->types->interfaces : NULL;
    if (!table || !table->declarations || op->targets[0] >= table->count || op->immediate)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirInterfaceDeclaration *declaration = &table->declarations[op->targets[0]];
    if (op->targets[1] >= declaration->method_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirGeneric *generic = module->generics ? &module->generics[caller] : NULL;
    uint32_t count = generic ? generic->argument_count : 0;
    if (op->type_arguments[0] > count || op->type_arguments[1] > count - op->type_arguments[0] ||
        op->type_arguments[1] != declaration->parameter_count ||
        (!op->type_arguments[1] && op->type_arguments[0])) return XR_XIR_BAD_STRUCTURE;
    *application = (XrXirInterfaceApplication) {op->targets[0],
        op->type_arguments[1] ? generic->arguments + op->type_arguments[0] : NULL, op->type_arguments[1]};
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
    XrXirProofContext context = {module, {XR_XIR_CONTEXT_FUNCTION, caller}};
    status = xr_xir_interface_prove(&context, module, subject, application, remaining);
    if (status != XR_XIR_OK) return status;
    status = xr_xir_type_substitution_matches(module->types, application.arguments,
        application.argument_count, signature->result, op->type, remaining);
    for (uint32_t a = 0; a < signature->parameter_count && status == XR_XIR_OK; ++a) {
        if (signature->parameters[a].mode) return XR_XIR_BAD_TYPE;
        XrXirType actual = xr_xir_operand_type(function, function->operands[op->args[0] + a + 1]);
        status = xr_xir_type_substitution_matches(module->types, application.arguments,
            application.argument_count, signature->parameters[a].type, actual, remaining);
    }
    return status;
}
