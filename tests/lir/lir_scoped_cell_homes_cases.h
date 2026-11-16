/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "lir/xlir_verify.h"

typedef struct ScopedCellFixture {
    CArrayFixture c;
    XrLirType cell, payload;
    XrLirFunction function, target;
    XrLirValue source, first, second, bound, kept, reference;
} ScopedCellFixture;
static XrLirValue scoped_cell_read(ScopedCellFixture *f, XrLirBlock block, XrLirValue cell) {
    XrLirValue result = ca_value(&f->c,block,XR_LIR_CELL_READ,f->payload,&cell,1);
    if (f->payload == XR_LIR_MAP_STRING_I64) ca_effect(&f->c,block,XR_LIR_MAP_DROP,result);
    return result;
}
static ScopedCellFixture scoped_cell_fixture(bool map, unsigned attack) {
    ScopedCellFixture f = {.payload=map ? XR_LIR_MAP_STRING_I64 : XR_LIR_I64};
    OK(xr_lir_module_create(NULL,&f.c.module));
    if (map) OK(xr_lir_map_catalog_install(f.c.module,false,false,false,false,false));
    OK(xr_lir_cell_type(f.c.module,(XrLirCellType){f.payload,0},&f.cell));
    XrLirType signature = 0;
    OK(xr_lir_callable_type(f.c.module,&(XrLirCallableType){.result=XR_LIR_I64},&signature));
    OK(xr_lir_declare_function(f.c.module,&(XrLirFunctionSpec){.name="borrow_cell",.name_length=11,
        .parameters=&f.cell,.parameter_count=1,.result=XR_LIR_I64},&f.target));
    OK(xr_lir_set_closure_target(f.c.module,f.target,(XrLirClosureTarget){signature,1,false}));
    OK(xr_lir_define_function(f.c.module,f.target));
    XrLirBlock body = {0}; XrLirValue formal = {0};
    OK(xr_lir_add_block(f.c.module,f.target,NULL,0,&body)); OK(xr_lir_set_entry(f.c.module,f.target,body));
    OK(xr_lir_function_parameter(f.c.module,f.target,0,&formal));
    XrLirValue inner = callable_fixture_scope(&f.c,body,(XrLirValue){0});
    XrLirValue kept = callable_fixture_leave(&f.c,body,inner,formal,f.cell);
    (void)scoped_cell_read(&f,body,kept);
    XrLirBlock formal_join = {0}; XrLirType joined_types[] = {f.cell,f.cell};
    OK(xr_lir_add_block(f.c.module,f.target,joined_types,2,&formal_join));
    XrLirValue incoming[] = {formal,formal};
    XrLirInstruction formal_jump = {.op=XR_LIR_BR,.type=XR_LIR_UNIT,.edge_count=1,
        .edges={{.target=formal_join,.arguments=incoming,.argument_count=2}}};
    OK(xr_lir_set_terminator(f.c.module,body,&formal_jump));
    XrLirValue first_formal = {0}, second_formal = {0};
    OK(xr_lir_block_parameter(f.c.module,formal_join,0,&first_formal));
    OK(xr_lir_block_parameter(f.c.module,formal_join,1,&second_formal));
    (void)scoped_cell_read(&f,formal_join,formal);
    (void)scoped_cell_read(&f,formal_join,first_formal);
    XrLirValue answer = scoped_cell_read(&f,formal_join,second_formal);
    if (map) answer = ca_constant(&f.c,formal_join,42);
    ca_return(&f.c,formal_join,answer);
    OK(xr_lir_declare_function(f.c.module,&(XrLirFunctionSpec){.name="cells",.name_length=5,.result=XR_LIR_I64},&f.function));
    OK(xr_lir_define_function(f.c.module,f.function));
    XrLirBlock entry = {0}, join = {0};
    OK(xr_lir_add_block(f.c.module,f.function,NULL,0,&entry)); OK(xr_lir_set_entry(f.c.module,f.function,entry));
    XrLirType parameters[] = {f.cell,f.cell};
    OK(xr_lir_add_block(f.c.module,f.function,parameters,2,&join));
    OK(xr_lir_block_parameter(f.c.module,join,0,&f.first)); OK(xr_lir_block_parameter(f.c.module,join,1,&f.second));
    XrLirValue outer = callable_fixture_scope(&f.c,entry,(XrLirValue){0});
    /* An outer source stays lexically visible after leaving the inner scope.
     * Only its explicit KEEP move can make the original handle unreadable. */
    bool outer_source = attack == 4 || attack == 5;
    if (!outer_source) inner = callable_fixture_scope(&f.c,entry,outer);
    XrLirValue seed = map ? role(&f.c,entry,XR_LIR_MAP_OP_NEW,XR_LIR_MAP_STRING_I64,NULL,0) : ca_constant(&f.c,entry,42);
    f.source = ca_value(&f.c,entry,XR_LIR_CELL_NEW,f.cell,&seed,1);
    if (map) ca_effect(&f.c,entry,XR_LIR_MAP_DROP,seed);
    if (outer_source) inner = callable_fixture_scope(&f.c,entry,outer);
    if (attack == 1) ca_effect(&f.c,entry,XR_LIR_VALUE_DROP,f.source);
    XrLirValue arguments[] = {f.source,f.source};
    XrLirInstruction jump = {.op=XR_LIR_BR,.type=XR_LIR_UNIT,.edge_count=1,
        .edges={{.target=join,.arguments=arguments,.argument_count=2}}};
    OK(xr_lir_set_terminator(f.c.module,entry,&jump));
    (void)scoped_cell_read(&f,join,f.source);
    (void)scoped_cell_read(&f,join,f.first); (void)scoped_cell_read(&f,join,f.second);
    XrLirValue replacement = map ? role(&f.c,join,XR_LIR_MAP_OP_NEW,XR_LIR_MAP_STRING_I64,NULL,0) : ca_constant(&f.c,join,43);
    XrLirValue write[] = {f.first,replacement};
    OK(xr_lir_emit_effect(f.c.module,join,&(XrLirInstruction){.op=XR_LIR_CELL_WRITE,.type=XR_LIR_UNIT,.operands=write,.operand_count=2}));
    if (map) {
        ca_effect(&f.c,join,XR_LIR_MAP_DROP,replacement);
        f.reference = ca_value(&f.c,join,XR_LIR_MAP_BORROW,XR_LIR_MAP_STRING_I64_REF,&f.first,1);
        if (attack == 3) ca_effect(&f.c,join,XR_LIR_VALUE_DROP,f.first);
        XrLirValue key = ca_text(&f.c,join,"x",1), value = ca_constant(&f.c,join,7), args[] = {f.reference,key,value};
        (void)role(&f.c,join,XR_LIR_MAP_OP_SET,XR_LIR_UNIT,args,3); ca_effect(&f.c,join,XR_LIR_STRING_DROP,key);
    }
    XrLirValue bind[] = {outer,f.second};
    OK(xr_lir_emit_value(f.c.module,join,&(XrLirInstruction){.op=XR_LIR_FUNCTION_BIND,.type=f.cell,
        .operands=bind,.operand_count=2,.bits=77},&f.bound));
    f.kept = callable_fixture_leave(&f.c,join,inner,f.source,f.cell);
    if (attack == 2 || attack == 4 || attack == 5 || attack == 6)
        (void)scoped_cell_read(&f,join,attack == 2 || attack == 4 ? f.source : f.kept);
    answer = scoped_cell_read(&f,join,f.kept); (void)scoped_cell_read(&f,join,f.bound);
    if (map) answer = ca_constant(&f.c,join,42);
    (void)callable_fixture_leave(&f.c,join,outer,(XrLirValue){0},XR_LIR_UNIT);
    ca_return(&f.c,join,answer);
    return f;
}
static void scoped_cell_projection(bool map) {
    ScopedCellFixture f = scoped_cell_fixture(map,0); OK(xr_lir_module_finish(f.c.module,NULL));
    XrtBillingOwner *billing = NULL; MANAGED(xrt_billing_owner_new(NULL,SIZE_MAX,&billing));
    XrLirValueLayoutKey roots[] = {{f.cell,0,0},{XR_LIR_I64,0,0},{XR_LIR_UNIT,0,0},{XR_LIR_STRING,0,0}};
    XrLirValueLayouts *set = NULL;
    HOME(xr_lir_value_layouts_create(f.c.module,roots,4,billing,&set));
    const XrLirOwnershipTypeFact *types = NULL; uint32_t type_count = 0;
    HOME(xr_lir_value_layouts_ownership(set,&types,&type_count)); CHECK(type_count == 1 && types[0].has_owned);
    XrLirOwnershipPlan *plan = NULL;
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){f.c.module,types,type_count},&plan,NULL));
    XrLirValueHomes *homes = NULL, *formals = NULL;
    XrLirValueHomesRequest request = {f.c.module,f.function,set,plan};
    attempts = 0; HOME(xr_lir_value_homes_create(&request,&homes)); size_t total = attempts;
    xr_lir_value_homes_destroy(homes); homes = NULL; CHECK(!live);
    for (size_t n = 1; n <= total; ++n) {
        attempts = 0; fail_at = n;
        CHECK(xr_lir_value_homes_create(&request,&homes) == XR_LIR_VALUE_LAYOUT_OOM && !homes && !live);
    }
    fail_at = SIZE_MAX; HOME(xr_lir_value_homes_create(&request,&homes));
    HOME(xr_lir_value_homes_create(&(XrLirValueHomesRequest){f.c.module,f.target,set,plan},&formals));
    CHECK(!(homes->unmodeled & XR_LIR_OWNERSHIP_UNMODELED_CELL) && homes->transfer_count == 2 && homes->values[f.source.id].owner_count == 1);
    CHECK(homes->transfers[0].source == f.source.id && homes->transfers[1].source == f.source.id);
    for (size_t t = 0; t < 2; ++t) CHECK(homes->transfers[t].transfer_mode == XR_LIR_OWNERSHIP_TRANSFER_COPY);
    const XrLirValueHome *formal = &formals->values[1];
    CHECK(formal->parameter == 1 && formal->cell_transport == XR_LIR_CELL_TRANSPORT_BORROW && !formal->owner_count &&
        formal->offset == SIZE_MAX && formal->first_owner == SIZE_MAX && formal->temporary_offset == SIZE_MAX &&
        formal->temporary_first_owner == SIZE_MAX && formal->byte_count == 1 &&
        formals->bytes[formal->first_bytes].offset == 0 && formals->bytes[formal->first_bytes].size == sizeof(void *));
    CHECK(formals->transfer_count == 2);
    for (size_t n = 0; n < formals->transfer_count; ++n) {
        const XrLirHomeTransfer *edge = &formals->transfers[n];
        CHECK(edge->source == 1 && edge->transfer_mode == XR_LIR_OWNERSHIP_TRANSFER_COPY &&
            formals->values[edge->target].cell_transport == XR_LIR_CELL_TRANSPORT_OWNED &&
            formals->values[edge->target].owner_count == 1 && formals->values[edge->target].temporary_first_owner != SIZE_MAX);
    }
    bool bound = false, kept = false, borrowed_keep = false;
    for (size_t n = 0; n < homes->event_count; ++n) {
        const XrLirHomeEvent *e = &homes->events[n];
        if (e->kind != XR_LIR_OWNERSHIP_CELL_VALUE) continue;
        CHECK(!e->callable_transport && e->cell_type && e->cell_payload_type && e->cell_transport == XR_LIR_CELL_TRANSPORT_OWNED);
        if (e->source == f.bound.id) { CHECK(e->transfer_mode == XR_LIR_OWNERSHIP_TRANSFER_COPY); bound = true; }
        if (e->source == f.kept.id) { CHECK(e->transfer_mode == XR_LIR_OWNERSHIP_TRANSFER_MOVE); kept = true; }
    }
    for (size_t n = 0; n < formals->event_count; ++n) {
        const XrLirHomeEvent *e = &formals->events[n];
        if (e->kind == XR_LIR_OWNERSHIP_CELL_VALUE && e->cell_operation == XR_LIR_CELL_OP_KEEP) {
            CHECK(e->transfer_mode == XR_LIR_OWNERSHIP_TRANSFER_COPY); borrowed_keep = true;
        }
    }
    CHECK(bound && kept && borrowed_keep);
    if (map) {
        const XrLirHomeBinding *ref = &homes->bindings[f.reference.id];
        CHECK(ref->kind == XR_LIR_HOME_BINDING_CELL_PAYLOAD_BORROW && ref->source == f.first.id &&
            ref->type == homes->values[f.first.id].cell_payload_type && ref->offset == SIZE_MAX && ref->first_owner == SIZE_MAX);
    }
    xr_lir_module_destroy(f.c.module); xr_lir_ownership_plan_destroy(plan); xr_lir_value_layouts_destroy(set);
    CHECK(homes->values[f.kept.id].cell_transport == XR_LIR_CELL_TRANSPORT_OWNED && homes->values[f.kept.id].temporary_first_owner != SIZE_MAX);
    xr_lir_value_homes_destroy(formals); xr_lir_value_homes_destroy(homes); CHECK(!live);
    CHECK(xrt_billing_owner_stats(billing).blocks == 1); xrt_billing_owner_drop(billing);
}
static void scoped_cell_capture_lifetimes(void) {
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        CArrayFixture c = {0}; XrLirType cell = 0, callable = 0;
        OK(xr_lir_module_create(NULL,&c.module));
        OK(xr_lir_cell_type(c.module,(XrLirCellType){XR_LIR_I64,0},&cell));
        OK(xr_lir_callable_type(c.module,&(XrLirCallableType){.result=XR_LIR_I64},&callable));
        XrLirFunction target = {0}, root = {0}; XrLirBlock block = {0}; XrLirValue formal = {0};
        OK(xr_lir_declare_function(c.module,&(XrLirFunctionSpec){.name="read",.name_length=4,
            .parameters=&cell,.parameter_count=1,.result=XR_LIR_I64},&target));
        OK(xr_lir_set_closure_target(c.module,target,(XrLirClosureTarget){callable,1,false}));
        OK(xr_lir_define_function(c.module,target));
        OK(xr_lir_add_block(c.module,target,NULL,0,&block)); OK(xr_lir_set_entry(c.module,target,block));
        OK(xr_lir_function_parameter(c.module,target,0,&formal));
        if (scenario == 2) ca_effect(&c,block,XR_LIR_VALUE_DROP,formal);
        ca_return(&c,block,ca_value(&c,block,XR_LIR_CELL_READ,XR_LIR_I64,&formal,1));
        OK(xr_lir_declare_function(c.module,&(XrLirFunctionSpec){.name="root",.name_length=4,.result=XR_LIR_I64},&root));
        OK(xr_lir_define_function(c.module,root));
        OK(xr_lir_add_block(c.module,root,NULL,0,&block)); OK(xr_lir_set_entry(c.module,root,block));
        XrLirValue seed = ca_constant(&c,block,42), local = ca_value(&c,block,XR_LIR_CELL_NEW,cell,&seed,1), function = {0};
        OK(xr_lir_emit_value(c.module,block,&(XrLirInstruction){.op=XR_LIR_FUNCTION_REF,.type=callable,
            .callee=target,.operands=&local,.operand_count=1,.bits=scenario == 1 ? XR_LIR_CLOSURE_OWNED : XR_LIR_CLOSURE_FRAME},&function));
        ca_effect(&c,block,XR_LIR_VALUE_DROP,local);
        ca_return(&c,block,ca_value(&c,block,XR_LIR_CALL_INDIRECT,XR_LIR_I64,&function,1));
        XrLirDiagnostic diagnostic; XrLirStatus status = xr_lir_module_finish(c.module,&diagnostic);
        if (scenario == 1) OK(status);
        else CHECK(status == XR_LIR_VERIFY_ERROR && !strcmp(diagnostic.message,scenario == 2 ?
            "borrowed Cell formal cannot be released" : "Cell use follows release or precedes ownership"));
        xr_lir_module_destroy(c.module);
    }
}
static void scoped_cells(void) {
    scoped_cell_projection(false); scoped_cell_projection(true);
    scoped_cell_capture_lifetimes();
    /* Each control has the same source placement and instruction sequence as
     * its rejected case; only the post-KEEP read selects the receiving value. */
    for (unsigned control = 5; control < 7; ++control) {
        ScopedCellFixture f = scoped_cell_fixture(false,control);
        OK(xr_lir_module_finish(f.c.module,NULL)); xr_lir_module_destroy(f.c.module);
    }
    for (unsigned attack = 1; attack < 5; ++attack) {
        ScopedCellFixture f = scoped_cell_fixture(attack == 3,attack); XrLirDiagnostic diagnostic;
        XrLirStatus status = xr_lir_module_finish(f.c.module,&diagnostic);
        const char *expected = attack == 1 ? "Cell edge follows release or precedes ownership" : attack == 2 ?
            "function scope parent or selected-edge lifetime is inconsistent" : "Cell use follows release or precedes ownership";
        if (status != XR_LIR_VERIFY_ERROR || strcmp(diagnostic.message,expected))
            fprintf(stderr,"Cell lifetime attack=%u status=%u function=%u block=%u instruction=%u: %s; expected: %s\n",
                attack,(unsigned)status,diagnostic.function,diagnostic.block,diagnostic.instruction,
                diagnostic.message,expected);
        CHECK(status == XR_LIR_VERIFY_ERROR && !strcmp(diagnostic.message,expected));
        xr_lir_module_destroy(f.c.module);
    }
}
