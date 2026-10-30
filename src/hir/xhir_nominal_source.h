/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Source function producers register their declaration once, before binding. */
#ifndef XHIR_NOMINAL_SOURCE_H
#define XHIR_NOMINAL_SOURCE_H
#include "xhir_nominal.h"
typedef struct XrHirNominalFunctionSource {uint32_t function,declaration;} XrHirNominalFunctionSource;
XR_FUNC XrXirStatus xr_hir_body_nominal_function_source(XrHirBody *,const XrHirNominalFunctionSource *,
    XrCompileRequestControl *,XrHirNominalFunction *);
/* A literal receiver has actual Checked metadata and no Source declarations.
 * This receives shapes only; it grants no default or helper permissions. */
XR_FUNC XrXirStatus xr_hir_body_nominal_checked(XrHirBody *,const XrXirModule *,XrCompileRequestControl *);
typedef struct XrHirNominalCallEmission {XrXirInstruction call;uint32_t anchor;} XrHirNominalCallEmission;
/* The mechanical producer records the actual source call coordinate. */
XR_FUNC XrXirStatus xr_hir_body_nominal_call_emitted(XrHirBody *,XrHirBodyUse,const XrHirNominalCallEmission *);
#endif // XHIR_NOMINAL_SOURCE_H
