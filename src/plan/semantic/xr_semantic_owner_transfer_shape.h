/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_owner_transfer_shape.h - Exact ARC owner transfer authority
 */

#ifndef XR_SEMANTIC_OWNER_TRANSFER_SHAPE_H
#define XR_SEMANTIC_OWNER_TRANSFER_SHAPE_H

#include "../../ir/xi.h"
#include "../../ir/xi_ops_gen.h"
#include "xr_semantic_plan.h"

/* Both ownership-transfer operations carry the source representation into a
 * fresh SSA value. OWNER_FORWARD is the ARC kernel spelling; SOURCE_MOVE is
 * the source-language move boundary. Their lifetime/provenance rows differ,
 * but neither changes the physical representation being transferred.
 *
 * This judgement deliberately owns only that common storage consequence. The
 * operation and operand shapes remain exact, so another consuming operation
 * cannot acquire representation authority merely because it has one operand.
 */
static inline const XrSemanticOperandRecord *xr_semantic_owner_transfer_base_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    if (!plan || !operation || !operands ||
        (operation->opcode != XI_OWNER_FORWARD && operation->opcode != XI_SOURCE_MOVE) ||
        operation->operand_count != 1 || operation->operand_begin >= operand_count ||
        operation->semantic_immediate != 0 || operation->allocation_key ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE || operation->metadata_count != 0 ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(operation->opcode) ||
        operation->flags != xi_generated_op_default_flags(operation->opcode) ||
        operation->ownership_use != xi_generated_op_own_use(operation->opcode) ||
        operation->result_ownership != xi_generated_op_result_ownership(operation->opcode) ||
        operation->transfer_mode != XR_TRANSFER_SHARE ||
        operation->parameter_mode != XR_PARAM_READ ||
        operation->parameter_ownership != XI_OWN_NONE || operation->return_parameter != -1 ||
        operation->view_source_value != XR_SEMANTIC_INDEX_NONE ||
        operation->view_element_type != XR_SEMANTIC_INDEX_NONE ||
        operation->view_source_operand != -1 || operation->view_source_parameter != -1 ||
        operation->intrinsic_kind != 0 || operation->view_origin != XI_VIEW_ORIGIN_NONE ||
        operation->view_capability != 0 || operation->view_lifetime != 0 ||
        operation->view_complete != 0 || operation->array_element_storage != 0 ||
        operation->reserved_view[0] != 0 || operation->reserved_view[1] != 0 ||
        operation->array_hof_kind != 0 || operation->array_result_element_storage != 0)
        return NULL;

    if (operation->opcode == XI_OWNER_FORWARD) {
        if (operation->result_alias_operand != -1 ||
            operation->return_provenance != XR_SEM_RETURN_OWNED || operation->return_complete != 1)
            return NULL;
    } else {
        /* SOURCE_MOVE preserves the source value's return provenance. A move
         * inside a function has no return provenance; moving a complete owned
         * call result preserves its complete-owned fact. Both are exact ARC
         * states, and neither changes the carried representation. */
        bool exact_return = (operation->return_provenance == XR_SEM_RETURN_NONE &&
                             operation->return_complete == 0) ||
                            (operation->return_provenance == XR_SEM_RETURN_OWNED &&
                             operation->return_complete == 1);
        if (operation->result_alias_operand != 0 || operation->evidence[0] != 0 ||
            operation->evidence[1] != 0 || operation->evidence[2] != 0 ||
            operation->evidence[3] != 0 || operation->evidence[4] != 0 ||
            operation->evidence[5] != 0 || operation->evidence[6] == 0 ||
            operation->evidence[7] != XR_SEMANTIC_INDEX_NONE || !exact_return)
            return NULL;
    }

    const XrSemanticOperandRecord *source = &operands[operation->operand_begin];
    if (source->value == XR_SEMANTIC_INDEX_NONE || source->role != XR_SEM_OPERAND_VALUE ||
        source->parameter != -1 ||
        source->transfer_mode != XR_TRANSFER_SHARE ||
        source->ownership_action != XR_SEM_OPERAND_CONSUME ||
        source->parameter_mode != XR_PARAM_READ || source->access != XR_CALL_ARG_PLAIN ||
        source->origin != XI_PLACE_ORIGIN_NONE || source->lifetime != XI_PLACE_LIFETIME_NONE ||
        source->escape != XI_PLACE_ESCAPE_NONE || source->flags != 0)
        return NULL;
    return source;
}

static inline bool xr_semantic_owner_transfer_is_exact(const XrSemanticPlan *plan,
                                                       const XrSemanticOperationRecord *operation,
                                                       uint32_t *source_value_out) {
    const XrSemanticOperandRecord *source =
        xr_semantic_owner_transfer_base_is_exact(plan, operation);
    if (!source || source->type != operation->result_type)
        return false;
    if (source_value_out)
        *source_value_out = source->value;
    return true;
}

#endif  // XR_SEMANTIC_OWNER_TRANSFER_SHAPE_H
