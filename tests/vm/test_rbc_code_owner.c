/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_rbc_code_owner.c - Executable code leaves and independently retained layouts
 *
 * KEY CONCEPT:
 *   Real compiled programs run after their shells are freed. Allocation failure
 *   sweeps include both unsealed compilation and published image preparations.
 */
#include "../lir/lir_test_programs.h"
#include "base/xmalloc.h"
#include "vm/register/xrbc_lir.h"
#include "xray_native_string_value.h"
#include "xray_native_array_local.h"
#include "runtime/native/xnative_array_ops.h"

static size_t region_begin_attempts,region_begin_successes;
static bool region_force_bypass;
static size_t array_pushes,array_push_transactions,array_push_fault;
static size_t failed_array_length;
static int64_t array_push_item_bias;
static bool counted_region_begin(const XrtArrayI64RegionContext *context,const XrtArrayI64RegionSpec *spec,
    bool ready,XrtArrayI64RegionCache *cache) {
    ++region_begin_attempts;
    CHECK(ready && !cache->owner && !cache->elements && !cache->length);
    if (region_force_bypass) return false;
    bool result=xrt_array_i64_region_try_begin(context,spec,ready,cache);
    if (result) ++region_begin_successes;
    return result;
}
static bool counted_array_local_update(const XrtArrayI64LocalView *view,XrtArrayI64LocalUpdate update) {
    if (update.operation==XRT_ARRAY_UPDATE_PUSH) ++array_pushes;
    return xrt_array_i64_try_update_local(view,update);
}

static size_t attempts, fail_at = SIZE_MAX, tracked_live;
static void *watched_program, *watched_layout;
static bool program_freed, layout_freed;
static struct { void *pointer; size_t bytes; } tracked[8192];
static size_t allocation_index(void *pointer) {
    for (size_t i = 0; i < 8192; ++i) if (tracked[i].pointer == pointer) return i;
    CHECK(false); return 0;
}
static void *allocate(size_t bytes) {
    if (++attempts == fail_at) return NULL;
    void *memory = xr_malloc(bytes);
    if (memory) {
        size_t index = allocation_index(NULL);
        tracked[index].pointer = memory; tracked[index].bytes = bytes; ++tracked_live;
    }
    return memory;
}
static void *allocate_zero(size_t count, size_t bytes) {
    if (bytes && count > SIZE_MAX / bytes) return NULL;
    void *memory = allocate(count * bytes);
    if (memory) memset(memory, 0, count * bytes);
    return memory;
}
static void release(void *memory) {
    if (!memory) return;
    size_t index = allocation_index(memory);
    tracked[index].pointer = NULL; tracked[index].bytes = 0; CHECK(tracked_live); --tracked_live;
    if (memory == watched_program) program_freed = true;
    if (memory == watched_layout) layout_freed = true;
    xr_free(memory);
}
static void *resize(void *memory, size_t bytes) {
    if (++attempts == fail_at) return NULL;
    size_t index = allocation_index(memory);
    void *fresh = xr_malloc(bytes);
    if (!fresh) return NULL;
    if (memory) { memcpy(fresh, memory, tracked[index].bytes < bytes ? tracked[index].bytes : bytes); xr_free(memory); }
    else ++tracked_live;
    tracked[index].pointer = fresh; tracked[index].bytes = bytes;
    return fresh;
}
#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#define xr_malloc(bytes) allocate(bytes)
#define xr_calloc(count, bytes) allocate_zero(count, bytes)
#define xr_realloc(memory, bytes) resize(memory, bytes)
#define xr_free(memory) release(memory)
#include "runtime/native/xnative_managed.c"
#include "runtime/native/xnative_locations.c"
#include "runtime/native/xnative_module_state.c"
static XrtManagedStatus counted_array_update(XrtPlace binding,XrtBackingArrayKind kind,XrtManagedFrame *frame,
    size_t temporary,const XrtBackingTypes *types,const XrtArrayUpdate *update) {
    if (update->operation!=XRT_ARRAY_UPDATE_PUSH || ++array_push_transactions!=array_push_fault)
        return xrt_array_update_place(binding,kind,frame,temporary,types,update);
    XrtManagedSlotLocation source={0},workspace={0};
    CHECK(kind==XRT_BACKING_ARRAY_I64 && binding.frame==frame && !binding.external);
    CHECK(xrt_managed_slot_location(frame,binding.slot,&source)==XRT_MANAGED_OK &&
        xrt_managed_slot_location(frame,temporary,&workspace)==XRT_MANAGED_OK);
    XrtManagedHeader *owner=*source.owner;
    const XrtArrayBacking *array=(const XrtArrayBacking *)owner;
    CHECK(array && array->length==array->capacity && array->length==8 && !*workspace.owner);
    unsigned char original[8*sizeof(int64_t)];
    memcpy(original,array->elements,sizeof(original));
    for (size_t n=0;n<8;++n) {
        int64_t item;
        memcpy(&item,original+n*sizeof(item),sizeof(item));
        CHECK(item==(n ? (int64_t)n-1+array_push_item_bias : 7));
    }
    XrtBillingOwner *billing=owner->ops->billing;
    XrtBillingStats before=xrt_billing_owner_stats(billing);
    CHECK(fail_at==SIZE_MAX);
    fail_at=attempts+1;
    XrtManagedStatus status=xrt_array_update_place(binding,kind,frame,temporary,types,update);
    CHECK(attempts==fail_at);
    fail_at=SIZE_MAX;
    CHECK(status==XRT_MANAGED_OOM && *source.owner==owner && !*workspace.owner &&
        !source.pending->value && !source.pending->allocation_bytes &&
        !workspace.pending->value && !workspace.pending->allocation_bytes && !*source.busy &&
        array->length==8 && !memcmp(original,array->elements,sizeof(original)));
    XrtBillingStats after=xrt_billing_owner_stats(billing);
    CHECK(after.blocks==before.blocks && after.bytes==before.bytes);
    failed_array_length=array->length;
    return status;
}
#define xrt_array_i64_region_try_begin counted_region_begin
#define xrt_array_i64_try_update_local counted_array_local_update
#define xrt_array_update_place counted_array_update
#include "vm/register/xrbc.c"
#undef xrt_array_update_place
#undef xrt_array_i64_try_update_local
#undef xrt_array_i64_region_try_begin
#include "vm/register/xrbc_lir.c"
#include "../lir/lir_fixed_bytecode_fixture.h"
#include "../lir/lir_array_c_fixture.h"
#include "../lir/lir_array_push_window_fixture.h"
#include "../lir/lir_callable_fixture.h"
#include "../lir/lir_callable_bytecode_cases.h"

static XrLirValue cell_map_role(CArrayFixture *c,XrLirBlock block,XrLirMapOperation role,
    XrLirType result,const XrLirValue *arguments,uint32_t count) {
    uint32_t intrinsic=0;
    OK(xr_lir_map_operation(c->module,role,role==XR_LIR_MAP_OP_SET ? XR_LIR_MAP_STRING_I64_REF : XR_LIR_MAP_STRING_I64,&intrinsic));
    XrLirInstruction instruction={.op=XR_LIR_CALL_INTRINSIC,.type=result,.intrinsic=intrinsic,
        .operands=arguments,.operand_count=count};
    XrLirValue value={0};
    if (result==XR_LIR_UNIT) OK(xr_lir_emit_effect(c->module,block,&instruction));
    else OK(xr_lir_emit_value(c->module,block,&instruction,&value));
    return value;
}
static XrLirValue cell_map_index(CArrayFixture *c,XrLirBlock block,XrLirValue cell,XrLirValue key) {
    XrLirValue map=ca_value(c,block,XR_LIR_CELL_READ,XR_LIR_MAP_STRING_I64,&cell,1),arguments[]={map,key};
    XrLirValue result=cell_map_role(c,block,XR_LIR_MAP_OP_INDEX,XR_LIR_I64,arguments,2);
    ca_effect(c,block,XR_LIR_MAP_DROP,map);
    return result;
}
static XrLirValue cell_counter_body(CArrayFixture *c,XrLirFunction target,XrLirBlock block,
    XrLirType cell,XrLirValue formal,XrLirValue argument,bool map) {
    XrLirValue scope=ca_value(c,block,XR_LIR_FUNCTION_SCOPE_ENTER,XR_LIR_FUNCTION_SCOPE,NULL,0),keep[]={scope,formal};
    (void)ca_value(c,block,XR_LIR_FUNCTION_SCOPE_LEAVE,cell,keep,2);
    XrLirBlock join={0}; XrLirType types[]={cell,cell};
    OK(xr_lir_add_block(c->module,target,types,2,&join));
    XrLirValue incoming[]={formal,formal};
    XrLirInstruction jump={.op=XR_LIR_BR,.type=XR_LIR_UNIT,.edges={{join,incoming,2}},.edge_count=1};
    OK(xr_lir_set_terminator(c->module,block,&jump));
    XrLirValue first={0},second={0};
    OK(xr_lir_block_parameter(c->module,join,0,&first)); OK(xr_lir_block_parameter(c->module,join,1,&second));
    XrLirValue key={0};
    if (map) key=ca_text(c,join,"x",1);
    XrLirValue old=map ? cell_map_index(c,join,formal,key) : ca_value(c,join,XR_LIR_CELL_READ,XR_LIR_I64,&formal,1);
    XrLirValue next=ca_binary(c,join,XR_LIR_ADD,old,argument),replacement=next;
    if (map) {
        XrLirValue initial[]={key,ca_constant(c,join,41)};
        replacement=cell_map_role(c,join,XR_LIR_MAP_OP_NEW,XR_LIR_MAP_STRING_I64,initial,2);
    }
    XrLirValue write[]={first,replacement};
    OK(xr_lir_emit_effect(c->module,join,&(XrLirInstruction){.op=XR_LIR_CELL_WRITE,.type=XR_LIR_UNIT,.operands=write,.operand_count=2}));
    if (map) {
        ca_effect(c,join,XR_LIR_MAP_DROP,replacement);
        XrLirValue snapshot=ca_value(c,join,XR_LIR_CELL_READ,XR_LIR_MAP_STRING_I64,&formal,1);
        XrLirValue ref=ca_value(c,join,XR_LIR_MAP_BORROW,XR_LIR_MAP_STRING_I64_REF,&first,1),update[]={ref,key,next};
        (void)cell_map_role(c,join,XR_LIR_MAP_OP_SET,XR_LIR_UNIT,update,3);
        XrLirValue index[]={snapshot,key};
        old=cell_map_role(c,join,XR_LIR_MAP_OP_INDEX,XR_LIR_I64,index,2);
        ca_effect(c,join,XR_LIR_MAP_DROP,snapshot);
        next=cell_map_index(c,join,second,key);
        next=ca_binary(c,join,XR_LIR_SUB,ca_binary(c,join,XR_LIR_ADD,next,old),ca_constant(c,join,41));
        ca_effect(c,join,XR_LIR_STRING_DROP,key);
    } else next=ca_value(c,join,XR_LIR_CELL_READ,XR_LIR_I64,&second,1);
    ca_return(c,join,next);
    return next;
}
static XrLirModule *owned_fixture(XrLirFunction *root,unsigned capture) {
    CArrayFixture c={0};
    OK(xr_lir_module_create(NULL,&c.module));
    XrLirCallableParameter parameter={.type=XR_LIR_I64};
    XrLirCallableType signature={.parameters=&parameter,.parameter_count=1,.result=XR_LIR_I64};
    XrLirType callable=0,arguments[]={XR_LIR_I64,XR_LIR_I64};
    XrLirType cell=0;
    if (capture>=2) {
        if (capture==3) OK(xr_lir_map_catalog_install(c.module,false,false,false,false,false));
        OK(xr_lir_cell_type(c.module,(XrLirCellType){capture==3 ? XR_LIR_MAP_STRING_I64 : XR_LIR_I64,0},&cell));
        arguments[0]=cell;
    }
    OK(xr_lir_callable_type(c.module,&signature,&callable));
    XrLirFunction target=test_function(c.module,"captured_add",arguments,capture ? 2u : 1u,XR_LIR_I64);
    OK(xr_lir_set_closure_target(c.module,target,(XrLirClosureTarget){.callable=callable,.capture_count=capture ? 1u : 0u}));
    XrLirBlock block=test_block(c.module,target,NULL,0);
    OK(xr_lir_set_entry(c.module,target,block));
    XrLirValue seed={0},argument={0};
    if (capture) OK(xr_lir_function_parameter(c.module,target,0,&seed));
    else seed=ca_constant(&c,block,40);
    OK(xr_lir_function_parameter(c.module,target,capture ? 1u : 0u,&argument));
    if (capture>=2) (void)cell_counter_body(&c,target,block,cell,seed,argument,capture==3);
    else ca_return(&c,block,ca_binary(&c,block,XR_LIR_ADD,seed,argument));
    *root=test_function(c.module,"factory",NULL,0,callable);
    block=test_block(c.module,*root,NULL,0);
    OK(xr_lir_set_entry(c.module,*root,block));
    if (capture) seed=ca_constant(&c,block,40);
    if (capture>=2) {
        XrLirValue outer=ca_value(&c,block,XR_LIR_FUNCTION_SCOPE_ENTER,XR_LIR_FUNCTION_SCOPE,NULL,0);
        XrLirValue inner=ca_value(&c,block,XR_LIR_FUNCTION_SCOPE_ENTER,XR_LIR_FUNCTION_SCOPE,&outer,1);
        if (capture==3) {
            XrLirValue key=ca_text(&c,block,"x",1),initial[]={key,seed};
            seed=cell_map_role(&c,block,XR_LIR_MAP_OP_NEW,XR_LIR_MAP_STRING_I64,initial,2);
            ca_effect(&c,block,XR_LIR_STRING_DROP,key);
        }
        XrLirValue initial=seed;
        seed=ca_value(&c,block,XR_LIR_CELL_NEW,cell,&initial,1);
        if (capture==3) ca_effect(&c,block,XR_LIR_MAP_DROP,initial);
        XrLirValue bind[]={outer,seed}; seed=ca_value(&c,block,XR_LIR_FUNCTION_BIND,cell,bind,2);
        bind[0]=inner; bind[1]=seed; seed=ca_value(&c,block,XR_LIR_FUNCTION_SCOPE_LEAVE,cell,bind,2);
        bind[0]=outer; bind[1]=seed; seed=ca_value(&c,block,XR_LIR_FUNCTION_SCOPE_LEAVE,cell,bind,2);
    }
    XrLirInstruction instruction={.op=XR_LIR_FUNCTION_REF,.type=callable,.callee=target,
        .bits=XR_LIR_CLOSURE_OWNED,.operands=capture ? &seed : NULL,.operand_count=capture ? 1u : 0u};
    XrLirValue value={0};
    OK(xr_lir_emit_value(c.module,block,&instruction,&value));
    ca_return(&c,block,value);
    OK(xr_lir_module_finish(c.module,NULL));
    return c.module;
}
static XrLirModule *fixture(unsigned kind, XrLirFunction *root) {
    if (kind>=4) return owned_fixture(root,kind==4 ? 1u : kind==5 ? 0u : kind-4u);
    if (!kind) {
        XrLirModule *module = NULL;
        OK(xr_lir_module_create(NULL, &module));
        XrLirFunction print = test_print_declaration(module);
        *root = test_function(module, "native_leaf", NULL, 0, XR_LIR_I64);
        XrLirBlock block = test_block(module, *root, NULL, 0);
        OK(xr_lir_set_entry(module, *root, block));
        XrLirValue value = test_constant(module, block, XR_LIR_I64, 7);
        XrLirInstruction call = {.op=XR_LIR_CALL, .type=XR_LIR_UNIT, .callee=print,
            .operands=&value, .operand_count=1};
        OK(xr_lir_emit_effect(module, block, &call));
        test_return(module, block, value);
        OK(xr_lir_module_finish(module, NULL));
        return module;
    }
    if (kind == 1) {
        XrLirModule *module = NULL;
        /* Borrowed stack captures exercise code lifetime without a retired
         * heap-environment representation. */
        callable_fixture_capture(&module, root, 1);
        return module;
    }
    if (kind == 2) return fixed_bytecode_state_fixture(17, 0, root);
    XrLirModule *module = NULL;
    OK(xr_lir_module_create(NULL, &module));
    *root = test_function(module, "text", NULL, 0, XR_LIR_STRING);
    XrLirBlock block = test_block(module, *root, NULL, 0);
    OK(xr_lir_set_entry(module, *root, block));
    XrLirLiteral literal = {0};
    OK(xr_lir_add_literal(module, "\303\251\0x", 4, &literal));
    XrLirInstruction instruction = {.op=XR_LIR_STRING_LITERAL, .type=XR_LIR_STRING, .bits=literal.id};
    XrLirValue value;
    OK(xr_lir_emit_value(module, block, &instruction, &value));
    test_return(module, block, value);
    OK(xr_lir_module_finish(module, NULL));
    return module;
}
static XrRbcProgram *compile_fixture(unsigned kind) {
    XrLirFunction root;
    XrLirModule *module = fixture(kind, &root);
    XrRbcProgram *program = NULL;
    CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(module);
    return program;
}
static XrRbcProgram *raw_callable_program(void) {
    CArrayFixture c={0}; OK(xr_lir_module_create(NULL,&c.module));
    XrLirCallableParameter parameter={.type=XR_LIR_I64};
    XrLirCallableType signature={.parameters=&parameter,.parameter_count=1,.result=XR_LIR_I64};
    XrLirType callable=0,integer=XR_LIR_I64;
    OK(xr_lir_callable_type(c.module,&signature,&callable));
    XrLirFunction leaf_target=test_function(c.module,"leaf",&integer,1,integer);
    OK(xr_lir_set_closure_target(c.module,leaf_target,(XrLirClosureTarget){.callable=callable}));
    XrLirBlock block=test_block(c.module,leaf_target,NULL,0); OK(xr_lir_set_entry(c.module,leaf_target,block));
    XrLirValue input={0}; OK(xr_lir_function_parameter(c.module,leaf_target,0,&input));
    ca_return(&c,block,ca_binary(&c,block,XR_LIR_ADD,input,ca_constant(&c,block,1)));
    XrLirType parameters[]={integer,integer};
    XrLirFunction capture_target=test_function(c.module,"ignore_capture",parameters,2,integer);
    OK(xr_lir_set_closure_target(c.module,capture_target,(XrLirClosureTarget){.callable=callable,.capture_count=1}));
    block=test_block(c.module,capture_target,NULL,0); OK(xr_lir_set_entry(c.module,capture_target,block));
    OK(xr_lir_function_parameter(c.module,capture_target,1,&input)); ca_return(&c,block,input);
    XrLirFunction root=test_function(c.module,"root",NULL,0,integer);
    block=test_block(c.module,root,NULL,0); OK(xr_lir_set_entry(c.module,root,block));
    XrLirValue scope=ca_value(&c,block,XR_LIR_FUNCTION_SCOPE_ENTER,XR_LIR_FUNCTION_SCOPE,NULL,0),leaf={0},wrapper={0};
    XrLirInstruction reference={.op=XR_LIR_FUNCTION_REF,.type=callable,.callee=leaf_target};
    OK(xr_lir_emit_value(c.module,block,&reference,&leaf));
    XrLirValue captured=ca_constant(&c,block,0);
    reference.callee=capture_target; reference.operands=&captured; reference.operand_count=1;
    OK(xr_lir_emit_value(c.module,block,&reference,&wrapper));
    XrLirValue operands[]={scope,wrapper};
    XrLirValue alias=ca_value(&c,block,XR_LIR_FUNCTION_BIND,callable,operands,2);
    XrLirValue arguments[]={alias,ca_constant(&c,block,42)};
    XrLirValue result=ca_value(&c,block,XR_LIR_CALL_INDIRECT,integer,arguments,2);
    ca_effect(&c,block,XR_LIR_FUNCTION_SCOPE_LEAVE,scope); ca_return(&c,block,result);
    OK(xr_lir_module_finish(c.module,NULL));
    XrRbcProgram *program=NULL;
    CHECK(xr_rbc_compile(c.module,root,&program,NULL)==XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(c.module);
    return program;
}
/* Keep every upstream receiver valid and pin rejection to the one changed
 * callable instruction, rather than passing through another malformed field. */
static void raw_callable_rejection(CallableRawFixture *raw,const XrRbcProgram *program,uint32_t access) {
    CHECK(rbc_borrowed_values_admit(&raw->module)==XR_RBC_OK);
    CHECK(rbc_callable_metadata_valid(&raw->module) && rbc_callable_image_valid(&raw->module));
    CHECK(xr_rbc_state_layout_valid(&raw->module));
    size_t rejected=0;
    for (size_t f=0;f<raw->module.function_count;++f) {
        const XrRbcFunction *function=&raw->functions[f];
        CHECK(rbc_aggregate_metadata_valid(&raw->module,function) && rbc_aggregate_calls_valid(&raw->module,function));
        for (size_t pc=0;pc<function->code_count;++pc) {
            uint32_t word=function->code[pc];
            bool changed=!f && (word&255)==XR_RBC_VALUE_ACCESS && (word>>16)==access;
            CHECK(xr_rbc_instruction_valid(&raw->module,function,pc)==!changed);
            rejected+=changed;
        }
        CHECK(xr_rbc_strings_valid(&raw->module,function)==XR_RBC_OK);
    }
    CHECK(rejected==1);
    XrRbcExecution *execution=NULL;
    CHECK(xr_rbc_module_prepare(&raw->module,0,NULL,0,xr_rbc_program_locations(program),&execution)==XR_RBC_INVALID);
    CHECK(!execution);
}
static void raw_callable_exact_boundaries(void) {
    CHECK(!tracked_live);
    XrRbcProgram *program=raw_callable_program();
    CallableRawFixture raw=callable_raw_fixture(program);
    callable_raw_positive(&raw,program);
    callable_raw_reset(&raw);
    uint32_t leaf=UINT32_MAX,wrapper=UINT32_MAX,alias=UINT32_MAX;
    for (size_t n=0;n<raw.code.access_count;++n) {
        XrRbcValueAccess access=raw.accesses[n];
        if (access.operation==XR_RBC_VALUE_FUNCTION_NEW) {
            if (!access.count) leaf=(uint32_t)n;
            else wrapper=(uint32_t)n;
        } else if (access.operation==XR_RBC_VALUE_FUNCTION_ALIAS && access.other) alias=(uint32_t)n;
    }
    CHECK(leaf!=UINT32_MAX && wrapper!=UINT32_MAX && alias!=UINT32_MAX);
    uint32_t leaf_value=raw.accesses[leaf].result,wrapper_value=raw.accesses[wrapper].result;
    CHECK(raw.accesses[leaf].index!=raw.accesses[wrapper].index);
    CHECK(rbc_value_signature(&raw.module,&raw.code,leaf_value)==rbc_value_signature(&raw.module,&raw.code,wrapper_value));
    XrRbcValueAccess producer=raw.accesses[alias];
    CHECK(producer.source==wrapper_value && raw.values[producer.result].borrow_source==wrapper_value);
    raw.values[producer.result].borrow_source=leaf_value;
    CHECK(!memcmp(&producer,&raw.accesses[alias],sizeof(producer)));
    raw_callable_rejection(&raw,program,alias);
    callable_raw_reset(&raw);
    uint32_t callee=raw.accesses[wrapper].index;
    const XrRbcFunction *scalar=&raw.functions[callee];
    CHECK(scalar->capture_count==1 && scalar->parameter_count==2 && !scalar->state_capture);
    size_t count=(size_t)scalar->register_count+1;
    XrLirValueHome *values=xr_calloc(count,sizeof(*values));
    XrRbcValueLocation *locations=xr_calloc(count,sizeof(*locations));
    CHECK(values && locations);
    uint32_t parameters[XR_RBC_REGISTER_COUNT_MAX]={0};
    XrRbcRegisterKind kinds[XR_RBC_REGISTER_COUNT_MAX]; uint32_t signatures[XR_RBC_REGISTER_COUNT_MAX]={0};
    for (uint32_t r=0;r<scalar->register_count;++r) {
        kinds[r]=rbc_register_kind(scalar,r);
        CHECK(kinds[r]==XR_RBC_BITS);
        values[r+1]=(XrLirValueHome){.offset=SIZE_MAX,.first_owner=SIZE_MAX,
            .temporary_offset=SIZE_MAX,.temporary_first_owner=SIZE_MAX};
        locations[r+1]=(XrRbcValueLocation){XR_RBC_VALUE_REGISTER,r};
    }
    for (uint32_t r=0;r<scalar->parameter_count;++r) {
        parameters[r]=r+1; values[r+1].parameter=r+1;
    }
    uint32_t formal=parameters[0],type=raw.values[leaf_value].type;
    values[formal]=(XrLirValueHome){.type=type,.parameter=1,.offset=SIZE_MAX,.size=sizeof(XrtFunction),
        .first_owner=SIZE_MAX,.temporary_offset=SIZE_MAX,.temporary_first_owner=SIZE_MAX,
        .first_bytes=0,.byte_count=1,.transport=XR_LIR_CALLABLE_TRANSPORT_BORROW};
    locations[formal]=(XrRbcValueLocation){XR_RBC_VALUE_NOWHERE,0};
    XrLirHomeBytes bytes={0,sizeof(XrtFunction)};
    XrLirHomeEvent events[]={
        {.kind=XR_LIR_OWNERSHIP_CALLABLE_VALUE,.source=formal,.callable_transport=XR_LIR_CALLABLE_TRANSPORT_BORROW},
        {.kind=XR_LIR_OWNERSHIP_PARAMETER_BORROW,.source=formal}};
    CHECK(raw.entries[callee].signature_type && raw.entries[callee].signature_type<=raw.module.borrowed_value_image->type_count);
    const XrtValueImageType *signature=&raw.module.borrowed_value_image->types[raw.entries[callee].signature_type-1];
    CHECK(signature->kind==XRT_VALUE_FUNCTION && signature->result);
    XrLirValueHomes homes={.source_function=raw.entries[callee].source_function,.value_count=scalar->register_count,
        .result_type=signature->result,.values=values,.events=events,.event_count=2,.bytes=&bytes,.byte_count=1,
        .frame={.payload_alignment=1}};
    /* This fixture's scalar body returns its visible formal. Give the new raw
     * receiving description that same body through the complete value path. */
    XrRbcValueAccess result={.operation=XR_RBC_VALUE_RETURN,.source=parameters[scalar->capture_count]};
    XrRbcValueCode receiver={.homes=&homes,.locations=locations,.value_count=scalar->register_count,
        .parameters=parameters,.parameter_count=scalar->parameter_count,.accesses=&result,.access_count=1};
    uint32_t body=0;
    CHECK(xr_rbc_encode(&body,(XrRbcOperands){.op=XR_RBC_VALUE_ACCESS}));
    raw.codes[callee]=&receiver; raw.functions[callee].code=&body; raw.functions[callee].code_count=1;
    raw.functions[callee].source_ids=NULL; raw.functions[callee].source_id_count=0;
    kinds[0]=XR_RBC_FUNCTION; signatures[0]=rbc_value_signature(&raw.module,&raw.code,leaf_value);
    CHECK(signatures[0]);
    raw.functions[callee].register_kinds=kinds; raw.functions[callee].register_kind_count=scalar->register_count;
    raw.functions[callee].register_callables=signatures; raw.functions[callee].register_callable_count=scalar->register_count;
    XrRbcCallableValue capture={XR_RBC_CALLABLE_FUNCTION,signatures[0]-1}; raw.functions[callee].captures=&capture;
    raw.arguments[raw.accesses[wrapper].first]=leaf_value;
    raw.functions[0].environment_size=(uint32_t)(sizeof(XrtFunction)*2);
    raw_callable_rejection(&raw,program,wrapper);
    xr_free(locations); xr_free(values);
    callable_raw_free(&raw); xr_rbc_program_destroy(program);
    CHECK(!tracked_live);
}
static void check_result(XrRbcExecution **execution, unsigned kind) {
    XrRbcOutcome *outcome = xr_rbc_execution_run(*execution);
    CHECK(outcome && xr_rbc_outcome_status(outcome) == XR_RBC_OK);
    xr_rbc_execution_free(execution);
    if (kind>=4) {
        const XrtFunction *function=NULL;
        CHECK(xr_rbc_outcome_function(outcome,&function));
        XrtEnvironmentBorrow environment={0}; XrtValuePlanSpec shape={0};
        CHECK(xrt_environment_borrow_owned(function->owner,&environment)==XRT_MANAGED_OK);
        CHECK(xrt_value_plan_view(environment.plan,&shape)==XRT_MANAGED_OK);
        CHECK(shape.kind==XRT_VALUE_ENVIRONMENT && shape.capture_count==(kind==5 ? 0u : 1u));
        if (kind==5) CHECK(!shape.size && environment.payload==(const void *)(function->owner+1));
        uint64_t argument=2;
        CHECK(xr_rbc_owned_function_prepare(function,&argument,1,execution)==XR_RBC_OK);
        xr_rbc_outcome_free(&outcome);
        outcome=xr_rbc_execution_run(*execution);
        CHECK(outcome && xr_rbc_outcome_status(outcome)==XR_RBC_OK);
        xr_rbc_execution_free(execution);
        uint64_t result=0;
        CHECK(xr_rbc_outcome_bits(outcome,&result) && result==42);
    } else if (kind == 3) {
        const XrtManagedHeader *string = NULL;
        CHECK(xr_rbc_outcome_string(outcome, &string));
        const XrtStringBacking *text = (const XrtStringBacking *)string;
        CHECK(text->length == 4 && !memcmp(text->bytes, "\303\251\0x", 4));
    } else {
        uint64_t value = UINT64_MAX;
        CHECK(xr_rbc_outcome_bits(outcome, &value));
        CHECK(value == (kind == 1 ? 42u : kind == 2 ? 128u : 7u));
    }
    xr_rbc_outcome_free(&outcome);
}
static void owned_receiving_failures(void) {
    XrRbcProgram *program=compile_fixture(4);
    XrRbcExecution *execution=NULL;
    CHECK(xr_rbc_program_prepare(program,NULL,0,&execution)==XR_RBC_OK);
    xr_rbc_program_destroy(program);
    XrRbcOutcome *outcome=xr_rbc_execution_run(execution);
    xr_rbc_execution_free(&execution);
    const XrtFunction *function=NULL;
    CHECK(xr_rbc_outcome_function(outcome,&function));
    size_t baseline=tracked_live;
    uint64_t argument=2;
    attempts=0;
    CHECK(xr_rbc_owned_function_prepare(function,&argument,1,&execution)==XR_RBC_OK);
    size_t calls=attempts;
    xr_rbc_execution_free(&execution);
    CHECK(tracked_live==baseline);
    for (size_t point=1;point<=calls;++point) {
        attempts=0; fail_at=point;
        CHECK(xr_rbc_owned_function_prepare(function,&argument,1,&execution)==XR_RBC_OUT_OF_MEMORY);
        CHECK(!execution && tracked_live==baseline);
    }
    fail_at=SIZE_MAX;
    for (unsigned mutation=0;mutation<3;++mutation) {
        XrtFunction invalid=*function;
        if (!mutation) invalid.entry=&argument;
        if (mutation==1) ++invalid.environment;
        if (mutation==2) invalid.owner=NULL;
        CHECK(xr_rbc_owned_function_prepare(&invalid,&argument,1,&execution)==XR_RBC_INVALID);
        CHECK(!execution && tracked_live==baseline);
    }
    int32_t references=atomic_load(&function->owner->references);
    atomic_store(&function->owner->references,-INT32_MAX);
    CHECK(xr_rbc_owned_function_prepare(function,&argument,1,&execution)==XR_RBC_VALUE_FAILURE && !execution);
    atomic_store(&function->owner->references,references);
    CHECK(tracked_live==baseline);
    CHECK(xr_rbc_owned_function_prepare(function,&argument,1,&execution)==XR_RBC_OK);
    xr_rbc_outcome_free(&outcome);
    outcome=xr_rbc_execution_run(execution);
    xr_rbc_execution_free(&execution);
    uint64_t result=0;
    CHECK(xr_rbc_outcome_status(outcome)==XR_RBC_OK && xr_rbc_outcome_bits(outcome,&result) && result==42);
    xr_rbc_outcome_free(&outcome);
    CHECK(!tracked_live);
    printf("Retired Owned Fn prepare: %zu allocation failures preserve the lender\n",calls);
}
static void owned_outcome_take(void) {
    XrRbcProgram *program=compile_fixture(5);
    XrRbcExecution *execution=NULL;
    CHECK(xr_rbc_program_prepare(program,NULL,0,&execution)==XR_RBC_OK);
    xr_rbc_program_destroy(program);
    XrRbcOutcome *outcome=xr_rbc_execution_run(execution);
    xr_rbc_execution_free(&execution);
    XrtFunction function={0};
    CHECK(xr_rbc_outcome_take_function(outcome,&function) && function.entry && function.owner);
    const XrtFunction *sentinel=&function;
    CHECK(!xr_rbc_outcome_function(outcome,&sentinel) && sentinel==&function);
    xr_rbc_outcome_free(&outcome);
    uint64_t argument=2;
    CHECK(xr_rbc_owned_function_prepare(&function,&argument,1,&execution)==XR_RBC_OK);
    xrt_managed_drop(function.owner);
    outcome=xr_rbc_execution_run(execution);
    xr_rbc_execution_free(&execution);
    uint64_t result=0;
    CHECK(xr_rbc_outcome_status(outcome)==XR_RBC_OK && xr_rbc_outcome_bits(outcome,&result) && result==42);
    xr_rbc_outcome_free(&outcome);
    CHECK(!tracked_live);
}
static void borrowed_cell_edge_rollback(void) {
    XrRbcProgram *program=compile_fixture(6);
    XrRbcExecution *execution=NULL;
    CHECK(xr_rbc_program_prepare(program,NULL,0,&execution)==XR_RBC_OK);
    xr_rbc_program_destroy(program);
    XrRbcOutcome *lender=xr_rbc_execution_run(execution);
    xr_rbc_execution_free(&execution);
    const XrtFunction *function=NULL;
    CHECK(xr_rbc_outcome_function(lender,&function));
    XrtEnvironmentBorrow environment={0};
    CHECK(xrt_environment_borrow_owned(function->owner,&environment)==XRT_MANAGED_OK);
    const XrtValuePlan *type=NULL; const void *bytes=NULL;
    CHECK(xrt_environment_capture_borrow(&environment,0,&type,&bytes)==XRT_MANAGED_OK);
    XrtValuePlanSpec shape={0}; CHECK(xrt_value_plan_view(type,&shape)==XRT_MANAGED_OK && shape.kind==XRT_VALUE_CELL);
    XrtManagedHeader *cell=NULL; memcpy(&cell,bytes,sizeof(cell)); CHECK(cell);
    int32_t saved=atomic_load(&cell->references);
    /* Formal KEEP retains once, then the first duplicate stages successfully.
     * The second retain must fail before either destination is published. */
    atomic_store(&cell->references,INT32_MAX-2);
    uint64_t argument=2;
    CHECK(xr_rbc_owned_function_prepare(function,&argument,1,&execution)==XR_RBC_OK);
    XrRbcOutcome *failed=xr_rbc_execution_run(execution);
    CHECK(failed && xr_rbc_outcome_status(failed)==XR_RBC_VALUE_FAILURE);
    XrXirValueStatus failure; uint32_t source; const XrtLocationOwner *locations;
    CHECK(xr_rbc_outcome_value_failure(failed,&failure,&source,&locations) && failure==XR_XIR_VALUE_REFCOUNT_LIMIT);
    xr_rbc_execution_free(&execution); xr_rbc_outcome_free(&failed);
    CHECK(atomic_load(&cell->references)==INT32_MAX-2);
    atomic_store(&cell->references,saved);
    CHECK(xr_rbc_owned_function_prepare(function,&argument,1,&execution)==XR_RBC_OK);
    xr_rbc_outcome_free(&lender);
    XrRbcOutcome *outcome=xr_rbc_execution_run(execution);
    xr_rbc_execution_free(&execution);
    uint64_t result=0;
    CHECK(xr_rbc_outcome_status(outcome)==XR_RBC_OK && xr_rbc_outcome_bits(outcome,&result) && result==42);
    xr_rbc_outcome_free(&outcome); CHECK(!tracked_live);
}
static void owned_run_failures(void) {
    XrRbcProgram *program=compile_fixture(4);
    size_t baseline=tracked_live;
    XrRbcExecution *execution=NULL;
    CHECK(xr_rbc_program_prepare(program,NULL,0,&execution)==XR_RBC_OK);
    attempts=0;
    XrRbcOutcome *outcome=xr_rbc_execution_run(execution);
    size_t calls=attempts;
    CHECK(xr_rbc_outcome_status(outcome)==XR_RBC_OK);
    xr_rbc_execution_free(&execution); xr_rbc_outcome_free(&outcome);
    CHECK(tracked_live==baseline);
    for (size_t point=1;point<=calls;++point) {
        CHECK(xr_rbc_program_prepare(program,NULL,0,&execution)==XR_RBC_OK);
        attempts=0; fail_at=point;
        outcome=xr_rbc_execution_run(execution);
        CHECK(outcome);
        XrRbcStatus status=xr_rbc_outcome_status(outcome);
        if (status==XR_RBC_VALUE_FAILURE) {
            XrXirValueStatus failure; uint32_t source; const XrtLocationOwner *locations;
            CHECK(xr_rbc_outcome_value_failure(outcome,&failure,&source,&locations) && failure==XR_XIR_VALUE_OOM);
        } else CHECK(status==XR_RBC_OUT_OF_MEMORY);
        fail_at=SIZE_MAX;
        xr_rbc_execution_free(&execution); xr_rbc_outcome_free(&outcome);
        CHECK(tracked_live==baseline);
    }
    CHECK(xr_rbc_program_prepare(program,NULL,0,&execution)==XR_RBC_OK);
    check_result(&execution,4);
    xr_rbc_program_destroy(program);
    CHECK(!tracked_live);
    printf("Owned Fn run: %zu allocation failures retire staging and restore the code baseline\n",calls);
}
static void scalar_callable_directory(void) {
    CHECK(!tracked_live);
    XrLirModule *module=NULL; XrLirFunction wrapper={0};
    callable_fixture(&module,&wrapper);
    XrLirFunction scalar={module,1};
    XrRbcProgram *program=NULL;
    attempts=0;
    CHECK(xr_rbc_compile(module,scalar,&program,NULL)==XR_RBC_COMPILE_OK);
    size_t calls=attempts;
    CHECK(!program->image->value_homes && !program->image->value_image && program->image->value_codes);
    CHECK(program->image->value_codes[0] && !program->image->value_codes[0]->homes);
    xr_rbc_program_destroy(program); program=NULL;
    CHECK(!tracked_live);
    for (size_t point=1;point<=calls;++point) {
        attempts=0; fail_at=point;
        CHECK(xr_rbc_compile(module,scalar,&program,NULL)==XR_RBC_COMPILE_OUT_OF_MEMORY);
        CHECK(!program && !tracked_live);
    }
    fail_at=SIZE_MAX;
    CHECK(xr_rbc_compile(module,scalar,&program,NULL)==XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(module);
    XrRbcExecution *execution=NULL; uint64_t argument=41;
    CHECK(xr_rbc_program_prepare(program,&argument,1,&execution)==XR_RBC_OK);
    xr_rbc_program_destroy(program);
    XrRbcOutcome *outcome=xr_rbc_execution_run(execution);
    xr_rbc_execution_free(&execution);
    uint64_t result=0;
    CHECK(xr_rbc_outcome_status(outcome)==XR_RBC_OK && xr_rbc_outcome_bits(outcome,&result) && result==42);
    xr_rbc_outcome_free(&outcome);
    CHECK(!tracked_live);
    printf("Scalar callable directory: %zu compile failures refunded without managed homes\n",calls);
}
static bool cell_raw_retain(void *context) { ++*(uint32_t *)context; return true; }
static void cell_raw_drop(void *context) { --*(uint32_t *)context; }
static void prepared_after_program(unsigned kind) {
    CHECK(!tracked_live);
    XrRbcProgram *program = compile_fixture(kind);
    if (kind>=6) {
        XrRbcModule raw=*xr_rbc_program_view(program);
        raw.code_lease=(XrRbcCodeLease){0};
        XrRbcExecution *rejected=NULL;
        CHECK(xr_rbc_module_prepare(&raw,0,NULL,0,program->locations,&rejected)==XR_RBC_INVALID && !rejected);
        uint32_t references=1;
        raw.code_lease=(XrRbcCodeLease){&references,cell_raw_retain,cell_raw_drop};
        CHECK(xr_rbc_module_prepare(&raw,0,NULL,0,program->locations,&rejected)==XR_RBC_INVALID && !rejected && references==1);
    }
    if (!kind) CHECK(program->module.natives == program->image->natives && program->module.natives != program->natives);
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
    CHECK(execution->module != &program->module && execution->module == &execution->module_snapshot);
    watched_program = program; watched_layout = execution->layout;
    program_freed = layout_freed = false;
    if (watched_layout) CHECK(atomic_load(&execution->layout->references) == -2);
    xr_rbc_program_destroy(program);
    CHECK(program_freed && (!watched_layout || !layout_freed));
    check_result(&execution, kind);
    CHECK(!watched_layout || layout_freed);
    watched_program = watched_layout = NULL;
    CHECK(!tracked_live);
}
static void code_lease_is_leaf(void) {
    XrRbcProgram *program = compile_fixture(2);
    XrRbcCodeLease lease = program->module.code_lease;
    CHECK(lease.owner && lease.owner != program && lease.retain(lease.owner));
    XrRbcCodeImage *image = lease.owner;
    CHECK(image->module.state_spec == program->module.state_spec &&
        image->module.functions == program->module.functions);
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
    CHECK(execution->layout && execution->state);
    watched_program = program; watched_layout = execution->layout;
    program_freed = layout_freed = false;
    xr_rbc_program_destroy(program);
    CHECK(program_freed && !layout_freed && tracked_live && image->module.function_count);
    xr_rbc_execution_free(&execution);
    CHECK(layout_freed && tracked_live && atomic_load(&image->code.references) == 1);
    CHECK(image->module.functions[0].code_count && xrt_locations_count(image->locations));
    watched_program = watched_layout = NULL;
    lease.release(lease.owner);
    CHECK(!tracked_live);
}
static void raw_borrow_and_refusal(void) {
    XrRbcProgram *program = compile_fixture(2);
    size_t baseline = tracked_live;
    XrRbcExecution *execution = NULL;
    XrRbcCodeImage *image = program->image;
    atomic_store(&image->code.references, UINT32_MAX);
    CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_VALUE_FAILURE && !execution);
    atomic_store(&image->code.references, 1); CHECK(tracked_live == baseline);
    CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
    atomic_store(&execution->layout->references, -INT32_MAX);
    CHECK(!xrt_module_layout_retain(execution->layout));
    atomic_store(&execution->layout->references, -2);
    xr_rbc_execution_free(&execution);
    CHECK(tracked_live == baseline && atomic_load(&image->code.references) == 1);
    XrRbcModule raw = program->module;
    raw.code_lease = (XrRbcCodeLease){0};
    CHECK(xr_rbc_module_prepare(&raw, 0, NULL, 0, program->locations, &execution) == XR_RBC_OK);
    CHECK(!execution->code_lease.owner && execution->module != &raw);
    check_result(&execution, 2);
    CHECK(tracked_live == baseline);
    xr_rbc_program_destroy(program); CHECK(!tracked_live);
}
static void failure_sweep(unsigned kind) {
    XrLirFunction root;
    XrLirModule *module = fixture(kind, &root);
    XrRbcProgram *program = NULL;
    attempts = 0;
    CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OK);
    size_t compile_calls = attempts;
    xr_rbc_program_destroy(program); program = NULL; CHECK(!tracked_live);
    for (size_t point = 1; point <= compile_calls; ++point) {
        attempts = 0; fail_at = point;
        CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OUT_OF_MEMORY);
        CHECK(!program && !tracked_live);
    }
    fail_at = SIZE_MAX;
    CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(module);
    size_t baseline = tracked_live;
    XrRbcExecution *execution = NULL;
    attempts = 0;
    CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
    size_t prepare_calls = attempts;
    attempts = 0;
    size_t run_calls;
    if (kind>=6) {
        /* Measure only the factory run. Retired invocation is a separate real
         * prepare/run boundary and has its own owner in check_result. */
        XrRbcOutcome *outcome=xr_rbc_execution_run(execution);
        run_calls=attempts;
        CHECK(outcome && xr_rbc_outcome_status(outcome)==XR_RBC_OK);
        xr_rbc_execution_free(&execution); xr_rbc_outcome_free(&outcome);
    } else { check_result(&execution,kind); run_calls=attempts; }
    CHECK(tracked_live == baseline);
    for (size_t point = 1; point <= prepare_calls; ++point) {
        attempts = 0; fail_at = point;
        CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OUT_OF_MEMORY && !execution);
        CHECK(tracked_live == baseline);
    }
    fail_at = SIZE_MAX;
    for (size_t point = 1; point <= run_calls; ++point) {
        CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
        attempts = 0; fail_at = point;
        XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
        CHECK(outcome);
        XrRbcStatus status = xr_rbc_outcome_status(outcome);
        if (status == XR_RBC_VALUE_FAILURE) {
            XrXirValueStatus value_status; uint32_t source; const XrtLocationOwner *locations;
            CHECK(xr_rbc_outcome_value_failure(outcome, &value_status, &source, &locations) && value_status == XR_XIR_VALUE_OOM);
        } else CHECK(status == XR_RBC_OUT_OF_MEMORY);
        fail_at = SIZE_MAX;
        xr_rbc_execution_free(&execution); xr_rbc_outcome_free(&outcome);
        CHECK(tracked_live == baseline);
    }
    xr_rbc_program_destroy(program); CHECK(!tracked_live);
    printf("Code owner fixture %u: compile/prepare/run OOM sites %zu/%zu/%zu refunded\n", kind, compile_calls, prepare_calls, run_calls);
}
#include "rbc_header_edge_cases.h"
#include "rbc_array_region_cases.h"
int main(void) {
    rbc_array_region_cases();
    for (unsigned kind = 0; kind < 4; ++kind) { prepared_after_program(kind); failure_sweep(kind); }
    code_lease_is_leaf(); raw_borrow_and_refusal();
    prepared_after_program(4); prepared_after_program(5); owned_receiving_failures(); owned_outcome_take();
    owned_run_failures();
    for (unsigned kind=6;kind<8;++kind) { prepared_after_program(kind); failure_sweep(kind); }
    borrowed_cell_edge_rollback();
    scalar_callable_directory();
    raw_callable_exact_boundaries();
    header_only_edge_failures(); tagged_edge_keeps_publication();
    mixed_edge_publication_order(false); mixed_edge_publication_order(true);
    puts("CodeImage leases, independent Layout snapshots and post-Program executions passed");
    return 0;
}
