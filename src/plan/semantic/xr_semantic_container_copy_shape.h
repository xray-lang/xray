/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_container_copy_shape.h - Exact container copy authority
 */

#ifndef XR_SEMANTIC_CONTAINER_COPY_SHAPE_H
#define XR_SEMANTIC_CONTAINER_COPY_SHAPE_H

#include <string.h>

#include "../../ir/xi.h"
#include "../../ir/xi_own.h"
#include "../../ir/xi_ops_gen.h"
#include "xr_semantic_array_element_storage_shape.h"
#include "xr_semantic_plan.h"

/* `copy(c)` on a container.  Unlike the scalar spelling -- where the result is a
 * second name for the same bits -- this one materialises: it allocates a fresh
 * Array and fills it from what the argument borrows, so the result is a new
 * ownership root while the argument stays a borrow.
 *
 * Two argument shapes reach it, and they differ only in what the argument is:
 *
 *   copy(Slice<T>) -> Array<T>    a borrowed window becomes an owned array
 *   copy(Array<T>) -> Array<T>    an owned array becomes a second owner
 *
 * The result is always an owned `Array<T>` whose element type is the argument's,
 * which is what makes the shape provable: a copy that changed element type, or
 * handed back a borrow, would be some other operation wearing this selector.
 *
 * The element must have a storage class this plan can name.  An element the
 * storage vocabulary has no word for -- a class instance, say -- would make the
 * fill a promise about layout that nothing here can keep, so it stays refused
 * rather than being answered approximately. */
static inline bool xr_semantic_container_copy_is_exact(const XrSemanticPlan *plan,
                                                       const XrSemanticOperationRecord *operation,
                                                       uint32_t *argument_value,
                                                       uint8_t *element_storage) {
    uint32_t operand_count = 0, metadata_count = 0, child_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
    if (!plan || !operation || !operands || !metadata || !children ||
        operation->opcode != XI_CALL_BUILTIN || operation->operand_count != 1 ||
        operation->operand_begin >= operand_count || operation->metadata_count != 1 ||
        operation->metadata_begin >= metadata_count ||
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
    const XrSemanticTypeRecord *result = xr_semantic_plan_type(plan, operation->result_type);
    const XrSemanticTypeRecord *source = xr_semantic_plan_type(plan, argument->type);
    if (argument->role != XR_SEM_OPERAND_ARGUMENT || argument->parameter != 0 ||
        argument->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        argument->ownership_action != XR_SEM_OPERAND_BORROW ||
        argument->transfer_mode != XR_TRANSFER_SHARE || argument->access != XR_CALL_ARG_PLAIN ||
        argument->origin != 0 || argument->lifetime != 0 || argument->escape != 0 || !result ||
        !source)
        return false;
    /* The result is the owned array; the argument is either that same array or a
     * borrowed window over the same elements. */
    if (result->kind != XR_KIND_ARRAY ||
        result->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) ||
        result->builtin_type != XR_TID_NULL || result->child_count != 1 ||
        result->aggregate_extent != 0 || result->aggregate_align != 0 ||
        result->scalar_rep != XR_SCALAR_REP_NONE || result->child_begin >= child_count)
        return false;
    bool source_is_result = argument->type == operation->result_type;
    bool source_is_window =
        source->kind == XR_KIND_SLICE &&
        source->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_BORROW_VIEW) &&
        source->builtin_type == XR_TID_NULL && source->child_count == 1 &&
        source->aggregate_extent == 0 && source->aggregate_align == 0 &&
        source->scalar_rep == XR_SCALAR_REP_NONE && source->child_begin < child_count &&
        children[source->child_begin] == children[result->child_begin];
    if (!source_is_result && !source_is_window)
        return false;
    /* The element storage is derived from the result's element rather than
     * recorded on the operation: a non-Array intrinsic that carried element
     * storage authority is refused by the semantic verifier, and deriving it
     * keeps this family out of that record entirely. */
    const XrSemanticTypeRecord *element =
        xr_semantic_plan_type(plan, children[result->child_begin]);
    uint8_t storage = xr_semantic_array_element_storage(element);
    if (storage == XR_ELEM_ANY)
        return false;
    if (argument_value)
        *argument_value = argument->value;
    if (element_storage)
        *element_storage = storage;
    return true;
}

#endif /* XR_SEMANTIC_CONTAINER_COPY_SHAPE_H */
