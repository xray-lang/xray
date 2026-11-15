/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_array_copy_cases.h - Selected-edge parallel copies retain one real owner
 */
static void compiled_array_edge_owners(void) {
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
    XrRbcProgram *program=NULL;
    CHECK(xr_rbc_compile(m,f,&program,NULL)==XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(m);
    const XrRbcModule *module=xr_rbc_program_view(program);
    const XrLirValueHomes *homes=xr_rbc_module_value_homes(module,0);
    CHECK(homes && homes->edge_count==1 && homes->edges[0].count==2);
    CHECK(homes->frame.slot_count>=4);
    for(unsigned repeat=0;repeat<2;++repeat) {
        uint64_t result=0;CHECK(execute(program,NULL,0,&result)==XR_RBC_OK && result==7);
    }
    xr_rbc_program_destroy(program);CHECK(!live_allocations);
}
