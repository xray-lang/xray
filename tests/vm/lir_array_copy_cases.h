/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_array_copy_cases.h - Selected-edge parallel copies retain one real owner
 */
static void physical_array_copy_cycle(void) {
    CArrayFixture builder={.element=XR_LIR_I64};
    OK(xr_lir_array_module_create(NULL,&builder.module));
    XrLirModule *m=builder.module;
    XrLirFunction f=test_function(m,"array_copy",NULL,0,XR_LIR_I64);
    XrLirType types[]={XR_LIR_ARRAY,XR_LIR_ARRAY};
    XrLirBlock entry=test_block(m,f,NULL,0),join=test_block(m,f,types,2);
    OK(xr_lir_set_entry(m,f,entry));
    XrLirValue values[2],parameters[2];
    for(uint32_t p=0;p<2;++p) {
        XrLirValue number=ca_constant(&builder,entry,p?11:7);
        values[p]=ca_intrinsic(&builder,entry,XR_LIR_ARRAY_OP_NEW,XR_LIR_ARRAY,&number,1);
        OK(xr_lir_block_parameter(m,join,p,&parameters[p]));
        XrLirArrayShape shape={XR_LIR_I64};OK(xr_lir_set_array_shape(m,parameters[p],&shape));
    }
    test_branch(m,entry,join,values,2);
    XrLirValue args[]={parameters[0],ca_constant(&builder,join,0)};
    XrLirValue first=ca_intrinsic(&builder,join,XR_LIR_ARRAY_OP_GET,XR_LIR_I64,args,2);
    for(uint32_t p=0;p<2;++p)ca_effect(&builder,join,XR_LIR_ARRAY_DROP,parameters[p]);
    ca_return(&builder,join,first);OK(xr_lir_module_finish(m,NULL));
    XrLirFunctionView view;OK(xr_lir_function_view(m,f,&view));
    XrRbcCompileDiagnostic diagnostic={0};
    RbcLirCompiler c={.input=m,.function=f,.view=view,.diagnostic=&diagnostic,.typed_spills=true,
        .slot_count=2,.temporary_arrays={1,0},.scratch_count=1};
    c.slots=xr_calloc((size_t)view.value_count+1,sizeof(*c.slots));CHECK(c.slots);
    c.slots[values[0].id]=1;c.slots[values[1].id]=0;
    c.slots[parameters[0].id]=0;c.slots[parameters[1].id]=1;
    CHECK(rbc_lir_constant(&c,3,7) && rbc_lir_constant(&c,4,11));
    XrRbcArrayLocation operand={3,false};
    XrRbcArrayAccess row={.operation=XR_RBC_ARRAY_NEW,.kind=XR_RBC_ARRAY_I64,.result={1,false},.count=1};
    CHECK(rbc_lir_array_append(&c,row,&operand));
    row.result.index=0;operand.index=4;CHECK(rbc_lir_array_append(&c,row,&operand));
    uint32_t actuals[]={values[0].id,values[1].id};bool scratch=false;
    CHECK(rbc_lir_copies(&c,(XrLirEdgeView){join.id,actuals,2},true,&scratch));
    CHECK(scratch && c.array_scratch[0] && c.array_access_count==5);
    CHECK(rbc_lir_constant(&c,3,0));
    XrRbcArrayLocation get[]={{0,false},{3,false}};
    row=(XrRbcArrayAccess){.operation=XR_RBC_ARRAY_GET,.kind=XR_RBC_ARRAY_I64,.result={4,false},.count=2};
    CHECK(rbc_lir_array_append(&c,row,get));
    row=(XrRbcArrayAccess){.operation=XR_RBC_ARRAY_DROP,.kind=XR_RBC_ARRAY_I64,.count=1};
    for(uint32_t p=0;p<2;++p){operand.index=p;CHECK(rbc_lir_array_append(&c,row,&operand));}
    CHECK(rbc_lir_emit(&c,XR_RBC_RET,4,0,0,0));
    XrRbcRegisterKind kinds[]={XR_RBC_ARRAY_I64,XR_RBC_ARRAY_I64,XR_RBC_ARRAY_I64,XR_RBC_BITS,XR_RBC_BITS};
    XrRbcFunction function={.code=c.code,.code_count=c.code_count,.register_count=5,
        .register_kinds=kinds,.register_kind_count=5,.array_accesses=c.array_accesses,.array_access_count=c.array_access_count,
        .array_operands=c.array_operands,.array_operand_count=c.array_operand_count};
    XrRbcModule module={.functions=&function,.function_count=1};
    XrtLocationOwner *locations=NULL;CHECK(xrt_locations_create(NULL,0,&locations)==XRT_LOCATION_OK);
    XrRbcExecution *execution=NULL;
    CHECK(xr_rbc_module_prepare(&module,0,NULL,0,locations,&execution)==XR_RBC_OK);
    XrRbcOutcome *outcome=xr_rbc_execution_run(execution);uint64_t result=0;
    CHECK(xr_rbc_outcome_bits(outcome,&result) && result==7);
    xr_rbc_execution_free(&execution);xr_rbc_outcome_free(&outcome);xrt_locations_release(locations);
    xr_free(c.slots);xr_free(c.code);xr_free(c.source_ids);xr_free(c.array_accesses);xr_free(c.array_operands);
    xr_lir_module_destroy(m);CHECK(!live_allocations);
}
