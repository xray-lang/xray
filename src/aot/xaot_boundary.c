/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_boundary.c - explicit AOT representation boundary plan
 */

#include "xaot_boundary.h"
#include "xaot_bundle.h"
#include "refine/xr_aot_scalar_value.h"
#include "../ir/xi_module.h"
#include <stdio.h>
#include <string.h>

static const XiImportRef *module_import_ref_for_value(const XaotBundle *bundle,
                                                      const XiFunc *current, const XiValue *value);
static const XiClassData *resolve_imported_class(const XaotBundle *bundle, const XiImportRef *ref,
                                                 const XiModule **owner_out);

static XaotDirectI64TargetStatus direct_i64_error(char *errbuf, size_t errbuf_len,
                                                  const char *message) {
    if (errbuf && errbuf_len > 0)
        snprintf(errbuf, errbuf_len, "%s", message ? message : "invalid direct-i64 TargetPlan");
    return XAOT_DIRECT_I64_TARGET_INVALID;
}

static void direct_i64_find_function(const XiFunc *function, const XrSemanticPlan *semantic,
                                     uint32_t semantic_function, const XiFunc **match,
                                     uint32_t *match_count) {
    if (!function || !match || !match_count)
        return;
    if (function->semantic_plan == semantic &&
        function->semantic_plan_function_index == semantic_function) {
        *match = function;
        (*match_count)++;
    }
    for (uint16_t i = 0; i < function->nchildren; i++)
        direct_i64_find_function(function->children[i], semantic, semantic_function, match,
                                 match_count);
}

static bool direct_i64_machine_rep(const XrTargetPlan *target, uint16_t rep) {
    const XrTargetMachineRepRecord *machine = xr_target_plan_machine_rep(target, rep);
    return machine && machine->kind == XR_MACHINE_REP_I64 &&
           machine->ownership == XR_TARGET_OWNERSHIP_TRIVIAL;
}

XR_FUNC XaotDirectI64TargetStatus xaot_boundary_direct_i64_function_status(
    const XaotBundle *bundle, const XiFunc *function, const XrTargetPlan **target_out,
    const XrTargetFunctionRecord **function_out, char *errbuf, size_t errbuf_len) {
    if (target_out)
        *target_out = NULL;
    if (function_out)
        *function_out = NULL;
    if (!bundle || !function || !function->semantic_plan ||
        function->semantic_plan_function_index == XR_SEMANTIC_INDEX_NONE)
        return XAOT_DIRECT_I64_TARGET_UNCOVERED;

    const XrTargetPlan *target = xaot_bundle_target_plan_for_func(bundle, function);
    if (!target || xr_target_plan_semantic_plan(target) != function->semantic_plan ||
        xr_target_plan_completed_family_mask(target) != XR_TARGET_REQUIRED_FAMILIES ||
        !xr_target_plan_is_verified(target) || !xr_target_plan_fingerprint_is_intact(target))
        return direct_i64_error(errbuf, errbuf_len,
                                "direct-i64 function has corrupt TargetPlan authority");

    uint32_t function_count = 0;
    const XrTargetFunctionRecord *functions = xr_target_plan_functions(target, &function_count);
    uint32_t index = function->semantic_plan_function_index;
    if (!functions || index >= function_count || functions[index].id != index ||
        functions[index].semantic_function != index)
        return direct_i64_error(errbuf, errbuf_len,
                                "direct-i64 function has no exact TargetPlan function row");
    if (xr_target_plan_function_execution_family_mask(target, index) !=
        XR_TARGET_EXECUTION_SCALAR_I64_CLOSED)
        return XAOT_DIRECT_I64_TARGET_UNCOVERED;

    if (target_out)
        *target_out = target;
    if (function_out)
        *function_out = &functions[index];
    return XAOT_DIRECT_I64_TARGET_FOUND;
}

XR_FUNC XaotDirectI64TargetStatus xaot_boundary_direct_i64_abi_status(
    const XaotBundle *bundle, const XiFunc *function, char *errbuf, size_t errbuf_len) {
    const XrTargetPlan *target = NULL;
    const XrTargetFunctionRecord *function_row = NULL;
    XaotDirectI64TargetStatus status = xaot_boundary_direct_i64_function_status(
        bundle, function, &target, &function_row, errbuf, errbuf_len);
    if (status != XAOT_DIRECT_I64_TARGET_FOUND)
        return status;

    /* A function can relinquish its legacy ABI only when every inbound edge
     * is itself an executable closed-i64 call row.  A module initializer may
     * call an otherwise closed helper while also performing unsupported work;
     * that helper keeps its legacy ABI until the initializer family migrates. */
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target, &call_count);
    for (uint32_t i = 0; calls && i < call_count; i++) {
        if (calls[i].callee_function != function_row->semantic_function)
            continue;
        if (calls[i].target_kind != XR_TARGET_CALL_TARGET_DIRECT_LOCAL ||
            calls[i].calling_convention != XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL ||
            xr_target_plan_function_execution_family_mask(target, calls[i].caller_function) !=
                XR_TARGET_EXECUTION_SCALAR_I64_CLOSED)
            return XAOT_DIRECT_I64_TARGET_UNCOVERED;
        uint32_t instruction_count = 0;
        const XrTargetInstructionRecord *instructions = xr_target_plan_function_instructions(
            target, calls[i].caller_function, &instruction_count);
        uint32_t matches = 0;
        for (uint32_t instruction = 0; instructions && instruction < instruction_count;
             instruction++) {
            if (instructions[instruction].opcode == XR_TARGET_INSTRUCTION_CALL_DIRECT_I64 &&
                instructions[instruction].immediate_bits == calls[i].id)
                matches++;
        }
        if (matches != 1)
            return direct_i64_error(errbuf, errbuf_len,
                                    "direct-i64 inbound instruction authority is inexact");
    }
    return XAOT_DIRECT_I64_TARGET_FOUND;
}

XR_FUNC XaotDirectI64TargetStatus xaot_boundary_direct_i64_call_view(
    const XaotBundle *bundle, const XiFunc *caller, const XiValue *call,
    XaotDirectI64TargetView *out, char *errbuf, size_t errbuf_len) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!bundle || !caller || !call || !out)
        return direct_i64_error(errbuf, errbuf_len, "direct-i64 call view input is incomplete");

    const XrTargetPlan *target = NULL;
    const XrTargetFunctionRecord *caller_row = NULL;
    XaotDirectI64TargetStatus status = xaot_boundary_direct_i64_function_status(
        bundle, caller, &target, &caller_row, errbuf, errbuf_len);
    if (status != XAOT_DIRECT_I64_TARGET_FOUND)
        return status;
    if (call->op != XI_CALL || call->nargs != 2 || !call->args || !call->args[1])
        return direct_i64_error(errbuf, errbuf_len,
                                "covered direct-i64 call has invalid live binding");

    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t semantic_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t argument_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t argument_value = XR_SEMANTIC_INDEX_NONE;
    if (!xr_aot_scalar_semantic_value_id(target, caller, call, &semantic_function,
                                         &semantic_value, errbuf, errbuf_len) ||
        !xr_aot_scalar_semantic_value_id(target, caller, call->args[1], &argument_function,
                                         &argument_value, errbuf, errbuf_len) ||
        semantic_function != caller_row->semantic_function ||
        argument_function != semantic_function)
        return direct_i64_error(errbuf, errbuf_len,
                                "covered direct-i64 call lacks exact semantic value identity");

    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target);
    const XrSemanticOperationRecord *operation = NULL;
    uint32_t operation_index = XR_SEMANTIC_INDEX_NONE;
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(semantic);
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(semantic, i);
        if (!candidate || candidate->function != semantic_function ||
            candidate->result_value != semantic_value)
            continue;
        if (operation)
            return direct_i64_error(errbuf, errbuf_len,
                                    "covered direct-i64 call semantic operation is ambiguous");
        operation = candidate;
        operation_index = i;
    }
    if (!operation)
        return direct_i64_error(errbuf, errbuf_len,
                                "covered direct-i64 call has no semantic operation");

    const XrTargetCallRecord *target_call = NULL;
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target, &call_count);
    for (uint32_t i = 0; calls && i < call_count; i++) {
        if (calls[i].semantic_operation != operation_index)
            continue;
        if (target_call)
            return direct_i64_error(errbuf, errbuf_len,
                                    "covered direct-i64 TargetPlan call is ambiguous");
        target_call = &calls[i];
    }
    if (!target_call || target_call->caller_function != semantic_function ||
        target_call->result_value != semantic_value || target_call->argument_count != 1 ||
        target_call->adapter_count != 0 ||
        target_call->calling_convention != XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL ||
        target_call->target_kind != XR_TARGET_CALL_TARGET_DIRECT_LOCAL ||
        target_call->result_mode != XR_TARGET_CALL_VALUE ||
        target_call->result_ownership != XR_TARGET_CALL_NONE ||
        target_call->error_mode != XR_TARGET_CALL_NO_CALL_OWNED_CHANNEL ||
        target_call->error_slot != XR_SEMANTIC_INDEX_NONE ||
        target_call->result_slot == XR_TARGET_INSTRUCTION_SLOT_NONE ||
        !direct_i64_machine_rep(target, target_call->result_register_rep) ||
        !direct_i64_machine_rep(target, target_call->result_memory_rep))
        return direct_i64_error(errbuf, errbuf_len,
                                "covered direct-i64 TargetPlan call row is inexact");

    const XrSemanticCallTargetRecord *semantic_target =
        xr_semantic_plan_call_target(semantic, target_call->semantic_call_target);
    if (!semantic_target || semantic_target->operation != operation_index ||
        semantic_target->function != target_call->callee_function ||
        semantic_target->kind != XR_SEM_CALL_TARGET_DIRECT_LOCAL)
        return direct_i64_error(errbuf, errbuf_len,
                                "covered direct-i64 callee identity is inexact");

    uint32_t function_count = 0;
    const XrTargetFunctionRecord *functions = xr_target_plan_functions(target, &function_count);
    if (!functions || target_call->callee_function >= function_count ||
        functions[target_call->callee_function].id != target_call->callee_function ||
        functions[target_call->callee_function].semantic_function != target_call->callee_function ||
        xr_target_plan_function_execution_family_mask(target, target_call->callee_function) !=
            XR_TARGET_EXECUTION_SCALAR_I64_CLOSED)
        return direct_i64_error(errbuf, errbuf_len,
                                "covered direct-i64 callee function row is inexact");

    uint32_t argument_count = 0;
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(target, &argument_count);
    if (!arguments || target_call->argument_begin >= argument_count)
        return direct_i64_error(errbuf, errbuf_len,
                                "covered direct-i64 argument row is missing");
    const XrTargetCallArgumentRecord *argument = &arguments[target_call->argument_begin];
    if (argument->call != target_call->id || argument->ordinal != 0 ||
        argument->semantic_value != argument_value || argument->mode != XR_TARGET_CALL_VALUE ||
        argument->ownership != XR_TARGET_CALL_CONSUME ||
        argument->transfer_mode != XR_TRANSFER_SHARE || argument->flags != 0 ||
        argument->caller_slot == XR_TARGET_INSTRUCTION_SLOT_NONE ||
        argument->callee_slot == XR_TARGET_INSTRUCTION_SLOT_NONE ||
        !direct_i64_machine_rep(target, argument->register_rep) ||
        !direct_i64_machine_rep(target, argument->memory_rep) ||
        !direct_i64_machine_rep(target, argument->callee_register_rep) ||
        !direct_i64_machine_rep(target, argument->callee_memory_rep))
        return direct_i64_error(errbuf, errbuf_len,
                                "covered direct-i64 argument representation is inexact");

    const XiFunc *callee = NULL;
    uint32_t callee_count = 0;
    for (uint32_t i = 0; i < bundle->nmodules; i++) {
        const XiModule *module = bundle->modules ? bundle->modules[i] : NULL;
        if (module && module->init && module->init->semantic_plan == semantic)
            direct_i64_find_function(module->init, semantic, target_call->callee_function, &callee,
                                     &callee_count);
    }
    if (!callee || callee_count != 1)
        return direct_i64_error(errbuf, errbuf_len,
                                "covered direct-i64 Xi callee identity is not unique");

    const XrTargetInstructionRecord *call_instruction = NULL;
    uint32_t instruction_count = 0;
    const XrTargetInstructionRecord *instructions = xr_target_plan_function_instructions(
        target, semantic_function, &instruction_count);
    for (uint32_t i = 0; instructions && i < instruction_count; i++) {
        if (instructions[i].opcode != XR_TARGET_INSTRUCTION_CALL_DIRECT_I64 ||
            instructions[i].immediate_bits != target_call->id)
            continue;
        if (call_instruction)
            return direct_i64_error(errbuf, errbuf_len,
                                    "covered direct-i64 instruction row is ambiguous");
        call_instruction = &instructions[i];
    }
    if (!call_instruction || call_instruction->function != semantic_function ||
        call_instruction->result_slot != target_call->result_slot ||
        call_instruction->operand_count != 0 ||
        call_instruction->operand_slots[0] != XR_TARGET_INSTRUCTION_SLOT_NONE ||
        call_instruction->operand_slots[1] != XR_TARGET_INSTRUCTION_SLOT_NONE)
        return direct_i64_error(errbuf, errbuf_len,
                                "covered direct-i64 instruction row is inexact");

    out->target_plan = target;
    out->caller_function = caller_row;
    out->callee_function = &functions[target_call->callee_function];
    out->call = target_call;
    out->argument = argument;
    out->call_instruction = call_instruction;
    out->callee = callee;
    out->argument_value = call->args[1];
    out->target_fingerprint = xr_target_plan_fingerprint(target);
    return XAOT_DIRECT_I64_TARGET_FOUND;
}

static const XiFunc *boundary_find_constructor(const XiFunc *parent,
                                               const XiClassData *class_data) {
    if (!parent || !class_data || !class_data->methods || !class_data->child_idx)
        return NULL;
    for (uint16_t i = 0; i < class_data->nmethod; i++) {
        const XiClassMethod *method = &class_data->methods[i];
        if (!method->is_constructor || method->is_static_constructor ||
            i >= class_data->ninst + class_data->nstat)
            continue;
        uint16_t child_index = class_data->child_idx[i];
        if (child_index < parent->nchildren) {
            const XiFunc *child = parent->children[child_index];
            if (child && child->name && strcmp(child->name, "constructor") == 0)
                return child;
        }
    }
    return NULL;
}

static const XiFunc *boundary_resolve_shared_constructor(const XaotBundle *bundle,
                                                         const XiFunc *current, int slot) {
    if (!bundle || !current || slot < 0)
        return NULL;
    for (uint32_t i = 0; i < bundle->nfunc_plans; i++) {
        const XaotFuncPlan *plan = &bundle->func_plans[i];
        if (plan->func != current || plan->module_index >= bundle->nmodules)
            continue;
        const XiModule *module = bundle->modules[plan->module_index];
        if (!module || slot >= module->nslots || !module->slot_classes)
            return NULL;
        return boundary_find_constructor(module->init, module->slot_classes[slot]);
    }
    return NULL;
}

XR_FUNC const XiFunc *xaot_boundary_resolve_constructor_call_target(const XaotBundle *bundle,
                                                                    const XiFunc *current,
                                                                    const XiValue *call,
                                                                    uint16_t *first_arg_out,
                                                                    uint16_t *first_param_out) {
    const XiFunc *target = NULL;
    if (first_arg_out)
        *first_arg_out = 0;
    if (first_param_out)
        *first_param_out = 0;
    if (!bundle || !current || !call || call->nargs < 1)
        return NULL;
    if (call->op == XI_CALL) {
        const XiValue *callee = xi_value_trace_repr(call->args[0]);
        if (callee && callee->op == XI_GET_SHARED)
            target = boundary_resolve_shared_constructor(bundle, current, (int) callee->aux_int);
    } else if (call->op == XI_CALL_METHOD && call->aux && (call->aux_int & 1) == 0) {
        const XiImportRef *module_ref = module_import_ref_for_value(bundle, current, call->args[0]);
        if (module_ref) {
            XiImportRef class_ref = *module_ref;
            const XiModule *owner = NULL;
            class_ref.member_name = (const char *) call->aux;
            class_ref.resolved_shared_slot = -1;
            class_ref.resolved_export_slot = -1;
            const XiClassData *cls = resolve_imported_class(bundle, &class_ref, &owner);
            if (cls && owner && owner->init)
                target = boundary_find_constructor(owner->init, cls);
        }
    }
    if (target) {
        if (first_arg_out)
            *first_arg_out = 1;
        if (first_param_out)
            *first_param_out = 1;
    }
    return target;
}

XR_FUNC const char *xaot_boundary_reason_name(XaotBoundaryReason reason) {
    switch (reason) {
        case XAOT_BOUNDARY_NONE:
            return "none";
        case XAOT_BOUNDARY_DIRECT_CALL:
            return "direct-call";
        case XAOT_BOUNDARY_DYNAMIC_CALL:
            return "dynamic-call";
        case XAOT_BOUNDARY_CLOSURE_OBJECT:
            return "closure-object";
        case XAOT_BOUNDARY_MODULE_INIT:
            return "module-init";
        case XAOT_BOUNDARY_EXCEPTION_FLOW:
            return "exception-flow";
        case XAOT_BOUNDARY_CORO_FRAME:
            return "coro-frame";
        case XAOT_BOUNDARY_TAGGED_TYPE:
            return "tagged-type";
        case XAOT_BOUNDARY_BOX:
            return "box";
        case XAOT_BOUNDARY_UNBOX:
            return "unbox";
        case XAOT_BOUNDARY_SHARED_SLOT:
            return "shared-slot";
        case XAOT_BOUNDARY_IMPORT_EXPORT:
            return "import-export";
        case XAOT_BOUNDARY_UNION_NULLABLE:
            return "union-nullable";
        case XAOT_BOUNDARY_RUNTIME_HELPER:
            return "runtime-helper";
        case XAOT_BOUNDARY_CORO_RESULT:
            return "coro-result";
        default:
            return "?";
    }
}

XR_FUNC const char *xaot_boundary_step_kind_name(XaotBoundaryStepKind kind) {
    switch (kind) {
        case XAOT_BOUNDARY_STEP_FUNC_ABI:
            return "func-abi";
        case XAOT_BOUNDARY_STEP_VALUE_REP:
            return "value-rep";
        case XAOT_BOUNDARY_STEP_DIRECT_CALL_ARG:
            return "direct-call-arg";
        case XAOT_BOUNDARY_STEP_DIRECT_CALL_RET:
            return "direct-call-ret";
        default:
            return "?";
    }
}

static const XiImportRef *value_import_ref(const XiValue *v) {
    v = xi_value_trace_repr(v);
    if (!v || v->op != XI_IMPORT_REF || !v->aux)
        return NULL;
    return (const XiImportRef *) v->aux;
}

static const XiImportRef *shared_slot_import_ref(const XiFunc *f, int slot) {
    uint32_t bi;

    if (!f || slot < 0)
        return NULL;
    for (bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        uint32_t vi;
        if (!blk)
            continue;
        for (vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v || v->op != XI_SET_SHARED || (int) v->aux_int != slot || v->nargs < 1)
                continue;
            const XiImportRef *ref = value_import_ref(v->args[0]);
            if (ref)
                return ref;
        }
    }
    return NULL;
}

static const XiImportRef *module_slot_import_ref(const XiModule *mod, int slot) {
    if (!mod || slot < 0 || slot >= mod->nslots || !mod->slot_imports)
        return NULL;
    return mod->slot_imports[slot];
}

static const XiModule *bundle_module_for_func(const XaotBundle *bundle, const XiFunc *func) {
    const XaotFuncPlan *plan;
    if (!bundle || !func)
        return NULL;
    plan = xaot_bundle_find_func_plan(bundle, func);
    if (plan && plan->module_index < bundle->nmodules)
        return bundle->modules[plan->module_index];
    for (const XiFunc *f = func; f; f = f->parent_func) {
        if (f->module)
            return f->module;
    }
    return NULL;
}

static bool module_matches_import(const XiModule *mod, const XiImportRef *ref) {
    if (!mod || !ref || !ref->module_path)
        return false;
    if (mod->path && strcmp(mod->path, ref->module_path) == 0)
        return true;
    if (mod->name && strcmp(mod->name, ref->module_path) == 0)
        return true;
    return false;
}

static const XiFunc *resolve_export_in_module(const XiModule *mod, const XiImportRef *ref) {
    uint16_t ei;

    if (!mod || !ref)
        return NULL;
    if (ref->resolved_shared_slot >= 0 && ref->resolved_shared_slot < mod->nslots &&
        mod->slot_funcs) {
        const XiFunc *slot_func = mod->slot_funcs[ref->resolved_shared_slot];
        if (slot_func)
            return slot_func;
    }
    if (!ref->member_name)
        return NULL;
    for (ei = 0; ei < mod->nexports; ei++) {
        const XiModuleExport *exp = &mod->exports[ei];
        if (exp->function && exp->name && strcmp(exp->name, ref->member_name) == 0)
            return exp->function;
    }
    return NULL;
}

static const XiFunc *resolve_import_ref(const XaotBundle *bundle, const XiImportRef *ref) {
    uint32_t mi;

    if (!bundle || !ref)
        return NULL;
    if (ref->resolved_mod_index >= 0 && (uint32_t) ref->resolved_mod_index < bundle->nmodules) {
        const XiFunc *target =
            resolve_export_in_module(bundle->modules[ref->resolved_mod_index], ref);
        if (target)
            return target;
    }
    for (mi = 0; mi < bundle->nmodules; mi++) {
        const XiModule *mod = bundle->modules[mi];
        const XiFunc *target;
        if (!module_matches_import(mod, ref))
            continue;
        target = resolve_export_in_module(mod, ref);
        if (target)
            return target;
    }
    return NULL;
}

static const XiImportRef *module_import_ref_for_value(const XaotBundle *bundle,
                                                      const XiFunc *current, const XiValue *value) {
    const XiModule *mod;
    const XiValue *v = xi_value_trace_repr(value);
    const XiImportRef *ref = value_import_ref(v);

    if (ref && !ref->member_name)
        return ref;
    if (!v || v->op != XI_GET_SHARED)
        return NULL;

    int slot = (int) v->aux_int;
    mod = bundle_module_for_func(bundle, current);
    ref = module_slot_import_ref(mod, slot);
    if (ref && !ref->member_name)
        return ref;
    ref = shared_slot_import_ref(current, slot);
    if (!ref && current && current->module && current->module->init != current)
        ref = shared_slot_import_ref(current->module->init, slot);
    return ref && !ref->member_name ? ref : NULL;
}

static const XiFunc *resolve_module_member_target(const XaotBundle *bundle, const XiFunc *current,
                                                  const XiValue *call) {
    const char *member_name;
    const XiImportRef *module_ref;
    XiImportRef member_ref;

    if (!bundle || !call || call->op != XI_CALL_METHOD || call->nargs < 1 || !call->aux)
        return NULL;
    if ((call->aux_int & 1) != 0)
        return NULL;

    member_name = (const char *) call->aux;
    module_ref = module_import_ref_for_value(bundle, current, call->args[0]);
    if (!module_ref)
        return NULL;

    member_ref = *module_ref;
    member_ref.member_name = member_name;
    member_ref.resolved_shared_slot = -1;
    member_ref.resolved_export_slot = -1;
    return resolve_import_ref(bundle, &member_ref);
}

static const XiImportRef *binding_import_ref_for_value(const XaotBundle *bundle,
                                                       const XiFunc *current,
                                                       const XiValue *value) {
    const XiValue *v = xi_value_trace_repr(value);
    const XiImportRef *ref = value_import_ref(v);
    const XiModule *mod;

    if (ref)
        return ref;
    if (!v || v->op != XI_GET_SHARED)
        return NULL;
    mod = bundle_module_for_func(bundle, current);
    ref = module_slot_import_ref(mod, (int) v->aux_int);
    if (ref)
        return ref;
    ref = shared_slot_import_ref(current, (int) v->aux_int);
    if (!ref && current && current->module && current->module->init != current)
        ref = shared_slot_import_ref(current->module->init, (int) v->aux_int);
    return ref;
}

static const XiClassData *resolve_imported_class(const XaotBundle *bundle, const XiImportRef *ref,
                                                 const XiModule **owner_out) {
    if (owner_out)
        *owner_out = NULL;
    if (!bundle || !ref || !ref->member_name)
        return NULL;
    for (uint32_t mi = 0; mi < bundle->nmodules; mi++) {
        const XiModule *mod = bundle->modules[mi];
        bool resolved_match =
            ref->resolved_mod_index >= 0 && (uint32_t) ref->resolved_mod_index == mi;
        if (!mod || (!resolved_match && !module_matches_import(mod, ref)))
            continue;
        if (resolved_match && ref->resolved_shared_slot >= 0 &&
            ref->resolved_shared_slot < mod->nslots && mod->slot_classes &&
            mod->slot_classes[ref->resolved_shared_slot]) {
            if (owner_out)
                *owner_out = mod;
            return mod->slot_classes[ref->resolved_shared_slot];
        }
        for (uint16_t ei = 0; ei < mod->nexports; ei++) {
            const XiModuleExport *exp = &mod->exports[ei];
            if (!exp->class_data || !exp->name || strcmp(exp->name, ref->member_name) != 0)
                continue;
            if (owner_out)
                *owner_out = mod;
            return exp->class_data;
        }
    }
    return NULL;
}

static const XiFunc *resolve_imported_static_method_target(const XaotBundle *bundle,
                                                           const XiFunc *current,
                                                           const XiValue *call) {
    const XiImportRef *ref;
    XiImportRef nested_ref;
    const XiClassData *cls;
    const XiModule *owner;
    const char *method_name;

    if (!bundle || !current || !call || call->op != XI_CALL_METHOD || call->nargs < 1 ||
        !call->aux || (call->aux_int & 1) != 0)
        return NULL;
    ref = binding_import_ref_for_value(bundle, current, call->args[0]);
    if (!ref) {
        const XiValue *receiver = xi_value_trace_repr(call->args[0]);
        const XiImportRef *module_ref =
            receiver && receiver->op == XI_LOAD_FIELD && receiver->nargs >= 1 && receiver->aux
                ? module_import_ref_for_value(bundle, current, receiver->args[0])
                : NULL;
        if (module_ref) {
            nested_ref = *module_ref;
            nested_ref.member_name = (const char *) receiver->aux;
            nested_ref.resolved_shared_slot = -1;
            nested_ref.resolved_export_slot = -1;
            ref = &nested_ref;
        }
    }
    cls = resolve_imported_class(bundle, ref, &owner);
    method_name = (const char *) call->aux;
    if (!cls || !owner || !owner->init || !cls->methods || !cls->child_idx)
        return NULL;
    for (uint16_t mi = 0; mi < cls->nmethod; mi++) {
        const XiClassMethod *method = &cls->methods[mi];
        uint16_t child_idx;
        if (!method->is_static || method->is_static_constructor || !method->name ||
            strcmp(method->name, method_name) != 0)
            continue;
        child_idx = cls->child_idx[mi];
        return child_idx < owner->init->nchildren ? owner->init->children[child_idx] : NULL;
    }
    return NULL;
}

static const XiFunc *resolve_local_static_method_target(const XaotBundle *bundle,
                                                        const XiFunc *current,
                                                        const XiValue *call) {
    if (!bundle || !current || !call || call->op != XI_CALL_METHOD || call->nargs < 1 ||
        !call->aux || (call->aux_int & 1) != 0)
        return NULL;
    const XiValue *receiver = xi_value_trace_repr(call->args[0]);
    const XiModule *mod = bundle_module_for_func(bundle, current);
    if (!receiver || receiver->op != XI_GET_SHARED || !mod || !mod->slot_classes ||
        receiver->aux_int < 0 || receiver->aux_int >= mod->nslots)
        return NULL;
    const XiClassData *cls = mod->slot_classes[receiver->aux_int];
    if (!cls || !cls->methods || !cls->child_idx || !mod->init)
        return NULL;
    const char *method_name = (const char *) call->aux;
    for (uint16_t mi = 0; mi < cls->nmethod; mi++) {
        const XiClassMethod *method = &cls->methods[mi];
        if (!method->is_static || method->is_static_constructor || !method->name ||
            strcmp(method->name, method_name) != 0)
            continue;
        uint16_t child_idx = cls->child_idx[mi];
        return child_idx < mod->init->nchildren ? mod->init->children[child_idx] : NULL;
    }
    return NULL;
}

static const XiFunc *resolve_shared_function(const XaotBundle *bundle, const XiFunc *current,
                                             int slot) {
    const XiModule *mod = NULL;
    const XiFunc *f;
    const XiImportRef *ref;

    if (!current || slot < 0)
        return NULL;
    mod = bundle_module_for_func(bundle, current);
    /* Slot metadata lives on the module init function; walk the lexical
     * parent chain so calls made inside nested functions resolve too. */
    for (f = current; f; f = f->parent_func) {
        if (f->shared_slot_funcs && slot < f->shared_slot_func_count && f->shared_slot_funcs[slot])
            return f->shared_slot_funcs[slot];
    }
    if (mod && slot < mod->nslots && mod->slot_funcs && mod->slot_funcs[slot])
        return mod->slot_funcs[slot];

    ref = module_slot_import_ref(mod, slot);
    if (ref)
        return resolve_import_ref(bundle, ref);

    ref = shared_slot_import_ref(current, slot);
    if (!ref && mod && mod->init && mod->init != current)
        ref = shared_slot_import_ref(mod->init, slot);
    return resolve_import_ref(bundle, ref);
}

static const char *receiver_class_name(const XiValue *recv) {
    if (!recv || !recv->type)
        return NULL;
    if ((recv->type->kind == XR_KIND_CLASS || recv->type->kind == XR_KIND_INSTANCE) &&
        recv->type->instance.class_name)
        return recv->type->instance.class_name;
    return NULL;
}

static const XiFunc *method_func_from_class(const XiModule *mod, const XiClassData *cd,
                                            const char *method_name, int method_index,
                                            bool is_static_call) {
    if (!mod || !mod->init || !cd || !cd->methods || !cd->child_idx)
        return NULL;
    for (uint16_t mi = 0; mi < cd->nmethod; mi++) {
        const XiClassMethod *method = &cd->methods[mi];
        uint16_t child_idx;
        if (method->is_static_constructor || method->is_constructor ||
            method->is_static != is_static_call)
            continue;
        if (method_index >= 0) {
            if ((int) mi != method_index)
                continue;
        } else if (!method_name || !method->name || strcmp(method->name, method_name) != 0) {
            continue;
        }
        if (mi >= cd->ninst + cd->nstat)
            return NULL;
        child_idx = cd->child_idx[mi];
        if (child_idx >= mod->init->nchildren)
            return NULL;
        return mod->init->children[child_idx];
    }
    return NULL;
}

static const XiFunc *resolve_method_target(const XaotBundle *bundle, const XiValue *call) {
    const char *class_name;
    const char *method_name;
    int method_index = -1;
    bool is_static_call;
    uint32_t mi;

    if (!bundle || !call || (call->op != XI_CALL_METHOD && call->op != XI_CALL_METHOD_DIRECT) ||
        call->nargs < 1)
        return NULL;
    if (call->op == XI_CALL_METHOD && (call->aux_int & 1) != 0)
        return NULL;

    class_name = receiver_class_name(call->args[0]);
    if (!class_name)
        return NULL;
    is_static_call = call->args[0]->type && call->args[0]->type->kind == XR_KIND_CLASS;
    method_name = call->aux ? (const char *) call->aux : NULL;
    if (call->op == XI_CALL_METHOD_DIRECT)
        method_index = (int) call->aux_int;

    for (mi = 0; mi < bundle->nmodules; mi++) {
        const XiModule *mod = bundle->modules[mi];
        uint16_t si;
        if (!mod || !mod->slot_classes)
            continue;
        for (si = 0; si < mod->nslots; si++) {
            const XiClassData *cd = mod->slot_classes[si];
            const XiFunc *target;
            if (!cd || !cd->class_name || strcmp(cd->class_name, class_name) != 0)
                continue;
            target = method_func_from_class(mod, cd, method_name, method_index, is_static_call);
            if (target)
                return target;
        }
    }
    return NULL;
}

static const XiFunc *resolve_dispatch_plan_target(const XaotBundle *bundle, const XiValue *call) {
    const XaotMethodDispatchPlan *plan;
    const XaotDispatchTargetCase *target;
    if (!bundle || !call || (call->op != XI_CALL_METHOD && call->op != XI_CALL_METHOD_DIRECT))
        return NULL;
    plan = xaot_bundle_find_method_dispatch_plan_for_xi_call(bundle, call);
    if (!plan || plan->target_count != 1 || plan->target_start == 0 ||
        plan->target_start - 1 >= bundle->ndispatch_target_cases)
        return NULL;
    target = &bundle->dispatch_target_cases[plan->target_start - 1];
    return xaot_bundle_find_dispatch_target_func(bundle, target, NULL);
}

XR_FUNC const XiFunc *xaot_boundary_resolve_direct_call_target(const XaotBundle *bundle,
                                                               const XiFunc *current,
                                                               const XiValue *call,
                                                               uint16_t *first_arg_out) {
    const XiValue *callee;

    if (first_arg_out)
        *first_arg_out = 0;
    if (!bundle || !current || !call)
        return NULL;
    /* Covered closed-i64 calls have a single TargetPlan owner.  Returning no
     * legacy answer here makes any missed consumer fail closed instead of
     * silently reconstructing the callee from a closure/name shape. */
    XaotDirectI64TargetStatus direct_i64 = xaot_boundary_direct_i64_function_status(
        bundle, current, NULL, NULL, NULL, 0);
    if (direct_i64 != XAOT_DIRECT_I64_TARGET_UNCOVERED)
        return NULL;
    if (call->op == XI_CALL_METHOD || call->op == XI_CALL_METHOD_DIRECT) {
        /* Module-member call: args[0] is the imported namespace object, not a
         * receiver. The target is a plain function whose ABI has no `this`
         * slot, so argument mapping starts at args[1]. */
        const XiFunc *target = resolve_module_member_target(bundle, current, call);
        if (target && first_arg_out)
            *first_arg_out = 1;
        if (target)
            return target;
        target = resolve_local_static_method_target(bundle, current, call);
        if (target) {
            if (first_arg_out)
                *first_arg_out = 1;
            return target;
        }
        target = resolve_imported_static_method_target(bundle, current, call);
        if (target) {
            if (first_arg_out)
                *first_arg_out = 1;
            return target;
        }
        target = resolve_dispatch_plan_target(bundle, call);
        if (target)
            return target;
        return resolve_method_target(bundle, call);
    }
    if (call->op != XI_CALL || call->nargs < 1)
        return NULL;
    if (first_arg_out)
        *first_arg_out = 1;

    callee = xi_value_trace_repr(call->args[0]);
    if (!callee)
        return NULL;
    if (callee->op == XI_CLOSURE_NEW && callee->aux)
        return (const XiFunc *) callee->aux;
    if (callee->op == XI_STACK_ALLOC && callee->aux_int == XI_CLOSURE_NEW && callee->aux)
        return (const XiFunc *) callee->aux;
    if (callee->op == XI_CONST && callee->type && callee->type->kind == XR_KIND_NULL &&
        current->name)
        return current;
    if (callee->op == XI_GET_SHARED) {
        return resolve_shared_function(bundle, current, (int) callee->aux_int);
    }
    if (callee->op == XI_IMPORT_REF && callee->aux)
        return resolve_import_ref(bundle, (const XiImportRef *) callee->aux);
    return NULL;
}
