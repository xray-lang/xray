/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_array_copy_cases.h - Selected-edge parallel copies retain one real owner
 */
static void compiled_array_edge_owners(bool stationary) {
    CArrayFixture builder={.element=XR_LIR_I64};
    OK(xr_lir_array_module_create(NULL,&builder.module));
    XrLirModule *m=builder.module;
    XrLirType flag=XR_LIR_I1;
    XrLirFunction f=test_function(m,"array_copy",stationary?NULL:&flag,stationary?0:1,XR_LIR_I64);
    XrLirType types[]={XR_LIR_ARRAY,XR_LIR_ARRAY};
    XrLirBlock entry=test_block(m,f,NULL,0),join=test_block(m,f,types,2);
    OK(xr_lir_set_entry(m,f,entry));
    XrLirFunction helper={0};
    if (!stationary) helper=ca_helper(&builder,1);
    XrLirValue values[2],parameters[2];
    for(uint32_t p=0;p<2;++p) {
        XrLirValue number=ca_constant(&builder,entry,p?11:7);
        values[p]=stationary ? ca_intrinsic(&builder,entry,XR_LIR_ARRAY_OP_NEW,XR_LIR_ARRAY,&number,1) :
            ca_call_array(&builder,entry,helper,number);
        OK(xr_lir_block_parameter(m,join,p,&parameters[p]));
        XrLirArrayShape shape={XR_LIR_I64};OK(xr_lir_set_array_shape(m,parameters[p],&shape));
    }
    if (stationary) {
        test_branch(m,entry,join,values,2);
    } else {
        /* A join reached in either order cannot reuse one origin per slot. */
        XrLirValue condition={0},swapped[]={values[1],values[0]};
        OK(xr_lir_function_parameter(m,f,0,&condition));
        XrLirInstruction select={.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,.operands=&condition,.operand_count=1,
            .edge_count=2,.edges={{join,values,2},{join,swapped,2}}};
        OK(xr_lir_set_terminator(m,entry,&select));
    }
    XrLirValue args[]={parameters[0],ca_constant(&builder,join,0)};
    XrLirValue result=ca_intrinsic(&builder,join,XR_LIR_ARRAY_OP_GET,XR_LIR_I64,args,2);
    if (!stationary) {
        args[0]=parameters[1];
        XrLirValue next=ca_intrinsic(&builder,join,XR_LIR_ARRAY_OP_GET,XR_LIR_I64,args,2);
        XrLirValue scaled=ca_binary(&builder,join,XR_LIR_MUL,result,ca_constant(&builder,join,100));
        result=ca_binary(&builder,join,XR_LIR_ADD,scaled,next);
    }
    for(uint32_t p=0;p<2;++p)ca_effect(&builder,join,XR_LIR_ARRAY_DROP,parameters[p]);
    ca_return(&builder,join,result);OK(xr_lir_module_finish(m,NULL));
    XrRbcProgram *program=NULL;
    CHECK(xr_rbc_compile(m,f,&program,NULL)==XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(m);
    const XrRbcModule *module=xr_rbc_program_view(program);
    const XrLirValueHomes *homes=xr_rbc_module_value_homes(module,0);
    CHECK(homes);
    if (stationary) {
        CHECK(!homes->edge_count);
        for (uint32_t p=0;p<2;++p) {
            const XrLirValueHome *from=&homes->values[values[p].id],*to=&homes->values[parameters[p].id];
            CHECK(from->stationary_owner==values[p].id && to->stationary_owner==values[p].id);
            CHECK(from->first_owner<homes->frame.slot_count && from->first_owner==to->first_owner);
        }
        CHECK(homes->values[values[0].id].first_owner!=homes->values[values[1].id].first_owner);
    } else {
        CHECK(homes->edge_count==2);
        for (uint32_t edge=0;edge<2;++edge) {
            const XrLirHomeEdge *selected=&homes->edges[edge];
            CHECK(selected->count==2 && !selected->direct_header_move);
            for (size_t transfer=0;transfer<selected->count;++transfer)
                CHECK(homes->transfers[selected->first+transfer].transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_MOVE);
        }
        CHECK(homes->frame.slot_count>=4);
    }
    for(unsigned repeat=0;repeat<2;++repeat) {
        uint64_t flag_value=repeat,actual=0;
        uint64_t expected=stationary?7:(flag_value?711:1107);
        CHECK(execute(program,stationary?NULL:&flag_value,stationary?0:1,&actual)==XR_RBC_OK && actual==expected);
    }
    xr_rbc_program_destroy(program);CHECK(!live_allocations);
}
