/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_scalar_copy_shape.h - Exact scalar copy() builtin authority
 */

#ifndef XR_SEMANTIC_SCALAR_COPY_SHAPE_H
#define XR_SEMANTIC_SCALAR_COPY_SHAPE_H

#include <string.h>

#include "../../ir/xi.h"
#include "../../ir/xi_own.h"
#include "../../ir/xi_ops_gen.h"
#include "xr_semantic_plan.h"

/* `copy(x)` on a scalar.  The builtin exists so a program can name the value a
 * binding held at one moment rather than the binding itself -- a cleanup block
 * that must not see later writes copies first.  For a scalar that is the whole
 * story: the result is a second scalar of the same type, holding the bits the
 * argument held, owning nothing the argument owned.
 *
 * The result type and the argument type are one and the same plan record, which
 * is what makes this shape provable: a copy that widened, narrowed, or changed
 * kind would be some other operation wearing this selector.
 *
 * Reference-capable arguments are deliberately out of scope.  Copying a String
 * or an Array is a second allocation and a second owner, and neither the
 * ownership obligation nor the storage that carries it is stated here; that
 * copy is a different construct and needs its own authority. */
static inline bool xr_semantic_scalar_copy_is_exact(const XrSemanticPlan *plan,
                                                    const XrSemanticOperationRecord *operation,
                                                    uint32_t *argument_value) {
    uint32_t operand_count = 0, metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    if (!plan || !operation || !operands || !metadata || operation->opcode != XI_CALL_BUILTIN ||
        operation->operand_count != 1 || operation->operand_begin >= operand_count ||
        operation->metadata_count != 1 || operation->metadata_begin >= metadata_count ||
        strcmp(metadata[operation->metadata_begin], "copy") != 0 ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE || operation->semantic_immediate != 0 ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_CALL_BUILTIN) ||
        operation->flags != xi_generated_op_default_flags(XI_CALL_BUILTIN) ||
        operation->ownership_use != xi_generated_op_own_use(XI_CALL_BUILTIN) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->transfer_mode != XR_TRANSFER_SHARE ||
        operation->parameter_mode != XR_PARAM_READ ||
        operation->parameter_ownership != XI_OWN_NONE || operation->result_alias_operand != -1 ||
        operation->return_parameter != -1 || operation->result_value == XR_SEMANTIC_INDEX_NONE)
        return false;
    const XrSemanticOperandRecord *argument = &operands[operation->operand_begin];
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, operation->result_type);
    if (argument->role != XR_SEM_OPERAND_ARGUMENT || argument->parameter != 0 ||
        argument->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        argument->ownership_action != XR_SEM_OPERAND_BORROW ||
        argument->transfer_mode != XR_TRANSFER_SHARE || argument->access != XR_CALL_ARG_PLAIN ||
        argument->type != operation->result_type || !type ||
        (type->flags & (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_NULLABLE)) != 0 ||
        type->child_count != 0 || type->aggregate_extent != 0 || type->aggregate_align != 0)
        return false;
    if (argument_value)
        *argument_value = argument->value;
    return true;
}

#endif /* XR_SEMANTIC_SCALAR_COPY_SHAPE_H */
