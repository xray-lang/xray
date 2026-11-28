/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * rbc_header_edge_cases.h - Owner-only edges keep the ordinary place transaction
 */
#ifndef RBC_HEADER_EDGE_CASES_H
#define RBC_HEADER_EDGE_CASES_H
typedef struct HeaderEdgeCase {
    XrRbcProgram *program;
    const XrRbcModule *module;
    XrtBillingOwner *billing;
    XrtManagedRoot *root;
    XrtBackingTypes types;
    RbcValueActivation activation;
    XrRbcStack stack;
    RbcValueEdgeBuilder builder;
    XrLirHomeTransfer transfer;
    XrtPlaceEdge edges[2];
    RbcValuePublication publications[2];
    uint8_t snapshots[32];
    uint64_t expected_result;
} HeaderEdgeCase;

static XrRbcProgram *header_edge_program(unsigned kind) {
    bool tagged=kind!=0,mixed=kind>=2,reverse=kind==3;
    CArrayFixture c={0}; OK(xr_lir_module_create(NULL,&c.module));
    XrLirType type=XR_LIR_STRING;
    if (tagged) OK(xr_lir_nullable_type(c.module,XR_LIR_STRING,&type));
    XrLirFunction root=test_function(c.module,"header_edge",NULL,0,XR_LIR_I64);
    XrLirType types[]={reverse ? type : XR_LIR_STRING,reverse ? XR_LIR_STRING : type};
    XrLirBlock entry=test_block(c.module,root,NULL,0),join=test_block(c.module,root,mixed ? types : &type,mixed ? 2u : 1u);
    OK(xr_lir_set_entry(c.module,root,entry));
    // A call result keeps a real transfer; a local String producer would stay in one home.
    XrLirValue text=test_text_call(c.module,entry,test_text_function(c.module,"header_edge_text",1234));
    XrLirValue source=text,incoming={0};
    if (tagged) {
        source=ca_value(&c,entry,XR_LIR_NULLABLE_SOME,type,&text,1);
        if (!mixed) ca_effect(&c,entry,XR_LIR_STRING_DROP,text);
    }
    XrLirValue arguments[]={reverse ? source : text,reverse ? text : source};
    OK(xr_lir_set_terminator(c.module,entry,&(XrLirInstruction){.op=XR_LIR_BR,.type=XR_LIR_UNIT,
        .edge_count=1,.edges={{join,mixed ? arguments : &source,mixed ? 2u : 1u}}}));
    OK(xr_lir_block_parameter(c.module,join,mixed && !reverse ? 1u : 0u,&incoming));
    text=incoming;
    if (tagged) {
        text=ca_value(&c,join,XR_LIR_NULLABLE_UNWRAP,XR_LIR_STRING,&incoming,1);
        ca_effect(&c,join,XR_LIR_VALUE_DROP,incoming);
    }
    XrLirValue length=ca_value(&c,join,XR_LIR_STRING_LEN,XR_LIR_I64,&text,1);
    ca_effect(&c,join,XR_LIR_STRING_DROP,text);
    if (mixed) {
        OK(xr_lir_block_parameter(c.module,join,reverse ? 1u : 0u,&incoming));
        XrLirValue other=ca_value(&c,join,XR_LIR_STRING_LEN,XR_LIR_I64,&incoming,1);
        ca_effect(&c,join,XR_LIR_STRING_DROP,incoming); length=ca_binary(&c,join,XR_LIR_ADD,length,other);
    }
    ca_return(&c,join,length);
    OK(xr_lir_module_finish(c.module,NULL));
    XrRbcProgram *program=NULL;
    CHECK(xr_rbc_compile(c.module,root,&program,NULL)==XR_RBC_COMPILE_OK && program);
    xr_lir_module_destroy(c.module); return program;
}
static void header_edge_case_begin(HeaderEdgeCase *c,unsigned kind) {
    *c=(HeaderEdgeCase){.expected_result=kind>=2 ? 8u : 4u}; c->program=header_edge_program(kind);
    c->module=xr_rbc_program_view(c->program);
    const XrRbcValueCode *code=xr_rbc_module_value_code(c->module,0);
    CHECK(code && code->homes && code->homes->edge_count==1 && code->homes->transfer_count==(kind>=2 ? 2u : 1u));
    c->transfer=code->homes->transfers[code->homes->edges[0].first+(kind==3 ? 1u : 0u)];
    CHECK(c->transfer.transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_MOVE);
    CHECK(xrt_billing_owner_new(NULL,SIZE_MAX,&c->billing)==XRT_MANAGED_OK);
    CHECK(xrt_backing_types_init(c->billing,&c->types)==XRT_MANAGED_OK);
    CHECK(xrt_managed_root_new(c->billing,&c->root)==XRT_MANAGED_OK);
    XrtManagedFrameLocations locations={0};
    CHECK(xrt_managed_frame_enter_locations(c->root,&code->homes->frame,&locations)==XRT_MANAGED_OK);
    XrtManagedFrame *frame=locations.frame;
    c->activation=(RbcValueActivation){.code=code,.image=xr_rbc_module_value_image(c->module),
        .frame=frame,.payload=locations.payload,.frame_locations=locations,
        .owned_code=xr_rbc_module_code_owner(c->module)!=NULL,
        .edges=c->edges,.edge_capacity=2,.publications=c->publications,
        .publication_capacity=2,.snapshots=c->snapshots,.snapshot_capacity=sizeof(c->snapshots)};
    const XrLirValueHome *source=&code->homes->values[c->transfer.source],*target=&code->homes->values[c->transfer.target];
    CHECK(source->owner_count==1 && target->owner_count==1 && source->size==target->size);
    CHECK((target->byte_count!=0)==(kind==1) && source->size<=sizeof(c->snapshots));
    CHECK(xrt_string_backing_create(frame,source->first_owner,&c->types,"next",4,0)==XRT_MANAGED_OK);
    CHECK(xrt_string_backing_create(frame,target->first_owner,&c->types,"old",3,0)==XRT_MANAGED_OK);
    if (kind==1) { c->activation.payload[source->offset]=1; c->activation.payload[target->offset]=1; }
}
static XrtManagedStatus header_edge_build(HeaderEdgeCase *c,XrLirOwnershipTransferMode mode) {
    c->activation.publication_count=0; memset(c->snapshots,0xa5,sizeof(c->snapshots));
    c->builder=(RbcValueEdgeBuilder){.module=c->module,.stack=&c->stack,.activation=&c->activation};
    return rbc_value_edge_pair(&c->builder,c->transfer.source,c->transfer.target,mode,&c->transfer);
}
static XrtManagedStatus header_edge_apply(HeaderEdgeCase *c,XrLirOwnershipTransferMode mode) {
    XrtManagedStatus status=header_edge_build(c,mode);
    XrtPlacePublication publication={rbc_value_publication,&c->activation};
    return status==XRT_MANAGED_OK ? xrt_place_edge_transfer(c->edges,c->builder.owners,&publication) : status;
}
static void header_edge_case_end(HeaderEdgeCase *c) {
    CHECK(xrt_managed_root_destroy(&c->root)==XRT_MANAGED_OK);
    xrt_backing_types_drop(&c->types);
    CHECK(xrt_billing_owner_stats(c->billing).blocks==1); xrt_billing_owner_drop(c->billing);
    XrRbcExecution *execution=NULL;
    CHECK(xr_rbc_program_prepare(c->program,NULL,0,&execution)==XR_RBC_OK);
    xr_rbc_program_destroy(c->program);
    XrRbcOutcome *outcome=xr_rbc_execution_run(execution); uint64_t result=0;
    CHECK(outcome && xr_rbc_outcome_status(outcome)==XR_RBC_OK && xr_rbc_outcome_bits(outcome,&result) && result==c->expected_result);
    xr_rbc_execution_free(&execution); xr_rbc_outcome_free(&outcome); CHECK(!tracked_live);
}
static void header_only_edge_failures(void) {
    HeaderEdgeCase c; header_edge_case_begin(&c,false);
    const XrLirValueHomes *homes=c.activation.code->homes;
    const XrLirValueHome *source=&homes->values[c.transfer.source],*target=&homes->values[c.transfer.target];
    c.activation.snapshot_capacity=sizeof(XrtManagedHeader *)-1;
    CHECK(header_edge_build(&c,XR_LIR_OWNERSHIP_TRANSFER_MOVE)==XRT_MANAGED_ARGUMENT);
    c.activation.snapshot_capacity=sizeof(c.snapshots); c.activation.publication_capacity=0;
    CHECK(header_edge_build(&c,XR_LIR_OWNERSHIP_TRANSFER_MOVE)==XRT_MANAGED_ARGUMENT);
    c.activation.publication_capacity=1;
    CHECK(header_edge_build(&c,XR_LIR_OWNERSHIP_TRANSFER_MOVE)==XRT_MANAGED_OK);
    CHECK(rbc_value_edge_pair(&c.builder,c.transfer.source,c.transfer.target,
        XR_LIR_OWNERSHIP_TRANSFER_MOVE,&c.transfer)==XRT_MANAGED_ARGUMENT);
    c.activation.publication_capacity=2;
    CHECK(header_edge_build(&c,XR_LIR_OWNERSHIP_TRANSFER_COPY)==XRT_MANAGED_OK);
    CHECK(c.activation.publication_count==0 && c.builder.rows==1 && c.builder.bytes==source->size);
    for (size_t n=0;n<sizeof(c.snapshots);++n) CHECK(c.snapshots[n]==0xa5);
    XrtManagedSlotLocation from,to,temporary;
    CHECK(xrt_managed_slot_location(c.activation.frame,source->first_owner,&from)==XRT_MANAGED_OK);
    CHECK(xrt_managed_slot_location(c.activation.frame,target->first_owner,&to)==XRT_MANAGED_OK);
    CHECK(xrt_managed_slot_location(c.activation.frame,target->temporary_first_owner,&temporary)==XRT_MANAGED_OK);
    XrtManagedHeader *old=*to.owner,*value=*from.owner;
    RbcValueOrigin origin={&c.activation,c.transfer.source};
    const XrtManagedHeader *borrowed=NULL;
    CHECK(c.activation.owned_code && rbc_value_borrow_header(origin,source,&borrowed)==XRT_MANAGED_OK && borrowed==value);
    atomic_store(&value->references,INT32_MAX);
    CHECK(header_edge_apply(&c,XR_LIR_OWNERSHIP_TRANSFER_COPY)==XRT_MANAGED_REFCOUNT);
    CHECK(*from.owner==value && *to.owner==old && !*temporary.owner);
    atomic_store(&value->references,1);
    size_t pending_slots[]={source->first_owner,target->first_owner,target->temporary_first_owner};
    for (size_t n=0;n<3;++n) {
        XrtManagedHeader *pending=NULL;
        CHECK(xrt_managed_slot_prepare(c.activation.frame,pending_slots[n],c.types.string,
            sizeof(XrtStringBacking)+1,&pending)==XRT_MANAGED_OK);
        if (n==0) {
            borrowed=NULL;
            CHECK(rbc_value_borrow_header(origin,source,&borrowed)==XRT_MANAGED_ARGUMENT && !borrowed);
        }
        CHECK(header_edge_apply(&c,XR_LIR_OWNERSHIP_TRANSFER_MOVE)==XRT_MANAGED_ARGUMENT);
        CHECK(*from.owner==value && *to.owner==old && !*temporary.owner);
        CHECK(xrt_managed_slot_abort(c.activation.frame,pending_slots[n])==XRT_MANAGED_OK);
    }
    CHECK(from.busy && !*from.busy); *from.busy=true;
    borrowed=NULL;
    CHECK(rbc_value_borrow_header(origin,source,&borrowed)==XRT_MANAGED_ARGUMENT && !borrowed);
    CHECK(header_edge_apply(&c,XR_LIR_OWNERSHIP_TRANSFER_MOVE)==XRT_MANAGED_ARGUMENT);
    CHECK(*from.owner==value && *to.owner==old && !*temporary.owner && *from.busy); *from.busy=false;
    CHECK(xrt_managed_slot_drop(c.activation.frame,source->first_owner)==XRT_MANAGED_OK);
    CHECK(rbc_value_borrow_header(origin,source,&borrowed)==XRT_MANAGED_ARGUMENT && !borrowed);
    CHECK(header_edge_apply(&c,XR_LIR_OWNERSHIP_TRANSFER_MOVE)==XRT_MANAGED_ARGUMENT);
    CHECK(!*from.owner && *to.owner==old && !*temporary.owner);
    CHECK(xrt_string_backing_create(c.activation.frame,source->first_owner,&c.types,"next",4,0)==XRT_MANAGED_OK);
    value=*from.owner;
    CHECK(header_edge_apply(&c,XR_LIR_OWNERSHIP_TRANSFER_MOVE)==XRT_MANAGED_OK);
    CHECK(*to.owner==value && !*from.owner && !*temporary.owner && xr_signed_rc_read(&value->references)==1);
    CHECK(c.activation.publication_count==0);
    header_edge_case_end(&c);
}
static void tagged_edge_keeps_publication(void) {
    HeaderEdgeCase c; header_edge_case_begin(&c,true);
    const XrLirValueHomes *homes=c.activation.code->homes;
    const XrLirValueHome *source=&homes->values[c.transfer.source],*target=&homes->values[c.transfer.target];
    CHECK(header_edge_apply(&c,XR_LIR_OWNERSHIP_TRANSFER_MOVE)==XRT_MANAGED_OK);
    CHECK(c.activation.publication_count==1 && c.builder.rows==1 && c.builder.bytes==source->size);
    CHECK(c.activation.payload[source->offset]==0 && c.activation.payload[target->offset]==1);
    XrtManagedHeader *owner=NULL;
    CHECK(xrt_managed_slot_borrow(c.activation.frame,target->first_owner,&owner)==XRT_MANAGED_OK && owner);
    CHECK(((XrtStringBacking *)owner)->length==4 && xr_signed_rc_read(&owner->references)==1);
    header_edge_case_end(&c);
}
static XrtManagedStatus mixed_edge_build(HeaderEdgeCase *c,XrLirHomeTransfer tagged,bool reverse) {
    c->activation.publication_count=0; memset(c->snapshots,0xa5,sizeof(c->snapshots));
    c->builder=(RbcValueEdgeBuilder){.module=c->module,.stack=&c->stack,.activation=&c->activation};
    XrLirHomeTransfer first=reverse ? tagged : c->transfer,second=reverse ? c->transfer : tagged;
    XrtManagedStatus status=rbc_value_edge_pair(&c->builder,first.source,first.target,first.transfer_mode,&first);
    return status==XRT_MANAGED_OK ? rbc_value_edge_pair(&c->builder,second.source,second.target,
        second.transfer_mode,&second) : status;
}
static void mixed_edge_publication_order(bool reverse) {
    HeaderEdgeCase c; header_edge_case_begin(&c,reverse ? 3u : 2u);
    const XrLirValueHomes *homes=c.activation.code->homes;
    XrLirHomeTransfer tagged=homes->transfers[homes->edges[0].first+(reverse ? 0u : 1u)];
    const XrLirValueHome *source=&homes->values[tagged.source],*target=&homes->values[tagged.target];
    CHECK(source->byte_count && target->byte_count);
    CHECK(xrt_string_backing_create(c.activation.frame,source->first_owner,&c.types,"next",4,0)==XRT_MANAGED_OK);
    CHECK(xrt_string_backing_create(c.activation.frame,target->first_owner,&c.types,"old",3,0)==XRT_MANAGED_OK);
    c.activation.payload[source->offset]=1; c.activation.payload[target->offset]=0;
    c.activation.publication_capacity=1;
    CHECK(mixed_edge_build(&c,tagged,reverse)==XRT_MANAGED_ARGUMENT);
    CHECK(c.activation.publication_count==(reverse ? 1u : 0u));
    c.activation.publication_capacity=2;
    size_t skipped=c.activation.code->homes->values[c.transfer.source].size;
    c.activation.snapshot_capacity=skipped+source->size-1;
    CHECK(mixed_edge_build(&c,tagged,reverse)==XRT_MANAGED_ARGUMENT);
    CHECK(c.activation.publication_count==(reverse ? 1u : 0u));
    c.activation.snapshot_capacity=sizeof(c.snapshots);
    CHECK(mixed_edge_build(&c,tagged,reverse)==XRT_MANAGED_OK);
    CHECK(c.builder.rows==2 && c.activation.publication_count==1 && c.builder.bytes==skipped+source->size);
    CHECK(c.publications[0].snapshot==c.snapshots+(reverse ? 0 : skipped));
    for (size_t n=0;n<skipped;++n) CHECK(c.snapshots[(reverse ? source->size : 0)+n]==0xa5);
    XrtPlacePublication publication={rbc_value_publication,&c.activation};
    CHECK(xrt_place_edge_transfer(c.edges,c.builder.owners,&publication)==XRT_MANAGED_OK);
    CHECK(c.activation.payload[source->offset]==0 && c.activation.payload[target->offset]==1);
    header_edge_case_end(&c);
}
#endif // RBC_HEADER_EDGE_CASES_H
