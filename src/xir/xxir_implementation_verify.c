/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_implementation_verify.c - Declaration-owned conditional witness validation
 *
 * KEY CONCEPT:
 *   Explicit records establish conditional conformance only after every member
 *   and declaration precondition has been checked.
 */
#include "xxir_implementation_verify.h"
#include "xxir_interface_members.h"
#include "xxir_types.h"
#include "../base/xmalloc.h"
#include <string.h>
static bool implementation_charge(XrXirBudget *budget, uint64_t bytes, uint64_t work) {
    if (bytes > SIZE_MAX || bytes > budget->metadata_bytes || work > budget->work) return false;
    budget->metadata_bytes -= bytes; budget->work -= work; return true;
}
static XrXirStatus implementation_identity_arguments(uint32_t count, XrXirBudget *budget, XrXirType **output) {
    *output = NULL;
    uint64_t bytes = (uint64_t)count * sizeof(XrXirType);
    if (bytes > SIZE_MAX || bytes > budget->scratch_bytes || count > budget->work) return XR_XIR_BUDGET;
    budget->scratch_bytes -= bytes; budget->work -= count;
    if (!count) return XR_XIR_OK;
    XrXirType *arguments = xr_malloc((size_t)bytes);
    if (!arguments) { budget->scratch_bytes += bytes; return XR_XIR_OUT_OF_MEMORY; }
    for (uint32_t a = 0; a < count; ++a) arguments[a] = (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+a);
    *output = arguments; return XR_XIR_OK;
}
static void implementation_arguments_free(XrXirType *arguments, uint32_t count, XrXirBudget *budget) {
    if (arguments) { xr_free(arguments); budget->scratch_bytes += (uint64_t)count*sizeof(*arguments); }
}
static XrXirStatus implementation_application_equal(const XrXirTypes *from, const XrXirTypes *to,
    const XrXirInterfaceApplication *expected, const XrXirInterfaceApplication *actual,
    const XrXirInterfaceApplication *substitution, XrXirBudget *budget) {
    if (!implementation_charge(budget,0,1)) return XR_XIR_BUDGET;
    if (expected->declaration != actual->declaration || expected->argument_count != actual->argument_count)
        return XR_XIR_BAD_TYPE;
    for (uint32_t a = 0; a < expected->argument_count; ++a) {
        XrXirStatus status = xr_xir_type_substitution_matches_between(from,to,substitution->arguments,
            substitution->argument_count,expected->arguments[a],actual->arguments[a],budget);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
static XrXirStatus implementation_function(const XrXirModule *module, const XrXirImplementation *record,
    const XrXirInterfaceRequirement *requirement, const XrXirInterfaceClosure *closure,
    uint32_t function, XrXirBudget *budget) {
    if (function >= module->function_count || !module->functions || !module->declarations->functions)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirFunctionIdentity *identity = &module->declarations->functions[function];
    if (identity->nominal_owner != record->nominal_declaration+1 || identity->method_kind != XR_XIR_READ_METHOD ||
        identity->member_access != XR_XIR_MEMBER_PUBLIC || identity->cleanup_owner) return XR_XIR_BAD_TYPE;
    const XrXirNominalDeclaration *owner = &module->types->nominals->declarations[record->nominal_declaration];
    if (identity->exported != owner->exported) return XR_XIR_BAD_TYPE;
    if (identity->module >= module->declarations->module_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirSourceModule *source = &module->declarations->modules[identity->module];
    if (!implementation_charge(budget,0,(uint64_t)owner->module.length+1)) return XR_XIR_BUDGET;
    if (source->name_length != owner->module.length || memcmp(source->name,owner->module.bytes,source->name_length))
        return XR_XIR_BAD_TYPE;
    const XrXirGeneric *generic = module->generics ? &module->generics[function] : NULL;
    if (owner->parameter_count > 65536 || requirement->own_parameter_count > 65536-owner->parameter_count)
        return XR_XIR_BAD_TYPE;
    uint32_t full_count = owner->parameter_count+requirement->own_parameter_count;
    if ((generic ? generic->parameter_count : 0) != full_count) return XR_XIR_BAD_TYPE;
    const XrXirFunction *f = &module->functions[function];
    const XrXirTypes *types = xr_xir_interface_closure_types(closure);
    const XrXirTypeNode *signature = xr_xir_callable_signature(types,requirement->signature);
    if (!signature || f->parameter_count != signature->parameter_count+1 || !f->parameters) return XR_XIR_BAD_TYPE;
    if ((signature->flags & XR_XIR_CALLABLE_NO_SUSPEND) && !(identity->promises & XR_XIR_FUNCTION_NO_SUSPEND))
        return XR_XIR_BAD_TYPE;
    XrXirStatus receiver_status = xr_xir_method_signature_verify(module,function,budget);
    if (receiver_status != XR_XIR_OK) return receiver_status;
    XrXirType *arguments = NULL;
    XrXirStatus status = implementation_identity_arguments(full_count,budget,&arguments);
    for (uint32_t a = 0; status == XR_XIR_OK && a < signature->parameter_count; ++a) {
        if (signature->parameters[a].mode) { status = XR_XIR_BAD_TYPE; break; }
        status = xr_xir_type_substitution_matches_between(types,module->types,arguments,full_count,
            signature->parameters[a].type,f->parameters[a+1],budget);
    }
    if (status == XR_XIR_OK) status = xr_xir_type_substitution_matches_between(types,module->types,
        arguments,full_count,signature->result,f->result,budget);
    implementation_arguments_free(arguments,full_count,budget); return status;
}
static XrXirStatus implementation_bindings(const XrXirModule *module, const XrXirImplementation *record,
    XrXirInterfaceClosure *closure, XrXirBudget *budget) {
    uint32_t count = module->types->nominals->declarations[record->nominal_declaration].parameter_count;
    uint32_t required = xr_xir_interface_closure_requirement_count(closure);
    if (record->binding_count != required || (!!record->bindings != !!record->binding_count)) return XR_XIR_BAD_STRUCTURE;
    XrXirType *arguments = NULL;
    XrXirStatus status = implementation_identity_arguments(count,budget,&arguments);
    XrXirInterfaceApplication substitution = {0,arguments,count};
    for (uint32_t r = 0; status == XR_XIR_OK && r < required; ++r) {
        const XrXirInterfaceRequirement *requirement = xr_xir_interface_closure_requirement(closure,r);
        const XrXirInterfaceApplication *app = xr_xir_interface_closure_application(closure,requirement->application);
        uint32_t found = UINT32_MAX;
        for (uint32_t b = 0; b < record->binding_count; ++b) {
            const XrXirImplementationBinding *binding = &record->bindings[b];
            if (binding->member != requirement->member) continue;
            XrXirStatus match = implementation_application_equal(module->types,xr_xir_interface_closure_types(closure),
                &binding->requirement,app,&substitution,budget);
            if (match != XR_XIR_OK && match != XR_XIR_BAD_TYPE) { status = match; break; }
            if (match != XR_XIR_OK) continue;
            if (found != UINT32_MAX) { status = XR_XIR_BAD_STRUCTURE; break; }
            found = binding->function;
        }
        if (status != XR_XIR_OK) break;
        if (found == UINT32_MAX) { status = XR_XIR_BAD_TYPE; break; }
        status = implementation_function(module,record,requirement,closure,found,budget);
    }
    implementation_arguments_free(arguments,count,budget); return status;
}
static XrXirStatus implementation_record_shape(const XrXirModule *module,
    const XrXirImplementation *record, XrXirBudget *budget) {
    const XrXirNominalTable *nominals = module->types ? module->types->nominals : NULL;
    if (!nominals || !nominals->declarations || record->nominal_declaration >= nominals->count ||
        (!!record->bindings != !!record->binding_count)) return XR_XIR_BAD_STRUCTURE;
    uint32_t count = nominals->declarations[record->nominal_declaration].parameter_count;
    XrXirConstraint constraint = {0,&record->interface,1};
    XrXirStatus status = xr_xir_constraint_structure(module->types,constraint,count,budget);
    if (status != XR_XIR_OK) return status;
    const XrXirInterfaceDeclaration *interface = &module->types->interfaces->declarations[record->interface.declaration];
    const XrXirNominalDeclaration *nominal = &nominals->declarations[record->nominal_declaration];
    if (!interface->exported) {
        if (!implementation_charge(budget,0,(uint64_t)interface->module.length+1)) return XR_XIR_BUDGET;
        if (interface->module.length != nominal->module.length ||
            memcmp(interface->module.bytes,nominal->module.bytes,interface->module.length)) return XR_XIR_BAD_TYPE;
    }
    if (!implementation_charge(budget,(uint64_t)record->binding_count*sizeof(*record->bindings),record->binding_count))
        return XR_XIR_BUDGET;
    for (uint32_t b = 0; b < record->binding_count; ++b) {
        const XrXirImplementationBinding *binding = &record->bindings[b];
        constraint.interfaces = &binding->requirement;
        status = xr_xir_constraint_structure(module->types,constraint,count,budget);
        if (status != XR_XIR_OK) return status;
        if (binding->member >= module->types->interfaces->declarations[binding->requirement.declaration].method_count ||
            binding->function >= module->function_count) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
static XrXirStatus implementation_record_prior(const XrXirModule *module,
    const XrXirImplementation *record, XrXirBudget *budget) {
    XrXirInterfaceClosure *closure = NULL;
    uint64_t before = budget->scratch_bytes;
    XrXirInterfaceClosureRoots roots = {module->types->interfaces,module->types,&record->interface,1,
        module->types->nominals->declarations[record->nominal_declaration].parameter_count};
    XrXirStatus status = xr_xir_interface_closure_build(&roots,budget,&closure);
    uint64_t reserved = before-budget->scratch_bytes;
    if (status == XR_XIR_OK) status = implementation_bindings(module,record,closure,budget);
    xr_xir_interface_closure_free(closure); budget->scratch_bytes += reserved; return status;
}
static XrXirStatus implementation_record_conditions(const XrXirModule *module,
    const XrXirImplementation *record, XrXirBudget *budget) {
    XrXirProofContext context = {module,{XR_XIR_CONTEXT_NOMINAL,record->nominal_declaration,0}};
    XrXirStatus status = XR_XIR_OK;
    for (uint32_t a = 0; status == XR_XIR_OK && a < record->interface.argument_count; ++a) {
        XrXirConstraintUse use = {module,{XR_XIR_CONTEXT_INTERFACE,record->interface.declaration,0},a,
            record->interface.arguments,record->interface.argument_count};
        status = xr_xir_constraints_prove(&context,&use,budget);
    }
    for (uint32_t b = 0; status == XR_XIR_OK && b < record->binding_count; ++b) {
        uint32_t function = record->bindings[b].function;
        uint32_t count = module->generics ? module->generics[function].parameter_count : 0;
        XrXirType *arguments = NULL;
        status = implementation_identity_arguments(count,budget,&arguments);
        context.owner = (XrXirDeclarationContext){XR_XIR_CONTEXT_CONFORMANCE_METHOD,
            (uint32_t)(record-module->declarations->implementations->records),b};
        for (uint32_t a = 0; status == XR_XIR_OK && a < count; ++a) {
            XrXirConstraintUse use = {module,{XR_XIR_CONTEXT_FUNCTION,function,0},a,arguments,count};
            status = xr_xir_constraints_prove(&context,&use,budget);
        }
        implementation_arguments_free(arguments,count,budget);
    }
    return status;
}
static XrXirStatus implementation_shared_binding(const XrXirModule *module,
    const XrXirImplementationBinding *a, const XrXirImplementationBinding *b, XrXirBudget *budget) {
    const XrXirInterfaceTable *interfaces = module->types->interfaces;
    XrXirLiteral an = interfaces->declarations[a->requirement.declaration].methods[a->member].name;
    XrXirLiteral bn = interfaces->declarations[b->requirement.declaration].methods[b->member].name;
    if (!implementation_charge(budget,0,an.length == bn.length ? (uint64_t)an.length+1 : 1)) return XR_XIR_BUDGET;
    if (an.length == bn.length && !memcmp(an.bytes,bn.bytes,an.length) && a->function != b->function)
        return XR_XIR_BAD_TYPE;
    return XR_XIR_OK;
}
static XrXirStatus implementation_binding_consistency(const XrXirModule *module, XrXirBudget *budget) {
    const XrXirImplementationTable *table = module->declarations->implementations;
    for (uint32_t i = 0; i < table->count; ++i) {
        const XrXirImplementation *record = &table->records[i];
        for (uint32_t b = 0; b < record->binding_count; ++b) {
            for (uint32_t j = 0; j <= i; ++j) {
                if (!implementation_charge(budget,0,1)) return XR_XIR_BUDGET;
                const XrXirImplementation *previous = &table->records[j];
                if (previous->nominal_declaration != record->nominal_declaration) continue;
                uint32_t before = j == i ? b : previous->binding_count;
                for (uint32_t a = 0; a < before; ++a) {
                    XrXirStatus status = implementation_shared_binding(module,&record->bindings[b],&previous->bindings[a],budget);
                    if (status != XR_XIR_OK) return status;
                }
            }
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus implementation_nominal_roots(const XrXirModule *module,
    uint32_t nominal, XrXirBudget *budget) {
    const XrXirImplementationTable *table = module->declarations->implementations;
    uint32_t count = 0;
    for (uint32_t i = 0; i < table->count; ++i) {
        if (!implementation_charge(budget,0,1)) return XR_XIR_BUDGET;
        count += table->records[i].nominal_declaration == nominal;
    }
    if (!count) return XR_XIR_OK;
    uint64_t bytes = (uint64_t)count*sizeof(XrXirInterfaceApplication);
    if (bytes > SIZE_MAX || bytes > budget->scratch_bytes) return XR_XIR_BUDGET;
    budget->scratch_bytes -= bytes;
    XrXirInterfaceApplication *roots = xr_malloc((size_t)bytes);
    if (!roots) { budget->scratch_bytes += bytes; return XR_XIR_OUT_OF_MEMORY; }
    uint32_t at = 0;
    for (uint32_t i = 0; i < table->count; ++i)
        if (table->records[i].nominal_declaration == nominal) roots[at++] = table->records[i].interface;
    XrXirInterfaceClosure *closure = NULL;
    uint64_t before = budget->scratch_bytes;
    XrXirInterfaceClosureRoots request = {module->types->interfaces,module->types,roots,count,
        module->types->nominals->declarations[nominal].parameter_count};
    XrXirStatus status = xr_xir_interface_closure_build(&request,budget,&closure);
    uint64_t reserved = before-budget->scratch_bytes;
    uint32_t parameters = module->types->nominals->declarations[nominal].parameter_count;
    XrXirType *arguments = NULL;
    if (status == XR_XIR_OK) status = implementation_identity_arguments(parameters,budget,&arguments);
    XrXirInterfaceApplication substitution = {0,arguments,parameters};
    for (uint32_t i = 0; status == XR_XIR_OK && i < count; ++i) {
        for (uint32_t j = 0; j < i; ++j) {
            XrXirStatus match = implementation_application_equal(module->types,module->types,&roots[i],&roots[j],&substitution,budget);
            if (match == XR_XIR_OK) { status = XR_XIR_BAD_STRUCTURE; break; }
            if (match != XR_XIR_BAD_TYPE) { status = match; break; }
        }
    }
    implementation_arguments_free(arguments,parameters,budget);
    xr_free(roots); xr_xir_interface_closure_free(closure); budget->scratch_bytes += bytes+reserved; return status;
}
XrXirStatus xr_xir_implementations_verify(const XrXirModule *module, XrXirBudget *budget) {
    if (!module || !budget) return XR_XIR_BAD_STRUCTURE;
    const XrXirImplementationTable *table = module->declarations ? module->declarations->implementations : NULL;
    if (!table) return XR_XIR_OK;
    if (!table->count || !table->records) return XR_XIR_BAD_STRUCTURE;
    if (!implementation_charge(budget,sizeof(*table)+(uint64_t)table->count*sizeof(*table->records),table->count))
        return XR_XIR_BUDGET;
    for (uint32_t i = 0; i < table->count; ++i) {
        XrXirStatus status = implementation_record_shape(module,&table->records[i],budget);
        if (status != XR_XIR_OK) return status;
    }
    for (uint32_t i = 0; i < table->count; ++i) {
        XrXirStatus status = implementation_record_prior(module,&table->records[i],budget);
        if (status != XR_XIR_OK) return status;
    }
    for (uint32_t n = 0; n < module->types->nominals->count; ++n) {
        XrXirStatus status = implementation_nominal_roots(module,n,budget);
        if (status != XR_XIR_OK) return status;
    }
    XrXirStatus consistency = implementation_binding_consistency(module,budget);
    if (consistency != XR_XIR_OK) return consistency;
    for (uint32_t i = 0; i < table->count; ++i) {
        XrXirStatus status = implementation_record_conditions(module,&table->records[i],budget);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
XrXirStatus xr_xir_witness_resolve(const XrXirProofContext *context,
    const XrXirWitnessRequest *request, XrXirBudget *budget, XrXirWitness *output) {
    if (output) *output = (XrXirWitness){0};
    if (!context || !context->module || !request || !request->declaration_module || !budget || !output)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirTypeNode *node = xr_xir_type_node(context->module->types,request->receiver);
    if (!node || node->kind != XR_XIR_TYPE_NOMINAL) return XR_XIR_BAD_TYPE;
    XrXirStatus status = xr_xir_interface_prove(context,request->declaration_module,request->receiver,request->application,budget);
    if (status != XR_XIR_OK) return status;
    const XrXirDeclarations *declarations = request->declaration_module->declarations;
    const XrXirImplementationTable *table = declarations ? declarations->implementations : NULL;
    if (!table) return XR_XIR_BAD_TYPE;
    XrXirInterfaceApplication substitution = {0,node->nominal.arguments,node->nominal.argument_count};
    uint32_t function = UINT32_MAX;
    for (uint32_t i = 0; i < table->count; ++i) {
        const XrXirImplementation *record = &table->records[i];
        if (!implementation_charge(budget,0,1)) return XR_XIR_BUDGET;
        if (record->nominal_declaration != node->nominal.declaration) continue;
        for (uint32_t b = 0; b < record->binding_count; ++b) {
            const XrXirImplementationBinding *binding = &record->bindings[b];
            if (binding->member != request->member) continue;
            status = implementation_application_equal(request->declaration_module->types,context->module->types,
                &binding->requirement,&request->application,&substitution,budget);
            if (status == XR_XIR_BAD_TYPE) continue;
            if (status != XR_XIR_OK) return status;
            if (function != UINT32_MAX && function != binding->function) return XR_XIR_BAD_TYPE;
            function = binding->function;
        }
    }
    if (function == UINT32_MAX) return XR_XIR_BAD_TYPE;
    *output = (XrXirWitness){function,node->nominal.arguments,node->nominal.argument_count};
    return XR_XIR_OK;
}
