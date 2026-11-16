/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static void callable_cell_c_fixture(XrLirModule **output, XrLirFunction *root, bool map, bool owned) {
    CArrayFixture c = {0}; OK(xr_lir_module_create(NULL,&c.module));
    if (map) OK(xr_lir_map_catalog_install(c.module,false,false,false,false,false));
    XrLirType payload = map ? XR_LIR_MAP_STRING_I64 : XR_LIR_I64, cell = 0, callable = 0, optional = 0;
    OK(xr_lir_cell_type(c.module,(XrLirCellType){payload,0},&cell));
    if (map) OK(xr_lir_nullable_type(c.module,XR_LIR_I64,&optional));
    OK(xr_lir_callable_type(c.module,&(XrLirCallableType){.result=XR_LIR_I64},&callable));
    XrLirFunction worker = {0};
    OK(xr_lir_declare_function(c.module,&(XrLirFunctionSpec){.name="cell_worker",.name_length=11,
        .parameters=&cell,.parameter_count=1,.result=XR_LIR_I64},&worker));
    OK(xr_lir_set_closure_target(c.module,worker,(XrLirClosureTarget){callable,1,false}));
    OK(xr_lir_define_function(c.module,worker));
    XrLirBlock entry = {0}, join = {0}; XrLirValue formal = {0};
    OK(xr_lir_add_block(c.module,worker,NULL,0,&entry)); OK(xr_lir_set_entry(c.module,worker,entry));
    XrLirType types[] = {cell,cell}; OK(xr_lir_add_block(c.module,worker,types,2,&join));
    OK(xr_lir_function_parameter(c.module,worker,0,&formal));
    XrLirValue outer = callable_fixture_scope(&c,entry,(XrLirValue){0}), inner = callable_fixture_scope(&c,entry,outer);
    XrLirValue bind[] = {outer,formal}, bound = ca_value(&c,entry,XR_LIR_FUNCTION_BIND,cell,bind,2);
    XrLirValue kept = callable_fixture_leave(&c,entry,inner,formal,cell);
    XrLirValue arguments[] = {formal,formal};
    XrLirInstruction branch = {.op=XR_LIR_BR,.type=XR_LIR_UNIT,.edge_count=1,.edges={{join,arguments,2}}};
    OK(xr_lir_set_terminator(c.module,entry,&branch));
    XrLirValue first = {0}, second = {0};
    OK(xr_lir_block_parameter(c.module,join,0,&first)); OK(xr_lir_block_parameter(c.module,join,1,&second));
    XrLirValue answer = {0};
    if (map) {
        XrLirValue replacement = map_fixture_call(&c,join,XR_LIR_MAP_OP_NEW,payload,NULL,0), write[] = {formal,replacement};
        OK(xr_lir_emit_effect(c.module,join,&(XrLirInstruction){.op=XR_LIR_CELL_WRITE,.type=XR_LIR_UNIT,.operands=write,.operand_count=2,.source=ca_source()}));
        ca_effect(&c,join,XR_LIR_MAP_DROP,replacement);
        XrLirValue reference = ca_value(&c,join,XR_LIR_MAP_BORROW,XR_LIR_MAP_STRING_I64_REF,&first,1);
        XrLirValue key = ca_text(&c,join,"answer",6), set[] = {reference,key,ca_constant(&c,join,42)};
        (void)map_fixture_call(&c,join,XR_LIR_MAP_OP_SET,XR_LIR_UNIT,set,3);
        XrLirValue child = ca_value(&c,join,XR_LIR_MAP_REF_READ,payload,&reference,1);
        ca_effect(&c,join,XR_LIR_MAP_DROP,child);
        child = ca_value(&c,join,XR_LIR_CELL_READ,payload,&formal,1);
        XrLirValue get[] = {child,key};
        XrLirValue found = map_fixture_call(&c,join,XR_LIR_MAP_OP_GET,optional,get,2);
        answer = ca_value(&c,join,XR_LIR_NULLABLE_UNWRAP,XR_LIR_I64,&found,1);
        ca_effect(&c,join,XR_LIR_MAP_DROP,child); ca_effect(&c,join,XR_LIR_STRING_DROP,key);
    } else {
        XrLirValue write[] = {first,ca_constant(&c,join,41)};
        OK(xr_lir_emit_effect(c.module,join,&(XrLirInstruction){.op=XR_LIR_CELL_WRITE,.type=XR_LIR_UNIT,.operands=write,.operand_count=2,.source=ca_source()}));
        XrLirValue original = ca_value(&c,join,XR_LIR_CELL_READ,payload,&formal,1);
        XrLirValue received = ca_value(&c,join,XR_LIR_CELL_READ,payload,&second,1);
        answer = ca_binary(&c,join,XR_LIR_SUB,ca_binary(&c,join,XR_LIR_ADD,original,received),ca_constant(&c,join,40));
    }
    XrLirValue copy = ca_value(&c,join,XR_LIR_VALUE_RETAIN,cell,&bound,1);
    ca_effect(&c,join,XR_LIR_VALUE_DROP,copy);
    (void)kept;
    (void)callable_fixture_leave(&c,join,outer,(XrLirValue){0},XR_LIR_UNIT); ca_return(&c,join,answer);
    OK(xr_lir_declare_function(c.module,&(XrLirFunctionSpec){.name="root",.name_length=4,.result=XR_LIR_I64},root));
    OK(xr_lir_define_function(c.module,*root));
    OK(xr_lir_add_block(c.module,*root,NULL,0,&entry)); OK(xr_lir_set_entry(c.module,*root,entry));
    XrLirValue seed = map ? map_fixture_call(&c,entry,XR_LIR_MAP_OP_NEW,payload,NULL,0) : ca_constant(&c,entry,0);
    XrLirValue identity = ca_value(&c,entry,XR_LIR_CELL_NEW,cell,&seed,1), function = {0};
    if (map) ca_effect(&c,entry,XR_LIR_MAP_DROP,seed);
    OK(xr_lir_emit_value(c.module,entry,&(XrLirInstruction){.op=XR_LIR_FUNCTION_REF,.type=callable,.callee=worker,
        .operands=&identity,.operand_count=1,.bits=owned ? XR_LIR_CLOSURE_OWNED : XR_LIR_CLOSURE_FRAME,.source=ca_source()},&function));
    answer = ca_value(&c,entry,XR_LIR_CALL_INDIRECT,XR_LIR_I64,&function,1);
    ca_return(&c,entry,answer); OK(xr_lir_module_finish(c.module,NULL)); *output = c.module;
}
