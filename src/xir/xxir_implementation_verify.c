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
#include "xxir_compile_memory.h"
#include "xxir_constraint_proof_internal.h"
#include "xxir_interface_members.h"
#include "xxir_types.h"
#include "../base/xmalloc.h"
#include <string.h>
static bool implementation_charge(XrXirCompileContext *budget, uint64_t bytes, uint64_t work) {
    if (bytes > SIZE_MAX ||!xir_compile_work(budget, work)) return false;
      return true;
}
static XrXirStatus implementation_identity_arguments(uint32_t count, XrXirCompileContext *budget, XrXirType **output) {
    XrXirStatus allocation_status = XR_XIR_OK;

    uint64_t bytes = (uint64_t)count * sizeof(XrXirType);
    if (bytes > SIZE_MAX ||!xir_compile_work(budget, count)) return XR_XIR_BUDGET;

    if (!count) return XR_XIR_OK;
    XrXirType *arguments = xir_compile_alloc(budget, (size_t)bytes, &allocation_status);
    if (!arguments) {  return allocation_status; }
    for (uint32_t a = 0; a < count; ++a) arguments[a] = (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+a);
    *output = arguments; return XR_XIR_OK;
}
static void implementation_arguments_free(XrXirType *arguments) {
    if (arguments) { xr_compile_resources_free(arguments);  }
}
static XrXirStatus implementation_application_equal(const XrXirTypes *from, const XrXirTypes *to,
    const XrXirInterfaceApplication *expected, const XrXirInterfaceApplication *actual,
    const XrXirInterfaceApplication *substitution, XrXirCompileContext *budget) {
    if (!implementation_charge(budget,0,1)) return XR_XIR_BUDGET;
    if (expected->declaration != actual->declaration || expected->argument_count != actual->argument_count)
        return XR_XIR_BAD_TYPE;
    for (uint32_t a = 0; a < expected->argument_count; ++a) {
        XrXirStatus status = xr_xir_compile_type_substitution_matches_between(budget, from, to, substitution->arguments, substitution->argument_count, expected->arguments[a], actual->arguments[a]);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
static XrXirStatus implementation_function(const XrXirModule *module, const XrXirImplementation *record,
    const XrXirInterfaceRequirement *requirement, const XrXirInterfaceClosure *closure,
    uint32_t function, XrXirCompileContext *budget) {
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
    XrXirStatus receiver_status = xr_xir_compile_method_signature_verify(budget, module, function);
    if (receiver_status != XR_XIR_OK) return receiver_status;
    XrXirType *arguments = NULL;
    XrXirStatus status = implementation_identity_arguments(full_count,budget,&arguments);
    for (uint32_t a = 0; status == XR_XIR_OK && a < signature->parameter_count; ++a) {
        if (signature->parameters[a].mode) { status = XR_XIR_BAD_TYPE; break; }
        status = xr_xir_compile_type_substitution_matches_between(budget, types, module->types, arguments, full_count, signature->parameters[a].type, f->parameters[a+1]);
    }
    if (status == XR_XIR_OK) status = xr_xir_compile_type_substitution_matches_between(budget, types, module->types, arguments, full_count, signature->result, f->result);
    implementation_arguments_free(arguments); return status;
}
static XrXirStatus implementation_bindings(const XrXirModule *module, const XrXirImplementation *record,
    XrXirInterfaceClosure *closure, XrXirCompileContext *budget) {
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
    implementation_arguments_free(arguments); return status;
}
static XrXirStatus implementation_record_shape(const XrXirModule *module,
    const XrXirImplementation *record, XrXirCompileContext *budget) {
    const XrXirNominalTable *nominals = module->types ? module->types->nominals : NULL;
    if (!nominals || !nominals->declarations || record->nominal_declaration >= nominals->count ||
        (!!record->bindings != !!record->binding_count)) return XR_XIR_BAD_STRUCTURE;
    uint32_t count = nominals->declarations[record->nominal_declaration].parameter_count;
    XrXirConstraint constraint = {0,&record->interface,1};
    XrXirStatus status = xr_xir_compile_constraint_structure(budget, module->types, constraint, count);
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
        status = xr_xir_compile_constraint_structure(budget, module->types, constraint, count);
        if (status != XR_XIR_OK) return status;
        if (binding->member >= module->types->interfaces->declarations[binding->requirement.declaration].method_count ||
            binding->function >= module->function_count) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
static XrXirStatus implementation_record_prior(const XrXirModule *module,
    const XrXirImplementation *record, XrXirCompileContext *budget) {
    XrXirInterfaceClosure *closure = NULL;
    XrXirInterfaceClosureRoots roots = {module->types->interfaces,module->types,&record->interface,1,
        module->types->nominals->declarations[record->nominal_declaration].parameter_count};
    XrXirStatus status = xr_xir_compile_interface_closure_build(budget, &roots, &closure);
    if (status == XR_XIR_OK) status = implementation_bindings(module,record,closure,budget);
    xr_xir_compile_interface_closure_free(closure);  return status;
}
static XrXirStatus implementation_record_conditions(const XrXirModule *module,
    const XrXirImplementation *record, XrXirCompileContext *budget) {
    XrXirProofContext context = {module,{XR_XIR_CONTEXT_NOMINAL,record->nominal_declaration,0}};
    XirConstraintArguments interface_arguments = {module,
        {XR_XIR_CONTEXT_INTERFACE,record->interface.declaration,0},
        record->interface.arguments,record->interface.argument_count};
    XrXirStatus status = xr_xir_compile_constraint_arguments_prove(budget,&context,&interface_arguments);
    for (uint32_t b = 0; status == XR_XIR_OK && b < record->binding_count; ++b) {
        uint32_t function = record->bindings[b].function;
        uint32_t count = module->generics ? module->generics[function].parameter_count : 0;
        XrXirType *arguments = NULL;
        status = implementation_identity_arguments(count,budget,&arguments);
        context.owner = (XrXirDeclarationContext){XR_XIR_CONTEXT_CONFORMANCE_METHOD,
            (uint32_t)(record-module->declarations->implementations->records),b};
        XirConstraintArguments use = {module,{XR_XIR_CONTEXT_FUNCTION,function,0},arguments,count};
        if (status == XR_XIR_OK) status = xr_xir_compile_constraint_arguments_prove(budget,&context,&use);
        implementation_arguments_free(arguments);
    }
    return status;
}
static XrXirStatus implementation_shared_binding(const XrXirModule *module,
    const XrXirImplementationBinding *a, const XrXirImplementationBinding *b, XrXirCompileContext *budget) {
    const XrXirInterfaceTable *interfaces = module->types->interfaces;
    XrXirLiteral an = interfaces->declarations[a->requirement.declaration].methods[a->member].name;
    XrXirLiteral bn = interfaces->declarations[b->requirement.declaration].methods[b->member].name;
    if (!implementation_charge(budget,0,an.length == bn.length ? (uint64_t)an.length+1 : 1)) return XR_XIR_BUDGET;
    if (an.length == bn.length && !memcmp(an.bytes,bn.bytes,an.length) && a->function != b->function)
        return XR_XIR_BAD_TYPE;
    return XR_XIR_OK;
}
static XrXirStatus implementation_binding_consistency(const XrXirModule *module, XrXirCompileContext *budget) {
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
    uint32_t nominal, XrXirCompileContext *budget) {
    XrXirStatus allocation_status = XR_XIR_OK;
    const XrXirImplementationTable *table = module->declarations->implementations;
    uint32_t count = 0;
    for (uint32_t i = 0; i < table->count; ++i) {
        if (!implementation_charge(budget,0,1)) return XR_XIR_BUDGET;
        count += table->records[i].nominal_declaration == nominal;
    }
    if (!count) return XR_XIR_OK;
    uint64_t bytes = (uint64_t)count*sizeof(XrXirInterfaceApplication);
    if (bytes > SIZE_MAX) return XR_XIR_BUDGET;

    XrXirInterfaceApplication *roots = xir_compile_alloc(budget, (size_t)bytes, &allocation_status);
    if (!roots) {  return allocation_status; }
    uint32_t at = 0;
    for (uint32_t i = 0; i < table->count; ++i)
        if (table->records[i].nominal_declaration == nominal) roots[at++] = table->records[i].interface;
    XrXirInterfaceClosure *closure = NULL;
    XrXirInterfaceClosureRoots request = {module->types->interfaces,module->types,roots,count,
        module->types->nominals->declarations[nominal].parameter_count};
    XrXirStatus status = xr_xir_compile_interface_closure_build(budget, &request, &closure);
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
    implementation_arguments_free(arguments);
    xr_compile_resources_free(roots); xr_xir_compile_interface_closure_free(closure);  return status;
}
XrXirStatus xr_xir_compile_implementations_verify(const XrXirCompileContext *compile_context, const XrXirModule *module) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *budget = &compile_state;
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
XrXirStatus xr_xir_compile_witness_resolve(const XrXirCompileContext *compile_context, const XrXirProofContext *context, const XrXirWitnessRequest *request, XrXirWitness *output) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *budget = &compile_state;
    if (!context || !context->module || !request || !request->declaration_module || !budget || !output)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirTypeNode *node = xr_xir_type_node(context->module->types,request->receiver);
    if (!node || node->kind != XR_XIR_TYPE_NOMINAL) return XR_XIR_BAD_TYPE;
    XrXirStatus status = xr_xir_compile_interface_prove(budget, context, request->declaration_module, request->receiver, request->application);
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
