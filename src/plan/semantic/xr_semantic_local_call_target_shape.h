/* Shared judgement: which SemanticPlan call targets name a function in this
 * module, and on which opcode each spelling is allowed to sit.
 *
 * Two layers need this same fact and had drifted apart: the TargetPlan verifier
 * built suspendability edges for both local kinds while the builder's adapter
 * family consumed only DIRECT_LOCAL, so a proven source-instance-method target
 * was refused as uncovered even though the verifier already treated it as an
 * edge. Both now ask here.
 *
 * A target that names a local function carries an edge from the call site to
 * the callee body; suspendability propagates backwards along those edges, so
 * missing one under-reports which functions may suspend.
 */
#ifndef XR_SEMANTIC_LOCAL_CALL_TARGET_SHAPE_H
#define XR_SEMANTIC_LOCAL_CALL_TARGET_SHAPE_H

#include "xr_semantic_plan.h"
#include "../../ir/xi.h"

/* The result side of an already-resolved local call has the same ABI whether
 * the source spelled a plain call, a tail call, or an instance-method call.
 * This helper deliberately answers only the opcode question: callers must
 * still prove the unique local target and pass its exact callee record before
 * using any result-family judgement. */
static inline bool
xr_semantic_local_call_result_opcode_is_exact(const XrSemanticOperationRecord *operation) {
    return operation && (operation->opcode == XI_CALL || operation->opcode == XI_TAIL_CALL ||
                         operation->opcode == XI_CALL_METHOD);
}

/* Whether the target names a function of this module and sits on the opcode
 * that spelling requires. DIRECT_LOCAL comes from a plain call whose callee is
 * an operand; SOURCE_INSTANCE_METHOD_LOCAL comes from a method call whose
 * receiver is operand 0. Pairing either kind with the other's opcode is not a
 * shape this plan produces, so the check stays exact rather than accepting the
 * union of both opcode sets. */
static inline bool
xr_semantic_call_target_names_local_function(const XrSemanticCallTargetRecord *target,
                                             const XrSemanticOperationRecord *operation,
                                             uint32_t function_count) {
    if (!target || !operation || target->function >= function_count)
        return false;
    if (target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL)
        return operation->opcode == XI_CALL || operation->opcode == XI_TAIL_CALL;
    /* A sealed candidate has the same operand shape as a proven local binding --
     * receiver in operand 0, filling parameter 0. The two differ in whether the
     * binding is proven or owed, which is a question for the layer holding the
     * module graph, not for the shape. */
    if (target->kind == XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_LOCAL ||
        target->kind == XR_SEM_CALL_TARGET_SOURCE_TEMPLATE_METHOD_LOCAL ||
        target->kind == XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_SEALED_CANDIDATE)
        return operation->opcode == XI_CALL_METHOD;
    return false;
}

/* How many leading operands of a local call precede the parameters they fill.
 * A direct call spends operand 0 on the callee, which fills no parameter, so
 * its operands run one ahead of the parameter list. A method call spends
 * operand 0 on the receiver, which fills parameter 0, so the two lists line up
 * one to one. Every layer that relates an operand index to a parameter index
 * needs this same number. */
static inline uint32_t
xr_semantic_local_call_operand_shift(const XrSemanticCallTargetRecord *target) {
    return (target && (target->kind == XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_LOCAL ||
                       target->kind == XR_SEM_CALL_TARGET_SOURCE_TEMPLATE_METHOD_LOCAL ||
                       target->kind == XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_SEALED_CANDIDATE))
               ? 0u
               : 1u;
}

#endif /* XR_SEMANTIC_LOCAL_CALL_TARGET_SHAPE_H */
