/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_value_query.c - Backend-neutral IR value/type classification predicates
 */

#include "xi_value_query.h"
#include "xi_analysis.h"
#include "xi_module.h"
#include "xi_range.h"
#include "xi_receiver_alias.h"
#include "../runtime/value/xtype.h"
#include "../runtime/class/xclass_info.h"
#include "../stdlib/xstdlib_metadata.h"
#include "../plan/semantic/xr_semantic_builder.h"
#include "../plan/semantic/xr_semantic_class_shape.h"
#include <string.h>

XR_FUNC bool xi_type_is_channel(const XrType *type) {
    if (!type)
        return false;
    if (type->kind == XR_KIND_CHANNEL)
        return true;
    if (type->kind == XR_KIND_UNION) {
        for (uint8_t i = 0; i < type->union_type.member_count; i++) {
            if (xi_type_is_channel(type->union_type.members[i]))
                return true;
        }
    }
    return false;
}

XR_FUNC bool xi_value_imported_constructor_authority(
    const XiFunc *caller, const XiValue *call, XiImportedConstructorAuthority *authority) {
    if (!caller || !call || !authority || !xi_value_is_constructor_call(call) ||
        call->nargs == 0 || !call->args || !call->type ||
        call->type->kind != XR_KIND_INSTANCE || !call->type->instance.class_ref ||
        call->type->instance.class_ref->xg_class_id == 0 || call->type->is_nullable ||
        call->type->is_const || call->type->is_value_type || call->type->is_literal)
        return false;
    bool plain = call->op == XI_CALL && !call->aux && call->aux_int == 0;
    bool member = call->op == XI_CALL_METHOD && call->aux && (call->aux_int & 1) == 0 &&
                  strcmp((const char *) call->aux, "constructor") == 0;
    if (!plain && !member)
        return false;
    const XiImportRef *ref = xi_value_import_ref(caller, call->args[0]);
    const XiModule *module = ref ? ref->resolved_module : NULL;
    if (!ref || !ref->resolution_attempted || !ref->member_name || !ref->member_name[0] ||
        ref->resolved_func || !module || !module->identity || !module->identity[0] ||
        !module->init || !module->slot_classes || !module->classes || !module->exports ||
        ref->resolved_shared_slot < 0 || ref->resolved_shared_slot >= module->nslots ||
        ref->resolved_export_slot < 0 || ref->resolved_export_slot >= module->nexports)
        return false;
    const XiClassData *data = module->slot_classes[ref->resolved_shared_slot];
    const XiModuleExport *exported = &module->exports[ref->resolved_export_slot];
    if (!data || !data->needs_runtime_type ||
        data->xg_class_id != call->type->instance.class_ref->xg_class_id ||
        exported->class_data != data || exported->function ||
        exported->shared_slot != ref->resolved_shared_slot || !exported->name ||
        strcmp(exported->name, ref->member_name) != 0)
        return false;
    for (uint16_t i = 0; i < module->nexports; i++)
        if (i != (uint16_t) ref->resolved_export_slot && module->exports[i].name &&
            strcmp(module->exports[i].name, exported->name) == 0)
            return false;
    uint32_t source_class = XR_SEMANTIC_INDEX_NONE;
    for (uint16_t i = 0; i < module->nclasses; i++) {
        if (module->classes[i] != data)
            continue;
        if (source_class != XR_SEMANTIC_INDEX_NONE)
            return false;
        source_class = i;
    }
    const XrSemanticPlan *plan = module->init->semantic_plan;
    if (!plan || !xr_semantic_plan_is_verified(plan) ||
        xr_semantic_plan_source_class_count(plan) != module->nclasses)
        return false;
    const XrSemanticSourceClassRecord *klass = xr_semantic_plan_source_class(plan, source_class);
    if (!klass || klass->ordinal != source_class || !klass->name || !data->class_name ||
        strcmp(klass->name, data->class_name) != 0)
        return false;
    uint32_t source_export = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < xr_semantic_plan_source_export_count(plan); i++) {
        const XrSemanticSourceExportRecord *row = xr_semantic_plan_source_export(plan, i);
        if (!row || !row->name || strcmp(row->name, exported->name) != 0)
            continue;
        if (source_export != XR_SEMANTIC_INDEX_NONE ||
            row->shared_slot != exported->shared_slot ||
            xr_semantic_source_class_export_source_class(plan, row) != source_class ||
            !xr_stable_id_equal(row->exported_entity, klass->id))
            return false;
        source_export = i;
    }
    if (source_export == XR_SEMANTIC_INDEX_NONE)
        return false;
    uint32_t constructor = xr_semantic_class_constructor_function(plan, source_class);
    uint16_t member_index = UINT16_MAX;
    for (uint16_t i = 0; i < data->nmethod; i++) {
        if (!data->methods || !data->methods[i].is_constructor || data->methods[i].is_static)
            continue;
        if (member_index != UINT16_MAX)
            return false;
        member_index = i;
    }
    const XrSemanticFunctionRecord *function = xr_semantic_plan_function(plan, constructor);
    if (!function) {
        if (member_index != UINT16_MAX || call->nargs != 1)
            return false;
    } else if (member_index == UINT16_MAX || function->source_member_ordinal != member_index ||
               !xr_semantic_class_constructor_arity_is_exact(plan, constructor, function,
                                                             (uint16_t) (call->nargs - 1u)) ||
               xr_semantic_class_constructor_receiver_source_class(
                   plan, function->parameter_begin) != source_class) {
        return false;
    }
    *authority = (XiImportedConstructorAuthority) {module, data, plan, source_class,
                                                  source_export, constructor};
    return true;
}

XR_FUNC int xi_value_imported_constructor_operand_borrowed(
    const XiFunc *caller, const XiValue *call, uint16_t operand) {
    XiImportedConstructorAuthority authority;
    if (!call || call->op != XI_CALL || operand == 0 || operand >= call->nargs ||
        !xi_value_imported_constructor_authority(caller, call, &authority))
        return -1;
    const XrSemanticFunctionRecord *function =
        xr_semantic_plan_function(authority.plan, authority.constructor);
    XiModule *modules[] = {(XiModule *) authority.module};
    if (!function || !xr_semantic_source_type_admits_parameter(
                         caller, call->type, authority.plan, function->parameter_begin, modules, 1))
        return -1;
    for (uint16_t i = 1; i < call->nargs; i++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(authority.plan, function->parameter_begin + i);
        if (!parameter || parameter->function != authority.constructor || parameter->ordinal != i ||
            parameter->mode != XR_PARAM_READ || parameter->reserved != 0 ||
            (parameter->ownership != XI_OWN_NONE && parameter->ownership != XI_OWN_OWNED &&
             parameter->ownership != XI_OWN_BORROWED) ||
            !call->args[i] || !xr_semantic_source_constructor_argument_admits(
                                 caller, call->args[i], authority.plan,
                                 function->parameter_begin + i, modules, 1))
            return -1;
    }
    const XrSemanticParameterRecord *parameter =
        xr_semantic_plan_parameter(authority.plan, function->parameter_begin + operand);
    return parameter->ownership == XI_OWN_BORROWED ? 1 : 0;
}

XR_FUNC bool xi_type_is_named_instance(const XrType *type, const char *name) {
    if (!type || !name)
        return false;
    if (type->kind == XR_KIND_INSTANCE)
        return type->instance.class_name && strcmp(type->instance.class_name, name) == 0;
    if (type->kind == XR_KIND_CLASS && strcmp(name, "Atomic") == 0)
        return type->instance.class_name && strcmp(type->instance.class_name, name) == 0;
    if (type->kind == XR_KIND_UNION) {
        for (uint8_t i = 0; i < type->union_type.member_count; i++) {
            if (xi_type_is_named_instance(type->union_type.members[i], name))
                return true;
        }
    }
    return false;
}

XR_FUNC bool xi_type_is_task(const XrType *type) {
    return xi_type_is_named_instance(type, "Task");
}

XR_FUNC bool xi_type_is_thread(const XrType *type) {
    return xi_type_is_named_instance(type, "Thread");
}

/* Strip BOX/UNBOX/COPY identity wrappers so the test sees the carried type. */
static const XiValue *xi_value_unwrap_identity(const XiValue *v) {
    while (v &&
           (v->op == XI_BOX || v->op == XI_UNBOX || xi_copy_is_identity_alias(v) ||
            xi_op_is_identity_forward(v->op)) &&
           v->nargs >= 1)
        v = v->args[0];
    return v;
}

static const XiImportRef *xi_shared_slot_import_ref_in_func(const XiFunc *func, int slot) {
    if (!func || slot < 0)
        return NULL;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *block = func->blocks[bi];
        if (!block)
            continue;
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            const XiValue *value = block->values[vi];
            if (!value || value->op != XI_SET_SHARED || (int) value->aux_int != slot ||
                value->nargs < 1)
                continue;
            const XiValue *source = xi_value_unwrap_identity(value->args[0]);
            if (source && source->op == XI_IMPORT_REF && source->aux)
                return (const XiImportRef *) source->aux;
        }
    }
    return NULL;
}

static const XiImportRef *xi_shared_slot_import_ref(const XiFunc *func, int slot) {
    if (!func || slot < 0)
        return NULL;
    if (func->module && slot < (int) func->module->nslots && func->module->slot_imports &&
        func->module->slot_imports[slot])
        return func->module->slot_imports[slot];
    for (const XiFunc *current = func; current; current = current->parent_func) {
        const XiImportRef *ref = xi_shared_slot_import_ref_in_func(current, slot);
        if (ref)
            return ref;
    }
    if (func->module && func->module->init && func->module->init != func)
        return xi_shared_slot_import_ref_in_func(func->module->init, slot);
    return NULL;
}

XR_FUNC const XiImportRef *xi_value_import_ref(const XiFunc *func, const XiValue *value) {
    value = xi_value_unwrap_identity(value);
    if (!value)
        return NULL;
    if (value->op == XI_IMPORT_REF && value->aux)
        return (const XiImportRef *) value->aux;
    if (value->op == XI_GET_SHARED)
        return xi_shared_slot_import_ref(func, (int) value->aux_int);
    return NULL;
}

static const XiModule *xi_value_owning_module(const XiFunc *function) {
    for (const XiFunc *owner = function; owner; owner = owner->parent_func)
        if (owner->module)
            return owner->module;
    return NULL;
}

static const XiValue *xi_value_method_receiver_identity(const XiValue *receiver) {
    while (receiver && receiver->nargs > 0 &&
           (xi_copy_is_identity_alias(receiver) || xi_call_result_aliases_receiver(receiver)))
        receiver = receiver->args[0];
    return receiver;
}

XR_FUNC XiFunc *xi_value_resolve_method_callee(const XiFunc *caller, const XiValue *call) {
    if (!caller || !call || (call->op != XI_CALL_METHOD && call->op != XI_CALL_METHOD_DIRECT) ||
        call->nargs < 1 || !call->args[0] || !call->aux)
        return NULL;

    const XiImportRef *namespace_ref = xi_value_import_ref(caller, call->args[0]);
    const char *member = (const char *) call->aux;
    if (namespace_ref && !namespace_ref->member_name && namespace_ref->resolved_module) {
        const XiModule *module = namespace_ref->resolved_module;
        for (uint16_t i = 0; i < module->nexports; i++) {
            const XiModuleExport *exported = &module->exports[i];
            if (exported->function && exported->name && strcmp(exported->name, member) == 0)
                return exported->function;
        }
    }

    const XiValue *receiver = xi_value_method_receiver_identity(call->args[0]);
    const XiModule *module = xi_value_owning_module(caller);
    const XiClassData *selected = NULL;
    bool expect_static = false;

    /* Generic method bodies carry an erased receiver type. Parameter zero is
     * still bound to exactly one class-member row in this module, so recover
     * that frozen declaration before consulting the type. */
    if (module && module->init && caller->params && caller->nparams > 0 &&
        receiver == caller->params[0]) {
        for (uint16_t ci = 0; ci < module->nclasses && !selected; ci++) {
            const XiClassData *candidate = module->classes[ci];
            for (uint16_t mi = 0;
                 candidate && candidate->methods && candidate->child_idx && mi < candidate->nmethod;
                 mi++) {
                uint16_t child = candidate->child_idx[mi];
                if (child < module->init->nchildren && module->init->children[child] == caller) {
                    selected = candidate;
                    break;
                }
            }
        }
    }

    if (!selected && receiver->op == XI_GET_SHARED && receiver->aux_int >= 0 && module &&
        module->slot_classes && receiver->aux_int < module->nslots) {
        selected = module->slot_classes[receiver->aux_int];
        expect_static = selected != NULL;
    } else {
        const XiImportRef *ref = xi_value_import_ref(caller, receiver);
        if (ref && ref->resolved_module && ref->resolved_shared_slot >= 0 &&
            ref->resolved_module->slot_classes &&
            ref->resolved_shared_slot < ref->resolved_module->nslots) {
            module = ref->resolved_module;
            selected = module->slot_classes[ref->resolved_shared_slot];
            expect_static = selected != NULL;
        }
    }

    const XrType *receiver_type = receiver->type;
    if (!selected && receiver_type &&
        (receiver_type->kind == XR_KIND_INSTANCE || receiver_type->kind == XR_KIND_CLASS) &&
        receiver_type->instance.class_ref) {
        expect_static = receiver_type->kind == XR_KIND_CLASS;
        for (uint16_t ci = 0; module && ci < module->nclasses; ci++) {
            const XiClassData *candidate = module->classes[ci];
            if (candidate && candidate->class_info == receiver_type->instance.class_ref) {
                selected = candidate;
                break;
            }
        }
    }
    if (!selected || !module || !selected->methods || !selected->child_idx)
        return NULL;
    for (uint16_t mi = 0; mi < selected->nmethod; mi++) {
        const XiClassMethod *method = &selected->methods[mi];
        if (method->is_static != expect_static || !method->name ||
            strcmp(method->name, member) != 0)
            continue;
        uint16_t child = selected->child_idx[mi];
        if (module->init && child < module->init->nchildren)
            return module->init->children[child];
    }
    return NULL;
}

XR_FUNC bool xi_import_ref_is_grounded_native(const XiImportRef *ref) {
    return ref && ref->module_path && ref->module_path[0] && ref->module_path[0] != '.' &&
           ref->resolution_attempted && ref->resolved_mod_index == -1 &&
           ref->resolved_shared_slot == -1 && ref->resolved_export_slot == -1 &&
           !ref->resolved_func && !ref->resolved_module;
}

XR_FUNC bool xi_import_ref_is_source_module(const XiImportRef *ref) {
    return ref && ref->module_path && ref->module_path[0] && ref->resolved_mod_index >= 0 &&
           ref->resolved_module != NULL;
}

XR_FUNC bool xi_import_ref_is_native_stdlib(const XiImportRef *ref) {
    return xi_import_ref_is_grounded_native(ref) &&
           xr_stdlib_metadata_module_known(ref->module_path);
}

XR_FUNC bool xi_import_ref_is_unresolved(const XiImportRef *ref) {
    return ref && ref->module_path && ref->module_path[0] && !xi_import_ref_is_source_module(ref) &&
           !xi_import_ref_is_native_stdlib(ref);
}

XR_FUNC bool xi_value_type_is_channel(const XiValue *v) {
    v = xi_value_unwrap_identity(v);
    return v && xi_type_is_channel(v->type);
}

XR_FUNC bool xi_value_type_is_task(const XiValue *v) {
    v = xi_value_unwrap_identity(v);
    return v && xi_type_is_task(v->type);
}

XR_FUNC bool xi_value_type_is_thread(const XiValue *v) {
    v = xi_value_unwrap_identity(v);
    return v && xi_type_is_thread(v->type);
}

XR_FUNC bool xi_value_type_is_atomic(const XiValue *v) {
    v = xi_value_unwrap_identity(v);
    return v && xi_type_is_named_instance(v->type, "Atomic");
}

XR_FUNC bool xi_value_type_is_unknown(const XiValue *v) {
    v = xi_value_unwrap_identity(v);
    return !v || !v->type || XR_TYPE_IS_UNKNOWN_OR_ERROR(v->type);
}

static bool xi_value_const_int(const XiValue *value, int64_t *out) {
    const XiValue *v = xi_value_unwrap_identity(value);
    if (!v || v->op != XI_CONST || !v->type || v->type->kind != XR_KIND_INT || !out)
        return false;
    *out = v->aux_int;
    return true;
}

static uint16_t xi_negated_cmp_op(uint16_t op) {
    switch ((XiOp) op) {
        case XI_EQ:
            return XI_NE;
        case XI_NE:
            return XI_EQ;
        case XI_LT:
            return XI_GE;
        case XI_LE:
            return XI_GT;
        case XI_GT:
            return XI_LE;
        case XI_GE:
            return XI_LT;
        default:
            return XI_OP_COUNT;
    }
}

static bool xi_same_int_value(const XiFunc *f, const XiValue *a, const XiValue *b) {
    a = xi_value_unwrap_identity(a);
    b = xi_value_unwrap_identity(b);
    if (!a || !b || !a->type || !b->type || a->type->kind != XR_KIND_INT ||
        b->type->kind != XR_KIND_INT)
        return false;
    if (a == b)
        return true;
    if (f && a->op == XI_LOAD_UPVAL && b->op == XI_LOAD_UPVAL && a->aux_int == b->aux_int &&
        a->aux_int >= 0 && a->aux_int < f->ncaptures) {
        const XiCapture *cap = &f->captures[a->aux_int];
        return cap && !cap->needs_cell && cap->capture_kind != XI_CAPTURE_BY_MUT_CELL &&
               cap->capture_kind != XI_CAPTURE_SHARED;
    }
    return false;
}

static bool xi_cmp_implies_value_positive(const XiFunc *f, const XiValue *cond,
                                          const XiValue *value, bool truth) {
    cond = xi_value_unwrap_identity(cond);
    value = xi_value_unwrap_identity(value);
    if (!cond || !value || !value->type || value->type->kind != XR_KIND_INT)
        return false;
    if (cond->type && cond->type->kind == XR_KIND_BOOL) {
        if (cond->op == XI_NOT && cond->nargs >= 1)
            return xi_cmp_implies_value_positive(f, cond->args[0], value, !truth);
        if (((truth && cond->op == XI_BAND) || (!truth && cond->op == XI_BOR)) &&
            cond->nargs >= 2) {
            return xi_cmp_implies_value_positive(f, cond->args[0], value, truth) ||
                   xi_cmp_implies_value_positive(f, cond->args[1], value, truth);
        }
    }
    if (cond->nargs < 2)
        return false;

    uint16_t op = truth ? cond->op : xi_negated_cmp_op(cond->op);
    if (op == XI_OP_COUNT)
        return false;

    int64_t c = 0;
    if (xi_same_int_value(f, cond->args[0], value) && xi_value_const_int(cond->args[1], &c)) {
        switch ((XiOp) op) {
            case XI_GT:
                return c >= 0;
            case XI_GE:
            case XI_EQ:
                return c >= 1;
            default:
                return false;
        }
    }
    if (xi_value_const_int(cond->args[0], &c) && xi_same_int_value(f, cond->args[1], value)) {
        switch ((XiOp) op) {
            case XI_LT:
                return c >= 0;
            case XI_LE:
            case XI_EQ:
                return c >= 1;
            default:
                return false;
        }
    }
    return false;
}

static bool xi_cmp_implies_value_nonnegative(const XiFunc *f, const XiValue *cond,
                                             const XiValue *value, bool truth) {
    cond = xi_value_unwrap_identity(cond);
    value = xi_value_unwrap_identity(value);
    if (!cond || !value || !value->type || value->type->kind != XR_KIND_INT)
        return false;
    if (cond->type && cond->type->kind == XR_KIND_BOOL) {
        if (cond->op == XI_NOT && cond->nargs >= 1)
            return xi_cmp_implies_value_nonnegative(f, cond->args[0], value, !truth);
        if (((truth && cond->op == XI_BAND) || (!truth && cond->op == XI_BOR)) &&
            cond->nargs >= 2) {
            return xi_cmp_implies_value_nonnegative(f, cond->args[0], value, truth) ||
                   xi_cmp_implies_value_nonnegative(f, cond->args[1], value, truth);
        }
    }
    if (cond->nargs < 2)
        return false;

    uint16_t op = truth ? cond->op : xi_negated_cmp_op(cond->op);
    if (op == XI_OP_COUNT)
        return false;

    int64_t c = 0;
    if (xi_same_int_value(f, cond->args[0], value) && xi_value_const_int(cond->args[1], &c)) {
        switch ((XiOp) op) {
            case XI_GT:
                return c >= -1;
            case XI_GE:
            case XI_EQ:
                return c >= 0;
            default:
                return false;
        }
    }
    if (xi_value_const_int(cond->args[0], &c) && xi_same_int_value(f, cond->args[1], value)) {
        switch ((XiOp) op) {
            case XI_LT:
                return c >= -1;
            case XI_LE:
            case XI_EQ:
                return c >= 0;
            default:
                return false;
        }
    }
    return false;
}

static bool xi_cmp_implies_value_ge(const XiFunc *f, const XiValue *cond, const XiValue *value,
                                    bool truth, int64_t lower_bound) {
    cond = xi_value_unwrap_identity(cond);
    value = xi_value_unwrap_identity(value);
    if (!cond || !value || !value->type || value->type->kind != XR_KIND_INT)
        return false;
    if (cond->type && cond->type->kind == XR_KIND_BOOL) {
        if (cond->op == XI_NOT && cond->nargs >= 1)
            return xi_cmp_implies_value_ge(f, cond->args[0], value, !truth, lower_bound);
        if (((truth && cond->op == XI_BAND) || (!truth && cond->op == XI_BOR)) &&
            cond->nargs >= 2) {
            return xi_cmp_implies_value_ge(f, cond->args[0], value, truth, lower_bound) ||
                   xi_cmp_implies_value_ge(f, cond->args[1], value, truth, lower_bound);
        }
    }
    if (cond->nargs < 2)
        return false;

    uint16_t op = truth ? cond->op : xi_negated_cmp_op(cond->op);
    if (op == XI_OP_COUNT)
        return false;

    int64_t c = 0;
    if (xi_same_int_value(f, cond->args[0], value) && xi_value_const_int(cond->args[1], &c)) {
        switch ((XiOp) op) {
            case XI_GT:
                return lower_bound == INT64_MIN || c >= lower_bound - 1;
            case XI_GE:
            case XI_EQ:
                return c >= lower_bound;
            default:
                return false;
        }
    }
    if (xi_value_const_int(cond->args[0], &c) && xi_same_int_value(f, cond->args[1], value)) {
        switch ((XiOp) op) {
            case XI_LT:
                return lower_bound == INT64_MIN || c >= lower_bound - 1;
            case XI_LE:
            case XI_EQ:
                return c >= lower_bound;
            default:
                return false;
        }
    }
    return false;
}

static int xi_if_truth_on_path_to_block(const XiBlock *guard, const XiBlock *site) {
    if (!guard || !site || guard == site || guard->kind != XI_BLOCK_IF || !guard->succs[0] ||
        !guard->succs[1])
        return -1;
    bool true_path = xi_dominates(guard->succs[0], site);
    bool false_path = xi_dominates(guard->succs[1], site);
    if (true_path == false_path)
        return -1;
    return true_path ? 1 : 0;
}

static bool xi_value_has_positive_dominating_guard(const XiFunc *f, const XiValue *value,
                                                   const XiBlock *site) {
    if (!f || !value || !site)
        return false;
    xi_ensure_dominators((XiFunc *) f);
    for (const XiBlock *guard = site->idom; guard; guard = guard->idom) {
        int truth = xi_if_truth_on_path_to_block(guard, site);
        if (truth >= 0 && xi_cmp_implies_value_positive(f, guard->control, value, truth != 0))
            return true;
    }
    return false;
}

static bool xi_value_has_nonnegative_dominating_guard(const XiFunc *f, const XiValue *value,
                                                      const XiBlock *site) {
    if (!f || !value || !site)
        return false;
    xi_ensure_dominators((XiFunc *) f);
    for (const XiBlock *guard = site->idom; guard; guard = guard->idom) {
        int truth = xi_if_truth_on_path_to_block(guard, site);
        if (truth >= 0 && xi_cmp_implies_value_nonnegative(f, guard->control, value, truth != 0))
            return true;
    }
    return false;
}

static bool xi_value_has_ge_dominating_guard(const XiFunc *f, const XiValue *value,
                                             const XiBlock *site, int64_t lower_bound) {
    if (!f || !value || !site)
        return false;
    xi_ensure_dominators((XiFunc *) f);
    for (const XiBlock *guard = site->idom; guard; guard = guard->idom) {
        int truth = xi_if_truth_on_path_to_block(guard, site);
        if (truth >= 0 &&
            xi_cmp_implies_value_ge(f, guard->control, value, truth != 0, lower_bound))
            return true;
    }
    return false;
}

XR_FUNC bool xi_value_known_positive_at(const XiFunc *f, const XiValue *value,
                                        const XiBlock *site) {
    const XiValue *v = xi_value_unwrap_identity(value);
    if (!v || !v->type || v->type->kind != XR_KIND_INT)
        return false;

    int64_t c = 0;
    if (xi_value_const_int(v, &c))
        return c > 0;
    if (xi_range_known_positive(xi_range_of(v)))
        return true;
    return xi_value_has_positive_dominating_guard(f, v, site);
}

static bool xi_value_is_unsigned_i64_safe_width(const XiValue *v) {
    if (!v || !v->type || v->type->kind != XR_KIND_INT)
        return false;

    if (v->type->scalar_rep == XR_NATIVE_U8 || v->type->scalar_rep == XR_NATIVE_U16 ||
        v->type->scalar_rep == XR_NATIVE_U32)
        return true;

    switch ((XiOp) v->op) {
        case XI_NARROW_U8:
        case XI_NARROW_U16:
        case XI_NARROW_U32:
        case XI_WIDEN_U8:
        case XI_WIDEN_U16:
        case XI_WIDEN_U32:
            return true;
        default:
            return false;
    }
}

XR_FUNC bool xi_value_known_nonnegative_at(const XiFunc *f, const XiValue *value,
                                           const XiBlock *site) {
    const XiValue *v = xi_value_unwrap_identity(value);
    if (!v || !v->type || v->type->kind != XR_KIND_INT)
        return false;

    int64_t c = 0;
    if (xi_value_const_int(v, &c))
        return c >= 0;
    if (xi_value_is_unsigned_i64_safe_width(v))
        return true;
    if (v->op == XI_CONVERT && v->nargs >= 1 && v->args[0] &&
        xi_value_known_nonnegative_at(f, v->args[0], site))
        return true;
    if (xi_range_known_nonneg(xi_range_of(v)))
        return true;
    return xi_value_has_nonnegative_dominating_guard(f, v, site);
}

XR_FUNC bool xi_value_known_ge_at(const XiFunc *f, const XiValue *value, const XiBlock *site,
                                  int64_t lower_bound) {
    const XiValue *v = xi_value_unwrap_identity(value);
    if (!v || !v->type || v->type->kind != XR_KIND_INT)
        return false;

    int64_t c = 0;
    if (xi_value_const_int(v, &c))
        return c >= lower_bound;
    if (lower_bound <= 0 && xi_value_is_unsigned_i64_safe_width(v))
        return true;
    if (v->op == XI_CONVERT && v->nargs >= 1 && v->args[0] &&
        xi_value_known_ge_at(f, v->args[0], site, lower_bound))
        return true;
    if (xi_range_known_ge(xi_range_of(v), lower_bound))
        return true;
    return xi_value_has_ge_dominating_guard(f, v, site, lower_bound);
}

XR_FUNC const struct XiClassData *xi_value_class_constructor_call(const XiFunc *func,
                                                                  const XiValue *call,
                                                                  const XiFunc **out_constructor) {
    if (out_constructor)
        *out_constructor = NULL;
    if (!func || !call || call->op != XI_CALL || call->nargs < 1 ||
        !xi_value_is_constructor_call(call))
        return NULL;
    const XiValue *callee = call->args[0];
    while (callee && callee->nargs > 0 &&
           (xi_copy_is_identity_alias(callee) || xi_op_is_identity_forward(callee->op)))
        callee = callee->args[0];
    const XiModule *module = NULL;
    for (const XiFunc *owner = func; owner; owner = owner->parent_func) {
        if (owner->module) {
            module = owner->module;
            break;
        }
    }
    if (!callee || callee->op != XI_GET_SHARED || callee->aux_int < 0 || !module ||
        !module->slot_classes || callee->aux_int >= module->nslots)
        return NULL;
    const XiClassData *class_data = module->slot_classes[callee->aux_int];
    if (!class_data)
        return NULL;
    for (uint16_t i = 0; class_data->methods && i < class_data->nmethod; i++) {
        if (!class_data->methods[i].is_constructor || class_data->methods[i].is_static)
            continue;
        if (!out_constructor)
            return class_data;
        if (*out_constructor)
            return NULL; /* Two instance constructors name no single body. */
        uint16_t child = class_data->child_idx ? class_data->child_idx[i] : UINT16_MAX;
        if (!module->init || child >= module->init->nchildren || !module->init->children[child])
            return NULL;
        *out_constructor = module->init->children[child];
    }
    return class_data;
}
