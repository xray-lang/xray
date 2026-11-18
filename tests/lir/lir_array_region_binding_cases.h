/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_array_region_binding_cases.h - Numeric bindings outlive their creators
 */
#ifndef LIR_ARRAY_REGION_BINDING_CASES_H
#define LIR_ARRAY_REGION_BINDING_CASES_H
static ArraySetRegionFixture array_region_binding_two_loops(void) {
    ArraySetRegionFixture c={.arity=2,.mode=ARRAY_SET_REGION_LOCAL};
    OK(xr_lir_array_module_create(NULL,&c.module));
    OK(xr_lir_declare_function(c.module,&(XrLirFunctionSpec){.name="two_regions",.name_length=11,
        .result=XR_LIR_I64},&c.function));
    OK(xr_lir_define_function(c.module,c.function));
    OK(xr_lir_add_block(c.module,c.function,NULL,0,&c.entry)); OK(xr_lir_set_entry(c.module,c.function,c.entry));
    XrLirBlock current=c.entry;
    XrLirType types[]={XR_LIR_ARRAY,XR_LIR_I64};
    for (uint32_t loop=0;loop<2;++loop) {
        c.count=loop+2;
        OK(xr_lir_add_block(c.module,c.function,types,2,&c.header));
        for (uint32_t n=0;n<c.count;++n) OK(xr_lir_add_block(c.module,c.function,types,2,&c.body[n]));
        OK(xr_lir_add_block(c.module,c.function,types,2,&c.exit));
        XrLirValue item=ars_number(&c,current,7),values[3]={{0}};
        values[0]=ars_array_op(&c,current,XR_LIR_ARRAY_OP_NEW,&item,1); values[1]=ars_number(&c,current,0);
        ars_branch(&c,current,c.header,values); ars_parameters(&c,c.header,values); c.header_array=values[0];
        (void)ars_array_op(&c,c.header,XR_LIR_ARRAY_OP_LEN,values,1);
        XrLirValue limit=ars_number(&c,c.header,4),operands[]={values[1],limit};
        XrLirValue condition=ars_value(&c,c.header,(XrLirInstruction){.op=XR_LIR_ICMP,.type=XR_LIR_I1,
            .predicate=XR_LIR_LT,.operands=operands,.operand_count=2});
        OK(xr_lir_set_terminator(c.module,c.header,&(XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,
            .operands=&condition,.operand_count=1,.edge_count=2,.edges={{c.body[0],values,2},{c.exit,values,2}}}));
        for (uint32_t n=0;n<c.count;++n) {
            ars_parameters(&c,c.body[n],values);
            if (!n) values[0]=ars_mutate(&c,c.body[n],values[0]);
            if (n+1==c.count) {
                XrLirValue increment[]={values[1],ars_number(&c,c.body[n],1)};
                values[1]=ars_value(&c,c.body[n],(XrLirInstruction){.op=XR_LIR_ADD,.type=XR_LIR_I64,
                    .operands=increment,.operand_count=2});
            }
            ars_branch(&c,c.body[n],n+1==c.count ? c.header : c.body[n+1],values);
        }
        ars_parameters(&c,c.exit,values); c.result=ars_array_op(&c,c.exit,XR_LIR_ARRAY_OP_LEN,values,1);
        ars_effect(&c,c.exit,XR_LIR_ARRAY_DROP,values[0]); current=c.exit;
    }
    OK(xr_lir_set_terminator(c.module,current,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,
        .operands=&c.result,.operand_count=1}));
    XrLirDiagnostic diagnostic={0}; XrLirStatus status=xr_lir_module_finish(c.module,&diagnostic);
    if (status!=XR_LIR_OK) fprintf(stderr,"two regions f%u b%u i%u: %s\n",diagnostic.function,
        diagnostic.block,diagnostic.instruction,diagnostic.message);
    OK(status); return c;
}
static void array_region_binding_check(const XrLirArrayRegionBindings *b,const XrLirValueHomes *homes,
    const XrLirModule *module,XrLirFunction function) {
    CHECK(b->region_count==homes->array_region_count);
    for (size_t n=0;n<b->region_count;++n) {
        const XrLirArrayRegionBinding *r=&b->regions[n]; CHECK(r->slot_count && r->move_count==r->region.step_count);
        CHECK(r->canonical_slot>=r->first_slot && r->canonical_slot-r->first_slot<r->slot_count);
        XrtArrayI64RegionSpec runtime={b->slots+r->first_slot,r->slot_count,r->canonical_slot-r->first_slot};
        CHECK(runtime.slots[runtime.canonical].slot==homes->values[r->region.canonical_value].first_owner);
        for (size_t p=0;p<r->slot_count;++p) {
            XrtArrayI64RegionSlot slot=runtime.slots[p];
            CHECK(slot.slot<homes->frame.slot_count && homes->frame.owned_offsets[slot.slot]==slot.offset);
            for (size_t q=0;q<p;++q) CHECK(runtime.slots[q].slot!=slot.slot && runtime.slots[q].offset!=slot.offset);
        }
        for (size_t p=0;p<r->move_count;++p) {
            const XrLirArrayRegionMove *move=&b->moves[r->first_move+p];
            CHECK(move->source_slot>=r->first_slot && move->source_slot-r->first_slot<r->slot_count);
            CHECK(move->target_slot>=r->first_slot && move->target_slot-r->first_slot<r->slot_count);
            const XrLirArrayRegionBlock *block=&b->block_index[move->step.block];
            if (move->step.edge) {
                size_t edge=block->edges[move->step.edge-1]; CHECK(edge && edge<=b->edge_count);
                CHECK(b->edges[edge-1].region==n+1 && b->edges[edge-1].move==r->first_move+p+1);
            } else {
                CHECK(block->region==n+1);
                CHECK(b->operations[block->first_operation+move->step.instruction-1].move==r->first_move+p+1);
                CHECK(b->operations[block->first_operation+move->step.drop-1].drop==r->first_move+p+1);
                bool found=false;
                for (size_t e=0;e<homes->event_count;++e) {
                    const XrLirHomeEvent *event=&homes->events[e];
                    if (event->block==move->step.block && event->instruction==move->step.instruction &&
                        event->forwarding_mode==XR_LIR_OWNERSHIP_TRANSFER_MOVE) {
                        CHECK(event->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_NONE && event->forwarding_drop==move->step.drop);
                        found=true;
                    }
                }
                CHECK(found);
            }
        }
        CHECK(r->block_count==(size_t)r->region.body_block_count+1);
        for (size_t p=0;p<r->block_count;++p) {
            uint32_t block=b->blocks[r->first_block+p]; CHECK(block && block<b->block_index_count);
            const XrLirArrayRegionBlock *index=&b->block_index[block];
            XrLirBlockView body={0}; OK(xr_lir_block_view(module,(XrLirBlock){module,function.id,block},&body));
            CHECK(index->region==n+1 && index->instruction_count==body.instruction_count &&
                index->first_operation<=b->operation_count && index->instruction_count<=b->operation_count-index->first_operation);
        }
        for (size_t p=0;p<r->access_count;++p) {
            const XrLirArrayRegionAccess *a=&b->accesses[r->first_access+p];
            XrLirInstructionView instruction={0};
            OK(xr_lir_instruction_view(module,(XrLirBlock){module,function.id,a->block},a->instruction-1,&instruction));
            CHECK(a->receiver==instruction.operands[0] && a->result==instruction.result);
            CHECK(a->receiver_slot>=r->first_slot && a->receiver_slot-r->first_slot<r->slot_count);
            CHECK(b->operations[b->block_index[a->block].first_operation+a->instruction-1].access==r->first_access+p+1);
            if (a->operation==XR_LIR_ARRAY_OP_SET) {
                CHECK(a->index==instruction.operands[1] && a->item==instruction.operands[2]);
                CHECK(a->workspace_slot>=r->first_slot && a->workspace_slot-r->first_slot<r->slot_count);
            } else {
                CHECK(a->workspace_slot==SIZE_MAX && !a->item);
                CHECK(a->operation==XR_LIR_ARRAY_OP_GET ? a->index==instruction.operands[1] : !a->index);
            }
        }
    }
}
static void array_region_binding_unsupported(const XrLirValueHomes *homes,const XrLirValueLayouts *set) {
    size_t count=(size_t)homes->value_count+1;
    XrLirValueHome *values=xr_malloc(count*sizeof(*values)); CHECK(values);
    for (uint32_t disabled=0;disabled<homes->array_region_count;++disabled) {
        memcpy(values,homes->values,count*sizeof(*values));
        values[homes->array_regions[disabled].canonical_value].parameter=1;
        XrLirValueHomes view=*homes; view.values=values;
        XrLirArrayRegionBindings *bindings=NULL; HOME(xr_lir_array_region_bindings_create(&view,set,&bindings));
        CHECK(bindings->region_count==homes->array_region_count && !bindings->regions[disabled].slot_count);
        for (size_t n=0;n<bindings->region_count;++n) if (n!=disabled) CHECK(bindings->regions[n].slot_count);
        for (size_t n=0;n<bindings->block_index_count;++n) CHECK(bindings->block_index[n].region!=disabled+1);
        for (size_t n=0;n<bindings->edge_count;++n) CHECK(bindings->edges[n].region!=disabled+1);
        xr_lir_array_region_bindings_destroy(bindings);
    }
    xr_free(values);
    XrLirValueHomes view=*homes; view.frame.payload_alignment=1;
    XrLirArrayRegionBindings *bindings=NULL; HOME(xr_lir_array_region_bindings_create(&view,set,&bindings));
    CHECK(!bindings->slot_count && !bindings->move_count && !bindings->access_count && !bindings->block_count && !bindings->operation_count);
    xr_lir_array_region_bindings_destroy(bindings); bindings=NULL;
    view=*homes; view.array_region_count=view.array_region_step_count=0;
    HOME(xr_lir_array_region_bindings_create(&view,set,&bindings));
    CHECK(bindings && !bindings->region_count && !bindings->slot_count && !bindings->edge_count);
    xr_lir_array_region_bindings_destroy(bindings);
    bindings=NULL; view=*homes; view.frame.slot_count=SIZE_MAX;
    CHECK(xr_lir_array_region_bindings_create(&view,set,&bindings)==XR_LIR_VALUE_LAYOUT_OVERFLOW && !bindings);
}
static void array_region_binding_projection(bool large) {
    ArraySetRegionFixture c=large ? array_set_region_fixture(257,ARRAY_SET_REGION_LOCAL) : array_region_binding_two_loops();
    XrtBillingOwner *billing=NULL; MANAGED(xrt_billing_owner_new(NULL,SIZE_MAX,&billing));
    XrLirValueLayoutKey roots[]={{XR_LIR_UNIT,0,0},{XR_LIR_I1,0,0},{XR_LIR_I64,0,0},{XR_LIR_ARRAY,XR_LIR_I64,0}};
    XrLirValueLayouts *set=NULL; HOME(xr_lir_value_layouts_create(c.module,roots,4,billing,&set));
    XrLirOwnershipPlan *plan=NULL; OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=c.module},&plan,NULL));
    XrLirValueHomes *homes=NULL;
    HOME(xr_lir_value_homes_create(&(XrLirValueHomesRequest){c.module,c.function,set,plan},&homes));
    CHECK(homes->array_region_count==(large ? 1u : 2u));
    size_t baseline=live,bytes=home_live_bytes(); attempts=0;
    XrLirArrayRegionBindings *bindings=NULL; HOME(xr_lir_array_region_bindings_create(homes,set,&bindings));
    size_t sites=attempts; CHECK(sites && live>baseline);
    array_region_binding_check(bindings,homes,c.module,c.function);
    if (!large) CHECK(bindings->regions[1].first_slot && bindings->regions[1].first_move &&
        bindings->regions[1].first_access && bindings->regions[1].canonical_slot>=bindings->regions[1].first_slot);
    XrLirArrayRegionBindings *preserved=bindings;
    CHECK(xr_lir_array_region_bindings_create(homes,set,&bindings)==XR_LIR_VALUE_LAYOUT_ARGUMENT && bindings==preserved);
    xr_lir_array_region_bindings_destroy(bindings); bindings=NULL;
    CHECK(live==baseline && home_live_bytes()==bytes);
    for (size_t point=1;point<=sites;++point) {
        attempts=0; fail_at=point;
        CHECK(xr_lir_array_region_bindings_create(homes,set,&bindings)==XR_LIR_VALUE_LAYOUT_OOM &&
            !bindings && attempts>=point && live==baseline && home_live_bytes()==bytes);
    }
    fail_at=SIZE_MAX;
    if (!large) array_region_binding_unsupported(homes,set);
    HOME(xr_lir_array_region_bindings_create(homes,set,&bindings));
    size_t regions=bindings->region_count,moves=bindings->move_count,accesses=bindings->access_count;
    XrLirArraySetRegion first=bindings->regions[0].region;
    xr_lir_value_homes_destroy(homes); xr_lir_ownership_plan_destroy(plan);
    xr_lir_value_layouts_destroy(set); xr_lir_module_destroy(c.module);
    CHECK(bindings->region_count==regions && bindings->move_count==moves && bindings->access_count==accesses &&
        bindings->regions[0].region.canonical_value==first.canonical_value);
    for (size_t n=0;n<regions;++n) {
        const XrLirArrayRegionBinding *r=&bindings->regions[n];
        CHECK(r->slot_count && r->canonical_slot<bindings->slot_count && r->first_move+r->move_count<=moves &&
            r->first_access+r->access_count<=accesses);
    }
    xr_lir_array_region_bindings_destroy(bindings); CHECK(!live && !home_live_bytes());
    CHECK(xrt_billing_owner_stats(billing).blocks==1); xrt_billing_owner_drop(billing);
    printf("Array region binding: %zu regions, %zu moves, %zu accesses, %zu allocation failures preserve output\n",
        regions,moves,accesses,sites);
}
#endif // LIR_ARRAY_REGION_BINDING_CASES_H
