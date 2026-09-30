/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_implementation_semantic_cases.h - Conditional conformance and witness authority
 *
 * KEY CONCEPT:
 *   Independent negative cases distinguish explicit declaration evidence from
 *   matching method shapes and preserve allocation-failure ownership.
 */
#ifndef XIR_IMPLEMENTATION_SEMANTIC_CASES_H
#define XIR_IMPLEMENTATION_SEMANTIC_CASES_H
#include "xir/xxir_implementation_verify.h"
typedef struct ImplementationSemanticFixture {
    XrXirTypeNode nodes[4];
    XrXirTypes types;
    XrXirInterfaceMethod method;
    XrXirInterfaceDeclaration interface;
    XrXirInterfaceTable interfaces;
    XrXirInterfaceApplication application;
    XrXirConstraint bound;
    XrXirNominalDeclaration nominals[2];
    XrXirNominalTable nominal_table;
    XrXirType parameter, meter, box_parameter, box_meter;
    XrXirFunction functions[2];
    XrXirGeneric generics[2];
    XrXirFunctionIdentity identities[2];
    XrXirSourceModule source;
    XrXirImplementationBinding bindings[2];
    XrXirImplementation records[2];
    XrXirImplementationTable implementations;
    XrXirDeclarations declarations;
    XrXirModule module;
} ImplementationSemanticFixture;
static void implementation_semantic_fixture(ImplementationSemanticFixture *f) {
    memset(f,0,sizeof(*f));
    f->parameter = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    f->meter = member_case_type(1); f->box_parameter = member_case_type(2); f->box_meter = member_case_type(3);
    f->nodes[0].kind = XR_XIR_TYPE_CALLABLE; f->nodes[0].result = XR_XIR_I64;
    f->method = (XrXirInterfaceMethod){{"measure",7},member_case_type(0),0,0,NULL};
    f->interface = (XrXirInterfaceDeclaration){{"m",1},{"Measure",7},1,NULL,0,NULL,0,&f->method,1};
    f->interfaces = (XrXirInterfaceTable){&f->interface,1};
    f->application = (XrXirInterfaceApplication){0,NULL,0};
    f->bound = (XrXirConstraint){0,&f->application,1};
    for (uint32_t n = 0; n < 2; ++n) {
        f->nominals[n].module = (XrXirLiteral){"m",1}; f->nominals[n].exported = 1;
        f->nodes[n+1].kind = XR_XIR_TYPE_NOMINAL; f->nodes[n+1].nominal.declaration = n;
        f->functions[n].parameter_count = 1; f->functions[n].result = XR_XIR_I64;
        f->identities[n].exported = 1; f->identities[n].nominal_owner = n+1; f->identities[n].method_kind = XR_XIR_READ_METHOD;
        f->bindings[n] = (XrXirImplementationBinding){f->application,0,n};
        f->records[n] = (XrXirImplementation){n,f->application,&f->bindings[n],1};
    }
    f->nominals[0].name = (XrXirLiteral){"Meter",5};
    f->nominals[1].name = (XrXirLiteral){"Box",3}; f->nominals[1].constraints = &f->bound;
    f->nominals[1].parameter_count = 1;
    f->nodes[2].parameter_span = 1; f->nodes[2].nominal.arguments = &f->parameter;
    f->nodes[2].nominal.argument_count = 1;
    f->nodes[3].kind = XR_XIR_TYPE_NOMINAL; f->nodes[3].nominal.declaration = 1;
    f->nodes[3].nominal.arguments = &f->meter; f->nodes[3].nominal.argument_count = 1;
    f->nominal_table = (XrXirNominalTable){f->nominals,2,NULL};
    f->types = (XrXirTypes){f->nodes,4,&f->nominal_table,&f->interfaces};
    f->functions[0].parameters = &f->meter; f->functions[1].parameters = &f->box_parameter;
    f->generics[1].constraints = &f->bound; f->generics[1].parameter_count = 1;
    f->source.name = "m"; f->source.name_length = 1;
    f->implementations = (XrXirImplementationTable){f->records,2};
    f->declarations.modules = &f->source; f->declarations.module_count = 1;
    f->declarations.functions = f->identities; f->declarations.implementations = &f->implementations;
    f->module.stage = XR_XIR_BUILT; f->module.functions = f->functions; f->module.function_count = 2;
    f->module.generics = f->generics; f->module.types = &f->types; f->module.declarations = &f->declarations;
}
static XrXirStatus implementation_semantic_status(ImplementationSemanticFixture *f) {
    XrXirBudget budget = xr_xir_default_budget();
    uint64_t scratch = budget.scratch_bytes;
    XrXirStatus status = xr_xir_implementations_verify(&f->module,&budget);
    CHECK(budget.scratch_bytes==scratch && !live); return status;
}
static void implementation_semantic_bindings(void) {
    for (unsigned attack = 0; attack < 10; ++attack) {
        ImplementationSemanticFixture f; implementation_semantic_fixture(&f);
        if (attack==1) f.identities[0].method_kind = XR_XIR_STATIC_METHOD;
        if (attack==2) f.identities[0].nominal_owner = 2;
        if (attack==3) f.identities[0].member_access = XR_XIR_MEMBER_PRIVATE;
        if (attack==4) f.identities[0].cleanup_owner = 1;
        if (attack==5) f.functions[0].result = XR_XIR_BOOL;
        if (attack==6) f.bindings[0].member = 1;
        if (attack==7) f.bindings[0].function = 1;
        if (attack==8) f.records[0].bindings = NULL;
        if (attack==9) f.identities[0].exported = 0;
        XrXirStatus status = implementation_semantic_status(&f);
        CHECK(attack ? status!=XR_XIR_OK : status==XR_XIR_OK);
    }
    ImplementationSemanticFixture f; implementation_semantic_fixture(&f);
    f.nodes[0].flags = XR_XIR_CALLABLE_NO_SUSPEND;
    CHECK(implementation_semantic_status(&f)==XR_XIR_BAD_TYPE);
    f.identities[0].promises = f.identities[1].promises = XR_XIR_FUNCTION_NO_SUSPEND;
    CHECK(implementation_semantic_status(&f)==XR_XIR_OK);
    f.nodes[0].flags = 0; CHECK(implementation_semantic_status(&f)==XR_XIR_OK);
    XrXirConstraint stronger = f.bound; stronger.markers = XR_XIR_CONSTRAINT_SENDABLE;
    f.generics[1].constraints = &stronger;
    CHECK(implementation_semantic_status(&f)==XR_XIR_BAD_TYPE);
}
static void implementation_semantic_concrete(void) {
    ImplementationSemanticFixture f; implementation_semantic_fixture(&f);
    XrXirProofContext context = {&f.module,{XR_XIR_CONTEXT_CLOSED,0,0}};
    XrXirBudget budget = xr_xir_default_budget();
    CHECK(xr_xir_interface_prove(&context,&f.module,f.box_meter,f.application,&budget)==XR_XIR_OK);
    XrXirWitnessRequest request = {&f.module,f.box_meter,f.application,0}; XrXirWitness witness = {0};
    CHECK(xr_xir_witness_resolve(&context,&request,&budget,&witness)==XR_XIR_OK);
    CHECK(witness.function==1 && witness.argument_count==1 && witness.arguments[0]==f.meter);
    XrXirType scalar = XR_XIR_I64; f.nodes[3].nominal.arguments = &scalar;
    budget = xr_xir_default_budget();
    CHECK(xr_xir_interface_prove(&context,&f.module,f.box_meter,f.application,&budget)==XR_XIR_BAD_TYPE);
    f.nodes[3].nominal.arguments = &f.meter;
    f.declarations.implementations = NULL; budget = xr_xir_default_budget();
    CHECK(xr_xir_interface_prove(&context,&f.module,f.meter,f.application,&budget)==XR_XIR_BAD_TYPE);
    CHECK(!live);
}
static void implementation_semantic_cross_pool(void) {
    ImplementationSemanticFixture source; implementation_semantic_fixture(&source);
    XrXirTypeNode nodes[5] = {0};
    nodes[0].kind = XR_XIR_TYPE_ARRAY; nodes[0].element = XR_XIR_I64;
    memcpy(nodes+1,source.nodes,sizeof(source.nodes));
    XrXirType meter = member_case_type(2), box = member_case_type(4);
    nodes[4].nominal.arguments = &meter;
    XrXirConstraint empty = {0};
    XrXirNominalDeclaration nominals[] = {source.nominals[0],source.nominals[1]};
    nominals[1].constraints = &empty;
    XrXirNominalTable nominal_table = {nominals,2,NULL};
    XrXirTypes types = {nodes,5,&nominal_table,NULL};
    XrXirModule actual = {0}; actual.types = &types;
    XrXirProofContext context = {&actual,{XR_XIR_CONTEXT_CLOSED,0,0}};
    XrXirWitnessRequest request = {&source.module,box,source.application,0};
    XrXirWitness witness = {0}; XrXirBudget budget = xr_xir_default_budget();
    CHECK(xr_xir_witness_resolve(&context,&request,&budget,&witness)==XR_XIR_OK);
    CHECK(witness.function==1 && witness.argument_count==1 && witness.arguments[0]==meter);
    source.declarations.implementations = NULL; budget = xr_xir_default_budget();
    CHECK(xr_xir_witness_resolve(&context,&request,&budget,&witness)==XR_XIR_BAD_TYPE);
    CHECK(!live);
}
static void implementation_semantic_shared_roots(void) {
    ImplementationSemanticFixture f; implementation_semantic_fixture(&f);
    XrXirInterfaceDeclaration interfaces[] = {f.interface,f.interface};
    interfaces[1].name = (XrXirLiteral){"AlsoMeasure",11};
    XrXirInterfaceTable table = {interfaces,2}; f.types.interfaces = &table;
    f.records[1].nominal_declaration = 0; f.records[1].interface.declaration = 1;
    f.bindings[1].requirement.declaration = 1; f.bindings[1].function = 0;
    CHECK(implementation_semantic_status(&f)==XR_XIR_OK);
    f.functions[1].parameters = &f.meter; f.identities[1].nominal_owner = 1;
    f.generics[1] = (XrXirGeneric){0}; f.bindings[1].function = 1;
    CHECK(implementation_semantic_status(&f)==XR_XIR_BAD_TYPE);
    XrXirImplementation swap = f.records[0]; f.records[0] = f.records[1]; f.records[1] = swap;
    CHECK(implementation_semantic_status(&f)==XR_XIR_BAD_TYPE);
}
static void implementation_semantic_resources(void) {
    size_t sites = 0;
    for (size_t site = 0; site <= sites; ++site) {
        ImplementationSemanticFixture f; implementation_semantic_fixture(&f);
        attempts = 0; fail_at = site ? site-1 : SIZE_MAX;
        XrXirStatus status = implementation_semantic_status(&f);
        CHECK(status==(site ? XR_XIR_OUT_OF_MEMORY : XR_XIR_OK));
        if (!site) sites = attempts;
    }
    fail_at = SIZE_MAX;
    ImplementationSemanticFixture f; implementation_semantic_fixture(&f);
    XrXirBudget budget = xr_xir_default_budget(); budget.work = 0;
    CHECK(xr_xir_implementations_verify(&f.module,&budget)==XR_XIR_BUDGET);
    budget = xr_xir_default_budget(); budget.scratch_bytes = 0;
    CHECK(xr_xir_implementations_verify(&f.module,&budget)==XR_XIR_BUDGET);
    CHECK(!live);
    printf("Implementation semantic proofs: %zu allocation failure sites\n",sites);
}
static void implementation_semantic_cases(void) {
    implementation_semantic_bindings(); implementation_semantic_concrete(); implementation_semantic_shared_roots();
    implementation_semantic_cross_pool(); implementation_semantic_resources();
}
#endif // XIR_IMPLEMENTATION_SEMANTIC_CASES_H
