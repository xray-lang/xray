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

#include "xr_c_emission_schema.h"
#include "../../plan/semantic/xr_semantic_ids.h"

typedef struct XrTargetPlan XrTargetPlan;

typedef struct XrCEmissionPlan XrCEmissionPlan;

XR_FUNC bool xr_c_emission_plan_build(const XrTargetPlan *target_plan,
                                      XrFingerprint expected_profile_fingerprint,
                                      XrCEmissionPlan **out, char *error, size_t error_size);
XR_FUNC void xr_c_emission_plan_free(XrCEmissionPlan *plan);
XR_FUNC bool xr_c_emission_plan_is_verified(const XrCEmissionPlan *plan);
XR_FUNC bool xr_c_emission_plan_verify(const XrCEmissionPlan *plan, const XrTargetPlan *target_plan,
                                       XrFingerprint expected_profile_fingerprint, char *error,
                                       size_t error_size);
XR_FUNC uint32_t xr_c_emission_plan_value_count(const XrCEmissionPlan *plan);
XR_FUNC uint32_t xr_c_emission_plan_call_argument_count(const XrCEmissionPlan *plan);
XR_FUNC uint32_t xr_c_emission_plan_cleanup_count(const XrCEmissionPlan *plan);
XR_FUNC XrFingerprint xr_c_emission_plan_fingerprint(const XrCEmissionPlan *plan);
XR_FUNC XrFingerprint xr_c_emission_plan_target_fingerprint(const XrCEmissionPlan *plan);
XR_FUNC XrFingerprint xr_c_emission_plan_profile_fingerprint(const XrCEmissionPlan *plan);
XR_FUNC bool xr_c_emission_plan_value_view(const XrCEmissionPlan *plan, uint32_t semantic_value,
                                           XrCValueEmissionView *out, char *error,
                                           size_t error_size);
XR_FUNC uint32_t xr_c_emission_plan_function_abi_count(const XrCEmissionPlan *plan);
XR_FUNC bool xr_c_emission_plan_function_abi_view(const XrCEmissionPlan *plan,
                                                  uint32_t semantic_function, uint16_t ordinal,
                                                  XrCFunctionAbiEmissionView *out, char *error,
                                                  size_t error_size);
bool xr_c_emission_plan_call_argument_view(const XrCEmissionPlan *plan,
                                           uint32_t semantic_call_value, uint16_t ordinal,
                                           XrCCallArgumentEmissionView *out, char *error,
                                           size_t error_size);
XR_FUNC bool xr_c_emission_plan_cleanup_view(const XrCEmissionPlan *plan,
                                             uint32_t semantic_operation,
                                             XrCCleanupEmissionView *out, char *error,
                                             size_t error_size);

#endif  // XR_C_EMISSION_PLAN_H
