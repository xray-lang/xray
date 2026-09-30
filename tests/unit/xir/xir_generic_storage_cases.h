/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generic_storage_cases.h - Intrinsic storage from authentic generic owners
 *
 * KEY CONCEPT:
 *   Intrinsic copy/save is independent of interface and Sendable obligations.
 */
#ifndef XIR_GENERIC_STORAGE_CASES_H
#define XIR_GENERIC_STORAGE_CASES_H
#include "xir/xxir_constraint_proof.h"
#include "xir/xxir_types.h"
static void generic_storage_cases(void) {
    XrXirConstraint facts[2]={{XR_XIR_CONSTRAINT_SENDABLE,NULL,0},{0}};
    XrXirConstraint requirements[2]={{0},{XR_XIR_CONSTRAINT_SENDABLE,NULL,0}};
    XrXirGeneric generic={0}; generic.constraints=facts; generic.parameter_count=2;
    XrXirFunction function={0};
    XrXirNominalDeclaration declarations[2]={0};
    declarations[0].constraints=facts; declarations[0].parameter_count=2;
    declarations[1].constraints=requirements; declarations[1].parameter_count=2;
    XrXirNominalTable table={declarations,2,NULL};
    XrXirType p0=XR_XIR_TYPE_PARAMETER_BASE,p1=XR_XIR_TYPE_PARAMETER_BASE+1;
    XrXirType arguments[]={p1,p0};
    XrXirTypeNode nodes[2]={0};
    nodes[0].kind=XR_XIR_TYPE_ARRAY; nodes[0].element=p1; nodes[0].parameter_span=2;
    nodes[1].kind=XR_XIR_TYPE_NOMINAL; nodes[1].parameter_span=2;
    nodes[1].nominal=(XrXirNominalType){1,arguments,2,NULL,0};
    XrXirTypes types={nodes,2,&table,NULL};
    XrXirModule module={0}; module.types=&types; module.functions=&function;
    module.function_count=1; module.generics=&generic;
    XrXirProofContext context={&module,{XR_XIR_CONTEXT_FUNCTION,0,0}};
    XrXirBudget budget=xr_xir_default_budget(); uint64_t scratch=budget.scratch_bytes;
    CHECK(xr_xir_type_storage_prove(&context,p1,&budget)==XR_XIR_OK);
    CHECK(budget.scratch_bytes==scratch);
    budget=xr_xir_default_budget();
    CHECK(xr_xir_type_markers_prove(&context,p1,XR_XIR_CONSTRAINT_SENDABLE,&budget)==XR_XIR_BAD_TYPE);
    budget=xr_xir_default_budget();
    CHECK(xr_xir_type_storage_prove(&context,XR_XIR_CONSTRUCTED_TYPE_BASE,&budget)==XR_XIR_OK);
    budget=xr_xir_default_budget();
    CHECK(xr_xir_type_storage_prove(&context,XR_XIR_CONSTRUCTED_TYPE_BASE+1,&budget)==XR_XIR_OK);
    arguments[0]=p0;arguments[1]=p1;budget=xr_xir_default_budget();
    CHECK(xr_xir_type_storage_prove(&context,XR_XIR_CONSTRUCTED_TYPE_BASE+1,&budget)==XR_XIR_BAD_TYPE);
    CHECK(budget.scratch_bytes==scratch);
    context.owner=(XrXirDeclarationContext){XR_XIR_CONTEXT_NOMINAL,0,0};
    budget=xr_xir_default_budget();
    CHECK(xr_xir_type_storage_prove(&context,p1,&budget)==XR_XIR_OK);
    context.owner.declaration=2;budget=xr_xir_default_budget();
    CHECK(xr_xir_type_storage_prove(&context,p1,&budget)==XR_XIR_BAD_STRUCTURE);
    context.owner=(XrXirDeclarationContext){XR_XIR_CONTEXT_CLOSED,0,0};
    budget=xr_xir_default_budget();
    CHECK(xr_xir_type_storage_prove(&context,p1,&budget)==XR_XIR_BAD_TYPE);
    context.owner=(XrXirDeclarationContext){XR_XIR_CONTEXT_FUNCTION,0,0};
    generic.parameter_count=1;budget=xr_xir_default_budget();
    CHECK(xr_xir_type_storage_prove(&context,p1,&budget)==XR_XIR_BAD_TYPE);
    generic.parameter_count=2;budget=xr_xir_default_budget();
    CHECK(xr_xir_type_storage_prove(&context,XR_XIR_UNIT,&budget)==XR_XIR_BAD_TYPE);
    CHECK(xr_xir_type_markers_prove(&context,XR_XIR_UNIT,XR_XIR_CONSTRAINT_SENDABLE,&budget)==XR_XIR_OK);
    CHECK(xr_xir_type_markers_prove(&context,XR_XIR_UNIT,XR_XIR_CONSTRAINT_ERROR,&budget)==XR_XIR_BAD_TYPE);
    budget=xr_xir_default_budget();budget.work=0;
    CHECK(xr_xir_type_storage_prove(&context,p1,&budget)==XR_XIR_BUDGET);
    budget=xr_xir_default_budget();budget.scratch_bytes=0;
    CHECK(xr_xir_type_storage_prove(&context,p1,&budget)==XR_XIR_OK);
    CHECK(xr_xir_type_storage_prove(&context,XR_XIR_CONSTRUCTED_TYPE_BASE,&budget)==XR_XIR_BUDGET);
}
#endif /* XIR_GENERIC_STORAGE_CASES_H */
