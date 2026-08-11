/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_c_emission_plan.h - Immutable TargetPlan-backed C emission plan
 *
 * KEY CONCEPT:
 *   C spelling is introduced once after a verified TargetPlan binding has
 *   selected the backend-neutral machine representation. Consumers query an
 *   immutable numeric/string projection and never revisit semantic types.
 */

#ifndef XR_C_EMISSION_PLAN_H
#define XR_C_EMISSION_PLAN_H

#include "../../plan/target/xr_target_plan.h"

typedef struct XrCEmissionPlan XrCEmissionPlan;

typedef enum XrCScalarRep {
    XR_C_SCALAR_REP_VOID = 0,
    XR_C_SCALAR_REP_I8,
    XR_C_SCALAR_REP_U8,
    XR_C_SCALAR_REP_I16,
    XR_C_SCALAR_REP_U16,
    XR_C_SCALAR_REP_I32,
    XR_C_SCALAR_REP_U32,
    XR_C_SCALAR_REP_I64,
    XR_C_SCALAR_REP_U64,
    XR_C_SCALAR_REP_ISIZE,
    XR_C_SCALAR_REP_USIZE,
    XR_C_SCALAR_REP_F32,
    XR_C_SCALAR_REP_F64,
    XR_C_SCALAR_REP_BOOL,
    XR_C_SCALAR_REP_RUNE,
    XR_C_SCALAR_REP_COUNT,
} XrCScalarRep;

typedef struct XrCScalarEmissionView {
    uint32_t semantic_value;
    uint16_t target_register_rep;
    uint16_t target_memory_rep;
    uint16_t target_register_kind;
    uint16_t target_memory_kind;
    uint16_t register_bits;
    uint16_t memory_align;
    uint32_t memory_size;
    uint8_t rep;
    const char *c_type;
} XrCScalarEmissionView;

XR_FUNC bool xr_c_emission_plan_build(const XrTargetPlan *target_plan,
                                      XrCEmissionPlan **out,
                                      char *error,
                                      size_t error_size);
XR_FUNC void xr_c_emission_plan_free(XrCEmissionPlan *plan);
XR_FUNC bool xr_c_emission_plan_is_verified(const XrCEmissionPlan *plan);
XR_FUNC uint32_t xr_c_emission_plan_scalar_count(const XrCEmissionPlan *plan);
XR_FUNC XrFingerprint xr_c_emission_plan_fingerprint(const XrCEmissionPlan *plan);
XR_FUNC XrFingerprint xr_c_emission_plan_target_fingerprint(const XrCEmissionPlan *plan);
XR_FUNC bool xr_c_emission_plan_scalar_view(const XrCEmissionPlan *plan,
                                            uint32_t semantic_value,
                                            XrCScalarEmissionView *out,
                                            char *error,
                                            size_t error_size);

#endif  // XR_C_EMISSION_PLAN_H
