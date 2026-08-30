/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_leaf_value_product_program_emission.h - Xi-bound leaf product C emission
 */

#ifndef XR_LEAF_VALUE_PRODUCT_PROGRAM_EMISSION_H
#define XR_LEAF_VALUE_PRODUCT_PROGRAM_EMISSION_H

#include "../plan/target/xr_target_plan.h"

struct XiModule;

/* Validates and freezes the one PSC/Xi-to-TargetPlan join internally, then
 * emits solely from that opaque binding and schema-56 TargetPlan rows. No
 * mutable binding escapes this call. The output contains no test oracle or
 * process entry point and is released with xr_free(). */
XR_FUNC bool xr_c_leaf_value_product_program_emit(
    const XrTargetPlan *target_plan, const struct XiModule *module,
    char **out_source, size_t *out_size, char *error, size_t error_size);

#endif  // XR_LEAF_VALUE_PRODUCT_PROGRAM_EMISSION_H
