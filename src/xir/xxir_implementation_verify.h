/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_implementation_verify.h - Authenticated interface witness lookup
 *
 * KEY CONCEPT:
 *   Witness targets belong to the declaring module while substituted arguments
 *   borrow the independently verified receiver pool.
 */
#ifndef XXIR_IMPLEMENTATION_VERIFY_H
#define XXIR_IMPLEMENTATION_VERIFY_H
#include "xxir_constraint_proof.h"
#include "xxir_implementation.h"
typedef struct XrXirWitnessRequest {
    const XrXirModule *declaration_module;
    XrXirType receiver;
    XrXirInterfaceApplication application;
    uint32_t member;
} XrXirWitnessRequest;
typedef struct XrXirWitness {
    uint32_t function;
    const XrXirType *arguments;
    uint32_t argument_count;
} XrXirWitness;
/* Requires descriptor/declaration shape admission. Reads signatures and
 * declaration facts only; function body/effect validation remains mandatory. */
XR_FUNC XrXirStatus xr_xir_implementations_verify(const XrXirModule *module,
    XrXirBudget *budget);
/* Output arguments borrow the receiver pool; function belongs to the request's
 * declaration module. No scratch type IDs escape this operation. */
XR_FUNC XrXirStatus xr_xir_witness_resolve(const XrXirProofContext *context,
    const XrXirWitnessRequest *request, XrXirBudget *budget, XrXirWitness *output);
#endif // XXIR_IMPLEMENTATION_VERIFY_H
