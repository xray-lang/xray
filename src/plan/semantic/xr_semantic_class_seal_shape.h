/* Shared judgement: does a module graph prove that a class carries no override?
 *
 * A module binding a method on a non-final class states an obligation rather
 * than a conclusion -- the binding holds provided nothing in the final graph
 * subclasses it. A module can never check that: it sees what it depends on,
 * never what depends on it. Only a layer holding every plan in the graph can
 * discharge the obligation, and both the TargetPlan builder and its verifier
 * hold one, so both ask here rather than each writing the walk.
 *
 * The edges are names. Two classes sharing one name anywhere make the question
 * unanswerable, and a class named as some class's parent has an override site;
 * either way the answer is no, which costs only the virtual call the emitter
 * would have made anyway.
 */
#ifndef XR_SEMANTIC_CLASS_SEAL_SHAPE_H
#define XR_SEMANTIC_CLASS_SEAL_SHAPE_H

#include <string.h>
#include "xr_semantic_plan.h"

static inline bool xr_semantic_graph_seals_class(const XrSemanticPlan *primary,
                                                 const XrSemanticPlan *const *dependencies,
                                                 uint32_t dependency_count,
                                                 const char *class_name) {
    if (!primary || !class_name || !class_name[0])
        return false;
    uint32_t same_name = 0;
    for (uint32_t p = 0; p <= dependency_count; p++) {
        const XrSemanticPlan *plan =
            p == 0 ? primary : (dependencies ? dependencies[p - 1u] : NULL);
        if (!plan)
            continue;
        uint32_t count = (uint32_t) xr_semantic_plan_source_class_count(plan);
        for (uint32_t c = 0; c < count; c++) {
            const XrSemanticSourceClassRecord *record = xr_semantic_plan_source_class(plan, c);
            if (!record)
                continue;
            if (record->super_name && strcmp(record->super_name, class_name) == 0)
                return false;
            if (record->name && strcmp(record->name, class_name) == 0)
                same_name++;
        }
    }
    return same_name == 1;
}

/* Whether a call target binds a method body in this graph. A final class states
 * the binding outright; a sealed candidate states it under the obligation the
 * walk above discharges. Both then travel the same adapter, collector and
 * boundary, so every layer consuming one consumes the other on these terms.
 * A template-local self call is already bound by its enclosing source-method
 * row rather than by a frozen class object, so it also answers true without a
 * graph sealing walk. */
static inline bool xr_semantic_call_target_binds_instance_method(
    const XrSemanticCallTargetRecord *target, const XrSemanticPlan *primary,
    const XrSemanticPlan *const *dependencies, uint32_t dependency_count) {
    if (!target)
        return false;
    if (target->kind == XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_LOCAL ||
        target->kind == XR_SEM_CALL_TARGET_SOURCE_TEMPLATE_METHOD_LOCAL)
        return true;
    if (target->kind != XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_SEALED_CANDIDATE)
        return false;
    const XrSemanticTypeRecord *receiver_type =
        primary ? xr_semantic_plan_type(primary, target->callable_type) : NULL;
    const XrSemanticSourceClassRecord *source_class =
        receiver_type && receiver_type->source_class != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_source_class(primary, receiver_type->source_class)
            : NULL;
    return source_class && xr_semantic_graph_seals_class(primary, dependencies, dependency_count,
                                                         source_class->name);
}

#endif /* XR_SEMANTIC_CLASS_SEAL_SHAPE_H */
