/* Shared judgement: which machine representation a scalar semantic type takes.
 *
 * This mapping is one fact -- an i32 is an I32 rep, a rune is a RUNE rep, a
 * unit enum is its ordinal -- and it was written twice: once in the TargetPlan
 * builder to bind storage, once in its verifier to state what it expected. The
 * two drifted the moment either grew a case the other lacked, which is how a
 * unit enum came to resolve here on one side and fall through to "not a scalar"
 * on the other.
 *
 * What stays separate is the raw-pointer test. The verifier derives it by
 * re-parsing the frozen canonical key while the builder reads the record's
 * fields, and those are genuinely independent routes to the same answer -- the
 * kind of check that catches a record whose fields and key disagree. So the
 * caller passes its own verdict in rather than sharing one.
 */
#ifndef XR_TARGET_SCALAR_REP_SHAPE_H
#define XR_TARGET_SCALAR_REP_SHAPE_H

#include "../semantic/xr_semantic_plan.h"
#include "../semantic/xr_semantic_enum_shape.h"
#include "xr_target_plan.h"

typedef enum XrTargetScalarRepVerdict {
    /* The type is one this mapping names, but its own fields contradict it --
     * an int with no width, a bool carrying a scalar spelling. Refuse. */
    XR_TARGET_SCALAR_REP_INVALID = -1,
    /* Not a scalar at all. Some other family answers for it, and saying so is
     * not a refusal. */
    XR_TARGET_SCALAR_REP_NOT_APPLICABLE = 0,
    /* A scalar, and `out_kind` now names its machine representation. */
    XR_TARGET_SCALAR_REP_EXACT = 1,
} XrTargetScalarRepVerdict;

static inline XrTargetScalarRepVerdict
xr_target_scalar_rep_for_type(const XrSemanticTypeRecord *type, bool raw_pointer_is_exact,
                              uint16_t *out_kind) {
    if (!type || !out_kind)
        return XR_TARGET_SCALAR_REP_INVALID;
    /* A nullable payload lives in a tagged carrier, so the scalar mapping has
     * nothing to say about it; the nullable family owns that row. */
    if ((type->flags & XR_SEM_TYPE_NULLABLE) != 0)
        return XR_TARGET_SCALAR_REP_NOT_APPLICABLE;
    switch (type->kind) {
        case XR_KIND_INT:
            switch (type->scalar_rep) {
                case XR_NATIVE_I8:
                    *out_kind = XR_MACHINE_REP_I8;
                    break;
                case XR_NATIVE_U8:
                    *out_kind = XR_MACHINE_REP_U8;
                    break;
                case XR_NATIVE_I16:
                    *out_kind = XR_MACHINE_REP_I16;
                    break;
                case XR_NATIVE_U16:
                    *out_kind = XR_MACHINE_REP_U16;
                    break;
                case XR_NATIVE_I32:
                    *out_kind = XR_MACHINE_REP_I32;
                    break;
                case XR_NATIVE_U32:
                    *out_kind = XR_MACHINE_REP_U32;
                    break;
                case XR_NATIVE_I64:
                    *out_kind = XR_MACHINE_REP_I64;
                    break;
                case XR_NATIVE_U64:
                    *out_kind = XR_MACHINE_REP_U64;
                    break;
                case XR_NATIVE_ISIZE:
                    *out_kind = XR_MACHINE_REP_ISIZE;
                    break;
                case XR_NATIVE_USIZE:
                    *out_kind = XR_MACHINE_REP_USIZE;
                    break;
                default:
                    return XR_TARGET_SCALAR_REP_INVALID;
            }
            return XR_TARGET_SCALAR_REP_EXACT;
        case XR_KIND_FLOAT:
            if (type->scalar_rep == XR_NATIVE_F32)
                *out_kind = XR_MACHINE_REP_F32;
            else if (type->scalar_rep == XR_NATIVE_F64)
                *out_kind = XR_MACHINE_REP_F64;
            else
                return XR_TARGET_SCALAR_REP_INVALID;
            return XR_TARGET_SCALAR_REP_EXACT;
        case XR_KIND_BOOL:
            if (type->scalar_rep != XR_SCALAR_REP_NONE)
                return XR_TARGET_SCALAR_REP_INVALID;
            *out_kind = XR_MACHINE_REP_I1;
            return XR_TARGET_SCALAR_REP_EXACT;
        case XR_KIND_RUNE:
            if (type->scalar_rep != XR_SCALAR_REP_NONE)
                return XR_TARGET_SCALAR_REP_INVALID;
            *out_kind = XR_MACHINE_REP_RUNE;
            return XR_TARGET_SCALAR_REP_EXACT;
        case XR_KIND_UNIT:
        case XR_KIND_NEVER:
            if (type->scalar_rep != XR_SCALAR_REP_NONE)
                return XR_TARGET_SCALAR_REP_INVALID;
            *out_kind = XR_MACHINE_REP_VOID;
            return XR_TARGET_SCALAR_REP_EXACT;
        case XR_KIND_POINTER:
            if (!raw_pointer_is_exact)
                return XR_TARGET_SCALAR_REP_INVALID;
            *out_kind = XR_MACHINE_REP_RAW_PTR;
            return XR_TARGET_SCALAR_REP_EXACT;
        case XR_KIND_ENUM:
            /* A unit enum is the ordinal it carries -- no payload, no
             * allocation, nothing a tagged value would hold. An enum with
             * payloads is a different shape and answers through the families
             * that bind tagged values. */
            if (!xr_semantic_unit_enum_type_is_exact(type))
                return XR_TARGET_SCALAR_REP_NOT_APPLICABLE;
            *out_kind = XR_MACHINE_REP_ENUM_ORDINAL;
            return XR_TARGET_SCALAR_REP_EXACT;
        default:
            return XR_TARGET_SCALAR_REP_NOT_APPLICABLE;
    }
}

#endif /* XR_TARGET_SCALAR_REP_SHAPE_H */
