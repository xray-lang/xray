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
    if (target->kind == XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_LOCAL)
        return operation->opcode == XI_CALL_METHOD;
    return false;
}

#endif /* XR_SEMANTIC_LOCAL_CALL_TARGET_SHAPE_H */
