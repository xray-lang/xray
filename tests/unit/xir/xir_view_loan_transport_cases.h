/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_view_loan_transport_cases.h - Context isolation and numeric owner failures
 */
#ifndef XIR_VIEW_LOAN_TRANSPORT_CASES_H
#define XIR_VIEW_LOAN_TRANSPORT_CASES_H
#include "xir/xxir_view_loan_transport_internal.h"
#include "xir/xxir_view_types_internal.h"

typedef struct ViewLoanTransportFixture {
    XrXirTypeNode nodes[2];
    XrXirTypes types;
    XrXirType parameters[3];
    XrXirInstruction returns[2];
    XrXirFunction functions[2];
    XrXirModule module;
    uint8_t flags[2];
    uint32_t intrinsic[11];
    XirRestrictionFormulaRow formulas[11];
    XrXirValueFlow flows[3], formal_flow;
    XirRestrictionFunction restrictions[2];
    XirStorageFlowFunction storage[2];
    XirStorageCrossFlow crossflows[2];
    XirStorageApplication applications[2];
    XirStorageBinding bindings[2];
    XirStorageInput input;
    XirStorageUnknown unknown;
    XirViewLoanFacts facts;
} ViewLoanTransportFixture;

static void view_loan_transport_fixture(ViewLoanTransportFixture *f) {
    *f=(ViewLoanTransportFixture){0};
    XrXirType slice=XR_XIR_CONSTRUCTED_TYPE_BASE,callable=slice+1;
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_U8};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_UNIT,
        .element=XR_XIR_NEVER,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    f->types=(XrXirTypes){.nodes=f->nodes,.count=2};
    f->parameters[0]=f->parameters[1]=slice;f->parameters[2]=callable;
    f->flags[0]=XR_XIR_VIEW_SHARED|4u;
    for (uint32_t n=0;n<2;++n) {
        f->returns[n].op=XR_XIR_RETURN;
        f->functions[n]=(XrXirFunction){.parameters=f->parameters+(n?2:0),.parameter_count=n?1:2,
            .instructions=f->returns+n,.instruction_count=1,.error_type=XR_XIR_NEVER};
    }
    f->module=(XrXirModule){.types=&f->types,.functions=f->functions,.function_count=2};
    f->flows[0]=(XrXirValueFlow){0,4};f->flows[1]=(XrXirValueFlow){1,3};
    f->formal_flow=(XrXirValueFlow){2,3};
    for (uint32_t n=0;n<11;++n) {f->intrinsic[n]=n;f->formulas[n].constant=true;}
    f->restrictions[0]=(XirRestrictionFunction){3,7,f->intrinsic,f->flows,2};
    f->restrictions[1]=(XirRestrictionFunction){2,4,f->intrinsic+7,&f->formal_flow,1};
    f->storage[0]=(XirStorageFlowFunction){.physical_values=3,.value_count=7,.parameter_count=2};
    f->storage[1]=(XirStorageFlowFunction){.physical_values=2,.value_count=4,.parameter_count=1};
    for (uint32_t n=0;n<2;++n) {
        f->crossflows[n]=(XirStorageCrossFlow){1,3,0,5+n,n,XIR_STORAGE_FOOTPRINT};
        f->applications[n]=(XirStorageApplication){.kind=XIR_EXECUTION_CALL,.function=0,.declaration=1,
            .environment=XIR_EXECUTION_CURRENT_ENVIRONMENT,.first=n,.count=1,.protocol=2};
        f->bindings[n]=(XirStorageBinding){1,2,0,n?3:4,XIR_EXECUTION_CURRENT_ENVIRONMENT};
    }
    f->input=(XirStorageInput){1,2,0,XIR_STORAGE_FOOTPRINT};
    f->unknown=(XirStorageUnknown){0,4,callable,UINT32_MAX};
    f->facts=(XirViewLoanFacts){
        .restrictions={f->restrictions,2,f->formulas,11,NULL,0,NULL,0},
        .storage={.functions=f->storage,.function_count=2,.crossflows=f->crossflows,.crossflow_count=2,
            .applications=f->applications,.application_count=2,.bindings=f->bindings,.binding_count=2,
            .inputs=&f->input,.input_count=1,.unknowns=&f->unknown}};
}

static XrXirStatus view_loan_transport_physical(void *context,uint32_t value) {
    if (value>=64) return XR_XIR_BAD_STRUCTURE;
    *(uint64_t *)context|=UINT64_C(1)<<value;
    return XR_XIR_OK;
}

static XrXirStatus view_loan_transport_case(ViewLoanTransportFixture *fixture,uint32_t carrier,uint64_t *mask) {
    XirViewLoanTransport *transport=NULL;
    XrXirStatus status=xir_view_loan_transport_new(&effect_context,&fixture->module,&fixture->facts,
        fixture->flags,NULL,&transport);
    XirViewLoanDemand demand={.function=0,.carrier=carrier,.physical=view_loan_transport_physical,.context=mask};
    if (status==XR_XIR_OK) status=xir_view_loan_transport_demand(transport,&demand);
    xir_view_loan_transport_free(transport);return status;
}

static void view_loan_transport_definition(ViewLoanTransportFixture *fixture) {
    XrXirValueFlow flows[] = {{0,3}, {2,3}};
    fixture->restrictions[1].flows = flows;
    fixture->restrictions[1].flow_count = 2;
    XirViewLoanTransport *transport = NULL;
    uint64_t mask = 0;
    CHECK(xir_view_loan_transport_new(&effect_context, &fixture->module, &fixture->facts,
        fixture->flags, NULL, &transport) == XR_XIR_OK);
    XirViewLoanDemand demand = {.function=1, .carrier=3,
        .physical=view_loan_transport_physical, .context=&mask};
    CHECK(xir_view_loan_transport_demand(transport, &demand) == XR_XIR_OK && mask == 1);
    xir_view_loan_transport_free(transport);
    uint32_t ordinal = 0;
    fixture->functions[1].escaping_parameters = &ordinal;
    fixture->functions[1].escaping_parameter_count = 1;
    transport = NULL;
    CHECK(xir_view_loan_transport_new(&effect_context, &fixture->module, &fixture->facts,
        fixture->flags, NULL, &transport) == XR_XIR_OK);
    CHECK(xir_view_loan_transport_demand(transport, &demand) == XR_XIR_BAD_STRUCTURE);
    xir_view_loan_transport_free(transport);
    fixture->functions[1].escaping_parameters = NULL;
    fixture->functions[1].escaping_parameter_count = 0;
    fixture->restrictions[1].flows = &fixture->formal_flow;
    fixture->restrictions[1].flow_count = 1;
}

static void view_loan_transport_cases(void) {
    ViewLoanTransportFixture fixture;view_loan_transport_fixture(&fixture);
    EffectMark baseline=effect_mark();uint64_t mask=0;
    attempts=0;
    CHECK(view_loan_transport_case(&fixture,5,&mask)==XR_XIR_OK && mask==1);
    size_t sites=attempts;
    mask=0;CHECK(view_loan_transport_case(&fixture,6,&mask)==XR_XIR_OK && mask==2);
    XirViewLoanTransport *transport=NULL;
    attempts=0;
    CHECK(xir_view_loan_transport_new(&effect_context,&fixture.module,&fixture.facts,
        fixture.flags,NULL,&transport)==XR_XIR_OK);
    CHECK(attempts==1);
    XirViewLoanDemand demand={.function=0,.carrier=5,.physical=view_loan_transport_physical,.context=&mask};
    mask=0;CHECK(xir_view_loan_transport_demand(transport,&demand)==XR_XIR_OK && mask==1);
    demand.carrier=6;mask=0;
    CHECK(xir_view_loan_transport_demand(transport,&demand)==XR_XIR_OK && mask==2);
    demand.frame_bytes=effect_context.limits.frame_bytes;
    CHECK(xir_view_loan_transport_demand(transport,&demand)==XR_XIR_BUDGET);
    xir_view_loan_transport_free(transport);
    effect_mark_check(baseline);
    for (size_t n=0;n<sites;++n) {
        attempts=0;injected=false;fail_at=n;mask=0;
        CHECK(view_loan_transport_case(&fixture,5,&mask)==XR_XIR_OUT_OF_MEMORY && injected);
        fail_at=SIZE_MAX;effect_mark_check(baseline);
    }
    fixture.bindings[0].actual_environment=XIR_EXECUTION_UNKNOWN_ENVIRONMENT;
    CHECK(view_loan_transport_case(&fixture,5,&(uint64_t){0})==XR_XIR_UNRESOLVED);
    fixture.bindings[0].actual_environment=0;
    CHECK(view_loan_transport_case(&fixture,5,&(uint64_t){0})==XR_XIR_UNRESOLVED);
    fixture.bindings[0].actual_environment=XIR_EXECUTION_CURRENT_ENVIRONMENT;
    fixture.applications[0].count=0;
    CHECK(view_loan_transport_case(&fixture,5,&(uint64_t){0})==XR_XIR_UNRESOLVED);
    fixture.applications[0].count=1;
    fixture.facts.storage.unknown_count=1;
    CHECK(view_loan_transport_case(&fixture,5,&(uint64_t){0})==XR_XIR_UNRESOLVED);
    fixture.facts.storage.unknown_count=0;
    fixture.crossflows[0].application=2;
    CHECK(view_loan_transport_case(&fixture,5,&(uint64_t){0})==XR_XIR_BAD_STRUCTURE);
    fixture.crossflows[0].application=0;
    fixture.flows[0]=(XrXirValueFlow){3,4};fixture.flows[1]=(XrXirValueFlow){4,3};
    CHECK(view_loan_transport_case(&fixture,5,&(uint64_t){0})==XR_XIR_UNRESOLVED);
    fixture.flows[2]=(XrXirValueFlow){0,4};
    fixture.restrictions[0].flow_count=3;
    mask=0;CHECK(view_loan_transport_case(&fixture,5,&mask)==XR_XIR_OK && mask==1);
    fixture.parameters[0]=XR_XIR_TYPE_PARAMETER_BASE;
    CHECK(view_loan_transport_case(&fixture,5,&(uint64_t){0})==XR_XIR_UNRESOLVED);
    fixture.parameters[0]=XR_XIR_CONSTRUCTED_TYPE_BASE;
    view_loan_transport_definition(&fixture);
    effect_mark_check(baseline);
    printf("loan transport: isolated application inputs and %zu allocation failures PASS\n",sites);
}
#endif // XIR_VIEW_LOAN_TRANSPORT_CASES_H
