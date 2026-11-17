/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_array_forwarding_cases.h - Preparation preserves all logical events and homes
 */
#include "lir_array_forwarding_fixture.h"
#include "lir/xlir_value_homes.h"

static uint64_t array_forwarding_home_digest(const XrLirValueHomes *homes) {
    uint64_t hash=0;
    for (size_t n=0;n<homes->event_count;++n) {
        const XrLirHomeEvent *event=&homes->events[n];
        hash=hash*UINT64_C(16777619)+event->forwarding_mode;
        hash=hash*UINT64_C(16777619)+event->forwarding_drop;
    }
    return hash;
}
static void array_forwarding_case(XrLirType element, enum ArrayForwardMode mode, uint32_t expected) {
    ArrayForwardFixture c=array_forward_fixture(element,mode); XrLirOwnershipPlan *plan=NULL;
    XrLirOwnershipRequest request={.module=c.module}; OK(xr_lir_ownership_plan_create(&request,&plan,NULL));
    compare_internal(&request,plan);
    uint32_t prepared=0,initial_drop=0,cell_drop=0,snapshot_owner=0;
    for (uint32_t n=0;n<xr_lir_ownership_fact_count(plan);++n) {
        XrLirOwnershipFact fact; OK(xr_lir_ownership_fact_at(plan,n,&fact));
        initial_drop+=fact.kind==XR_LIR_OWNERSHIP_RELEASE && fact.value==c.initial.id && fact.function==c.function.id;
        cell_drop+=fact.kind==XR_LIR_OWNERSHIP_BINDING_RELEASE && fact.value==c.cell.id && fact.function==c.function.id;
        snapshot_owner+=c.saved.id && fact.kind==XR_LIR_OWNERSHIP_RESULT_OWNER &&
            fact.value==c.saved.id && fact.function==c.function.id;
        if (!fact.forwarding_mode) { CHECK(!fact.forwarding_drop); continue; }
        CHECK(element==XR_LIR_I64 && fact.function==c.function.id && fact.block==c.block.id &&
            fact.value==c.cell.id && fact.type==XR_LIR_ARRAY_CELL && fact.array_element==XR_LIR_I64 &&
            fact.forwarding_mode==XR_LIR_OWNERSHIP_TRANSFER_MOVE && fact.forwarding_drop==fact.instruction+1 &&
            fact.transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_NONE);
        XrLirInstructionView copy,drop;
        OK(xr_lir_instruction_view(c.module,c.block,fact.instruction-1,&copy));
        OK(xr_lir_instruction_view(c.module,c.block,fact.forwarding_drop-1,&drop));
        if (fact.kind==XR_LIR_OWNERSHIP_BINDING_NEW) CHECK(copy.op==XR_LIR_CELL_NEW &&
            copy.result==c.cell.id && fact.target_value==c.initial.id && drop.op==XR_LIR_ARRAY_DROP &&
            drop.operands[0]==c.initial.id);
        else CHECK(fact.kind==XR_LIR_OWNERSHIP_BINDING_READ && copy.op==XR_LIR_CELL_READ &&
            copy.result==c.result.id && fact.target_value==c.result.id && drop.op==XR_LIR_ARRAY_CELL_DROP &&
            drop.operands[0]==c.cell.id);
        ++prepared;
    }
    CHECK(prepared==expected && initial_drop==1 && cell_drop==1 && snapshot_owner==(c.saved.id?1u:0u));
    XrtBillingOwner *billing=NULL; CHECK(xrt_billing_owner_new(NULL,SIZE_MAX,&billing)==XRT_MANAGED_OK);
    XrLirValueLayoutKey roots[4]={{XR_LIR_UNIT,0,0},{XR_LIR_I64,0,0},{XR_LIR_ARRAY,element,0}};
    size_t root_count=3;
    if (element!=XR_LIR_I64 && element!=XR_LIR_STRING)
        roots[root_count++]=(XrLirValueLayoutKey){element,0,0};
    XrLirValueLayouts *layouts=NULL;
    CHECK(xr_lir_value_layouts_create(c.module,roots,root_count,billing,&layouts)==XR_LIR_VALUE_LAYOUT_OK);
    XrLirValueHomes *homes=NULL;
    CHECK(xr_lir_value_homes_create(&(XrLirValueHomesRequest){c.module,c.function,layouts,plan},&homes)==XR_LIR_VALUE_LAYOUT_OK);
    const XrLirValueHome *source=&homes->values[c.initial.id],*result=&homes->values[c.result.id];
    const XrLirHomeBinding *binding=&homes->bindings[c.cell.id];
    CHECK(source->owner_count==1 && result->owner_count==1 && binding->kind==XR_LIR_HOME_BINDING_LOCAL &&
        binding->first_owner!=source->first_owner && binding->first_owner!=result->first_owner &&
        source->first_owner!=result->first_owner && source->offset!=result->offset);
    uint32_t received=0;
    for (size_t n=0;n<homes->event_count;++n) {
        const XrLirHomeEvent *event=&homes->events[n];
        if (!event->forwarding_mode) { CHECK(!event->forwarding_drop); continue; }
        CHECK(event->forwarding_mode==XR_LIR_OWNERSHIP_TRANSFER_MOVE && event->forwarding_drop==event->instruction+1 &&
            event->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_NONE && event->source==c.cell.id);
        bool retained=false;
        for (size_t p=0;p<homes->event_count;++p) {
            const XrLirHomeEvent *end=&homes->events[p];
            if (end->block!=event->block || end->instruction!=event->forwarding_drop) continue;
            retained=event->kind==XR_LIR_OWNERSHIP_BINDING_NEW ?
                end->kind==XR_LIR_OWNERSHIP_RELEASE && end->source==event->target :
                end->kind==XR_LIR_OWNERSHIP_BINDING_RELEASE && end->source==event->source;
            if (retained) { CHECK(!end->forwarding_mode && !end->forwarding_drop); break; }
        }
        CHECK(retained); ++received;
    }
    CHECK(received==expected);
    uint64_t before=digest(plan),home_before=array_forwarding_home_digest(homes); size_t events=homes->event_count;
    xr_lir_module_destroy(c.module); CHECK(digest(plan)==before && homes->event_count==events);
    xr_lir_ownership_plan_destroy(plan); xr_lir_value_layouts_destroy(layouts);
    CHECK(array_forwarding_home_digest(homes)==home_before);
    xr_lir_value_homes_destroy(homes);
    CHECK(xrt_billing_owner_stats(billing).blocks==1); xrt_billing_owner_drop(billing);
}
static void array_forwarding_preparation(void) {
    const uint32_t expected[]={2,1,1,0,0,0,2};
    for (unsigned mode=0;mode<sizeof(expected)/sizeof(*expected);++mode)
        array_forwarding_case(XR_LIR_I64,(enum ArrayForwardMode)mode,expected[mode]);
    const XrLirType others[]={XR_LIR_U8,XR_LIR_U64,XR_LIR_STRING};
    for (size_t t=0;t<sizeof(others)/sizeof(*others);++t)
        array_forwarding_case(others[t],ARRAY_FORWARD_LOCAL,0);
}
