/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_binding_homes.c - Stable binding owners and instruction-local pending slots
 */
#include "lir/xlir_value_homes.h"
#include "lir/xlir_state.h"
#include "runtime/native/xnative_array_backing.h"
#include "runtime/native/xnative_map_backing.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); abort(); } } while (0)
#define OK(c) CHECK((c) == XR_LIR_OK)
#define HOME(c) CHECK((c) == XR_LIR_VALUE_LAYOUT_OK)
#define MANAGED(c) CHECK((c) == XRT_MANAGED_OK)
#include "lir_array_fixture.h"
#include "lir_array_repeat_fixture.h"
#include "lir_array_ref_fixture.h"
#include "lir_array_cell_cfg_cases.h"
#include "lir_array_block_cell_cases.h"

typedef struct CArrayFixture { XrLirModule *module; XrLirType element; } CArrayFixture;
static XrLirValue ca_value(CArrayFixture *c, XrLirBlock block, XrLirOp op,
    XrLirType type, const XrLirValue *args, uint32_t count) {
    XrLirValue result={0}; XrLirInstruction instruction={.op=op,.type=type,.operands=args,.operand_count=count};
    OK(xr_lir_emit_value(c->module,block,&instruction,&result)); return result;
}
static XrLirValue number(CArrayFixture *c,XrLirBlock block,XrLirType type,uint64_t bits) {
    XrLirValue result={0}; XrLirInstruction instruction={.op=XR_LIR_CONST,.type=type,.bits=bits};
    OK(xr_lir_emit_value(c->module,block,&instruction,&result)); return result;
}
static XrLirValue ca_constant(CArrayFixture *c,XrLirBlock block,int64_t value) { return number(c,block,XR_LIR_I64,(uint64_t)value); }
static void ca_effect(CArrayFixture *c,XrLirBlock block,XrLirOp op,XrLirValue value) {
    XrLirInstruction instruction={.op=op,.type=XR_LIR_UNIT,.operands=&value,.operand_count=1};
    OK(xr_lir_emit_effect(c->module,block,&instruction));
}
static void ca_return(CArrayFixture *c,XrLirBlock block,XrLirValue value) {
    XrLirInstruction instruction={.op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=value.id ? &value : NULL,
        .operand_count=value.id ? 1u : 0u}; OK(xr_lir_set_terminator(c->module,block,&instruction));
}
static XrLirValue ca_text(CArrayFixture *c,XrLirBlock block,const char *bytes,size_t count) {
    XrLirLiteral literal={0}; XrLirValue result={0}; OK(xr_lir_add_literal(c->module,bytes,count,&literal));
    XrLirInstruction instruction={.op=XR_LIR_STRING_LITERAL,.type=XR_LIR_STRING,.bits=literal.id};
    OK(xr_lir_emit_value(c->module,block,&instruction,&result)); return result;
}
#include "lir_map_fixture.h"
static XrLirValue role(CArrayFixture *c,XrLirBlock block,uint32_t operation,
    XrLirType result,const XrLirValue *args,uint32_t count) {
    uint32_t id=0;
    if (operation>=XR_LIR_MAP_OP_NEW) {
        XrLirType receiver=operation>=XR_LIR_MAP_OP_SET ? XR_LIR_MAP_STRING_I64_REF : XR_LIR_MAP_STRING_I64;
        OK(xr_lir_map_operation(c->module,(XrLirMapOperation)operation,receiver,&id));
    } else {
        XrLirType element=c->element;
        if (operation==XR_LIR_ARRAY_OP_SET || operation==XR_LIR_ARRAY_OP_PUSH) {
            OK(xr_lir_array_mutator(c->module,element,XR_LIR_ARRAY_REF,(XrLirArrayOperation)operation,&id));
        } else OK(xr_lir_array_operation(c->module,element,(XrLirArrayOperation)operation,&id));
    }
    XrLirValue value={0}; XrLirInstruction instruction={.op=XR_LIR_CALL_INTRINSIC,.type=result,
        .intrinsic=id,.operands=args,.operand_count=count};
    if (result==XR_LIR_UNIT) OK(xr_lir_emit_effect(c->module,block,&instruction));
    else OK(xr_lir_emit_value(c->module,block,&instruction,&value));
    return value;
}
static XrLirModule *roles_fixture(XrLirType element,bool map) {
    CArrayFixture c={.element=element}; XrLirType optional=0;
    if (map) {
        OK(xr_lir_module_create(NULL,&c.module)); OK(xr_lir_map_catalog_install(c.module,false,false,false,false,false));
        OK(xr_lir_nullable_type(c.module,XR_LIR_I64,&optional));
    } else OK(xr_lir_array_module_create(NULL,&c.module));
    XrLirType value_type=map ? XR_LIR_MAP_STRING_I64 : XR_LIR_ARRAY;
    XrLirType parameters[]={map ? XR_LIR_MAP_STRING_I64_CELL : XR_LIR_ARRAY_CELL,
        map ? XR_LIR_MAP_STRING_I64_REF : XR_LIR_ARRAY_REF};
    XrLirFunction function={0}; XrLirFunctionSpec spec={.name="roles",.name_length=5,
        .parameters=parameters,.parameter_count=2,.result=XR_LIR_UNIT}; OK(xr_lir_declare_function(c.module,&spec,&function));
    XrLirValue inputs[2]={{0}};
    for (uint32_t p=0;p<2;++p) {
        OK(xr_lir_function_parameter(c.module,function,p,&inputs[p]));
        if (!map) OK(xr_lir_set_array_shape(c.module,inputs[p],&(XrLirArrayShape){element}));
    }
    OK(xr_lir_define_function(c.module,function)); XrLirBlock block={0};
    OK(xr_lir_add_block(c.module,function,NULL,0,&block)); OK(xr_lir_set_entry(c.module,function,block));
    XrLirValue snapshot=ca_value(&c,block,XR_LIR_CELL_READ,value_type,&inputs[0],1),write[]={inputs[0],snapshot};
    XrLirInstruction instruction={.op=XR_LIR_CELL_WRITE,.type=XR_LIR_UNIT,.operands=write,.operand_count=2};
    OK(xr_lir_emit_effect(c.module,block,&instruction)); ca_effect(&c,block,map ? XR_LIR_MAP_DROP : XR_LIR_ARRAY_DROP,snapshot);
    XrLirValue borrowed=ca_value(&c,block,map ? XR_LIR_MAP_BORROW : XR_LIR_ARRAY_BORROW,parameters[1],&inputs[0],1);
    snapshot=ca_value(&c,block,map ? XR_LIR_MAP_REF_READ : XR_LIR_ARRAY_REF_READ,value_type,&inputs[1],1);
    write[0]=inputs[1]; write[1]=snapshot;
    instruction=(XrLirInstruction){.op=map ? XR_LIR_MAP_REF_WRITE : XR_LIR_ARRAY_REF_WRITE,
        .type=XR_LIR_UNIT,.operands=write,.operand_count=2}; OK(xr_lir_emit_effect(c.module,block,&instruction));
    XrLirValue fill=map || element==XR_LIR_STRING ? ca_text(&c,block,"key",3) : number(&c,block,element,7);
    XrLirValue zero=ca_constant(&c,block,0),args[]={snapshot,map ? fill : zero};
    (void)role(&c,block,map ? XR_LIR_MAP_OP_LEN : XR_LIR_ARRAY_OP_LEN,XR_LIR_I64,args,1);
    XrLirValue item=role(&c,block,map ? XR_LIR_MAP_OP_GET : XR_LIR_ARRAY_OP_GET,map ? optional : element,args,2);
    if (!map && element==XR_LIR_STRING) ca_effect(&c,block,XR_LIR_STRING_DROP,item);
    if (map) {
        (void)role(&c,block,XR_LIR_MAP_OP_INDEX,XR_LIR_I64,args,2); (void)role(&c,block,XR_LIR_MAP_OP_CONTAINS,XR_LIR_I1,args,2);
        XrLirValue set[]={borrowed,fill,ca_constant(&c,block,42)};
        (void)role(&c,block,XR_LIR_MAP_OP_SET,XR_LIR_UNIT,set,3); args[0]=inputs[1];
        (void)role(&c,block,XR_LIR_MAP_OP_REMOVE,XR_LIR_I1,args,2); (void)role(&c,block,XR_LIR_MAP_OP_CLEAR,XR_LIR_UNIT,args,1);
    } else {
        XrLirValue set[]={borrowed,zero,fill},push[]={inputs[1],fill},repeat[]={ca_constant(&c,block,2),fill};
        (void)role(&c,block,XR_LIR_ARRAY_OP_SET,XR_LIR_UNIT,set,3); (void)role(&c,block,XR_LIR_ARRAY_OP_PUSH,XR_LIR_UNIT,push,2);
        XrLirValue repeated=role(&c,block,XR_LIR_ARRAY_OP_REPEAT,XR_LIR_ARRAY,repeat,2); ca_effect(&c,block,XR_LIR_ARRAY_DROP,repeated);
        XrLirValue display=role(&c,block,XR_LIR_ARRAY_OP_DISPLAY,XR_LIR_STRING,&snapshot,1); ca_effect(&c,block,XR_LIR_STRING_DROP,display);
    }
    ca_effect(&c,block,map ? XR_LIR_MAP_DROP : XR_LIR_ARRAY_DROP,snapshot);
    if (map || element==XR_LIR_STRING) ca_effect(&c,block,XR_LIR_STRING_DROP,fill);
    ca_return(&c,block,(XrLirValue){0}); OK(xr_lir_module_finish(c.module,NULL)); return c.module;
}

static XrLirModule *loop_fixture(void) {
    CArrayFixture c={.element=XR_LIR_I64}; OK(xr_lir_array_module_create(NULL,&c.module));
    XrLirFunction function={0}; XrLirFunctionSpec spec={.name="loop",.name_length=4,.result=XR_LIR_UNIT};
    OK(xr_lir_declare_function(c.module,&spec,&function)); OK(xr_lir_define_function(c.module,function));
    XrLirBlock entry={0},body={0},exit={0};
    OK(xr_lir_add_block(c.module,function,NULL,0,&entry)); OK(xr_lir_add_block(c.module,function,NULL,0,&body));
    OK(xr_lir_add_block(c.module,function,NULL,0,&exit)); OK(xr_lir_set_entry(c.module,function,entry));
    XrLirInstruction branch={.op=XR_LIR_BR,.type=XR_LIR_UNIT,.edge_count=1,.edges={{body,NULL,0}}};
    OK(xr_lir_set_terminator(c.module,entry,&branch));
    XrLirValue array=role(&c,body,XR_LIR_ARRAY_OP_NEW,XR_LIR_ARRAY,NULL,0);
    XrLirValue cell=ca_value(&c,body,XR_LIR_CELL_NEW,XR_LIR_ARRAY_CELL,&array,1);
    ca_effect(&c,body,XR_LIR_ARRAY_DROP,array);
    XrLirValue borrow=ca_value(&c,body,XR_LIR_ARRAY_BORROW,XR_LIR_ARRAY_REF,&cell,1);
    XrLirValue args[]={borrow,ca_constant(&c,body,7)};
    (void)role(&c,body,XR_LIR_ARRAY_OP_PUSH,XR_LIR_UNIT,args,2);
    ca_effect(&c,body,XR_LIR_ARRAY_CELL_DROP,cell);
    XrLirValue condition=number(&c,body,XR_LIR_I1,0);
    branch=(XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,.operands=&condition,.operand_count=1,
        .edge_count=2,.edges={{body,NULL,0},{exit,NULL,0}}};
    OK(xr_lir_set_terminator(c.module,body,&branch)); ca_return(&c,exit,(XrLirValue){0});
    OK(xr_lir_module_finish(c.module,NULL)); return c.module;
}

static size_t attempts,fail_at=SIZE_MAX,live;
static struct { void *memory; size_t bytes; } buffers[4096];
static size_t buffer_index(void *memory) {
    for (size_t i=0;i<4096;++i) if (buffers[i].memory==memory) return i;
    CHECK(false); return 0;
}
static void *home_allocate(size_t bytes) {
    if (++attempts==fail_at) return NULL;
    void *memory=xr_malloc(bytes);
    if (memory) { size_t i=buffer_index(NULL); buffers[i].memory=memory; buffers[i].bytes=bytes; ++live; } return memory;
}
static void home_free(void *memory) {
    if (!memory) return;
    size_t i=buffer_index(memory); buffers[i].memory=NULL; CHECK(live); --live; xr_free(memory);
}
static void *home_callocate(size_t count,size_t bytes) {
    CHECK(!bytes || count<=SIZE_MAX/bytes); void *memory=home_allocate(count*bytes);
    if (memory) memset(memory,0,count*bytes);
    return memory;
}
static void *home_resize(void *old,size_t bytes) {
    if (++attempts==fail_at) return NULL;
    void *memory=xr_malloc(bytes); if (!memory) return NULL;
    size_t i=buffer_index(old);
    if (old) { memcpy(memory,old,bytes<buffers[i].bytes ? bytes : buffers[i].bytes); xr_free(old); } else ++live;
    buffers[i].memory=memory; buffers[i].bytes=bytes; return memory;
}
#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#define xr_malloc(bytes) home_allocate(bytes)
#define xr_calloc(count,bytes) home_callocate(count,bytes)
#define xr_realloc(old,bytes) home_resize(old,bytes)
#define xr_free(memory) home_free(memory)
#include "lir/xlir_value_homes.c"

static XrLirValueLayouts *layouts(XrLirModule *module,XrtBillingOwner *billing) {
    XrLirValueLayoutKey roots[32]={{XR_LIR_UNIT,0},{XR_LIR_I1,0},{XR_LIR_I64,0},{XR_LIR_U8,0},{XR_LIR_U64,0},
        {XR_LIR_STRING,0},{XR_LIR_MAP_STRING_I64,0},{XR_LIR_ARRAY,XR_LIR_I64},{XR_LIR_ARRAY,XR_LIR_STRING},
        {XR_LIR_ARRAY,XR_LIR_U8},{XR_LIR_ARRAY,XR_LIR_U64}}; size_t count=11;
    for (uint32_t i=0;i<xr_lir_constructed_type_count(module);++i) {
        XrLirType type=XR_LIR_TYPE_COUNT+i; XrLirTypeView shape; OK(xr_lir_type_view(module,type,&shape)); CHECK(count<32);
        if (shape.kind==XR_LIR_TYPE_NULLABLE) roots[count++]=(XrLirValueLayoutKey){type,0};
    }
    XrLirValueLayouts *set=NULL; HOME(xr_lir_value_layouts_create(module,roots,count,billing,&set)); return set;
}
static unsigned kinds,array_roles,map_roles;
static void check_projection(const XrLirValueHomes *homes,XrtBillingOwner *billing,
    const XrtBackingTypes *types,const XrtValueKind *workspace_kinds) {
    for (uint32_t v=0;homes->bindings && v<=homes->value_count;++v) {
        const XrLirHomeBinding *row=&homes->bindings[v];
        if (!row->kind) { CHECK(!row->type); continue; }
        kinds|=1u<<row->kind; CHECK(row->type && !homes->values[v].type);
        if (row->kind==XR_LIR_HOME_BINDING_LOCAL) {
            CHECK(!row->parameter && !row->source && row->first_owner<homes->frame.slot_count);
            CHECK(homes->frame.owned_offsets[row->first_owner]==row->offset);
        } else CHECK(row->offset==SIZE_MAX && row->first_owner==SIZE_MAX);
        if (row->kind==XR_LIR_HOME_BINDING_PARAMETER) CHECK(row->parameter && !row->source);
        if (row->kind==XR_LIR_HOME_BINDING_CELL_BORROW) CHECK(row->source && homes->bindings[row->source].type==row->type);
        if (row->kind==XR_LIR_HOME_BINDING_STATE_BORROW) CHECK(row->source && !homes->values[row->source].type);
    }
    size_t expected=0;
    for (size_t i=0;i<homes->event_count;++i) {
        const XrLirHomeEvent *event=&homes->events[i];
        if (event->kind==XR_LIR_OWNERSHIP_ARRAY_OPERATION) {
            CHECK(event->operation && event->operation<=XR_LIR_ARRAY_OP_DISPLAY); array_roles|=1u<<event->operation;
            expected+=event->operation==XR_LIR_ARRAY_OP_SET || event->operation==XR_LIR_ARRAY_OP_PUSH;
        }
        if (event->kind==XR_LIR_OWNERSHIP_MAP_OPERATION) {
            CHECK(event->operation>=XR_LIR_MAP_OP_NEW && event->operation<=XR_LIR_MAP_OP_CLEAR);
            map_roles|=1u<<(event->operation-XR_LIR_MAP_OP_NEW);
            expected+=event->operation==XR_LIR_MAP_OP_SET || event->operation==XR_LIR_MAP_OP_REMOVE || event->operation==XR_LIR_MAP_OP_CLEAR;
        }
    }
    CHECK(homes->workspace_count==expected);
    for (size_t i=0;i<homes->workspace_count;++i) {
        XrLirHomeWorkspace row=homes->workspaces[i]; CHECK(row.block && row.instruction && row.type);
        CHECK(row.first_owner<homes->frame.slot_count && homes->frame.owned_offsets[row.first_owner]==row.offset);
        for (size_t j=0;j<i;++j) CHECK(row.offset!=homes->workspaces[j].offset);
    }
    XrtManagedRoot *root=NULL; XrtManagedFrame *frame=NULL;
    MANAGED(xrt_managed_root_new(billing,&root)); MANAGED(xrt_managed_frame_enter_layout(root,&homes->frame,&frame));
    void *payload=NULL; size_t bytes=0; MANAGED(xrt_managed_frame_payload(frame,&payload,&bytes)); CHECK(bytes==homes->frame.payload_size);
    for (size_t i=0;i<homes->frame.slot_count;++i) {
        XrtManagedHeader *owner=(XrtManagedHeader *)(uintptr_t)1;
        memcpy(&owner,(unsigned char *)payload+homes->frame.owned_offsets[i],sizeof(owner)); CHECK(!owner);
    }
    for (size_t i=0;i<homes->workspace_count;++i) {
        XrLirHomeWorkspace row=homes->workspaces[i]; XrtValueKind kind=workspace_kinds[i];
        XrtManagedOps *ops=NULL; size_t extent=sizeof(XrtArrayBacking);
        switch (kind) {
        case XRT_VALUE_ARRAY_I64: ops=types->arrays[XRT_BACKING_ARRAY_I64]; break;
        case XRT_VALUE_ARRAY_STRING: ops=types->arrays[XRT_BACKING_ARRAY_STRING]; break;
        case XRT_VALUE_ARRAY_U8: ops=types->arrays[XRT_BACKING_ARRAY_U8]; break;
        case XRT_VALUE_ARRAY_U64: ops=types->arrays[XRT_BACKING_ARRAY_U64]; break;
        case XRT_VALUE_MAP_STRING_I64: ops=types->map; extent=sizeof(XrtMapBacking); break;
        default: CHECK(false);
        }
        XrtManagedSlotLocation location; MANAGED(xrt_managed_slot_location(frame,row.first_owner,&location));
        CHECK((void *)location.owner==(unsigned char *)payload+row.offset && !*location.owner);
        XrtBillingStats before=xrt_billing_owner_stats(billing); XrtManagedHeader *pending=NULL;
        xrt_billing_owner_fail_after(billing,0);
        CHECK(xrt_managed_slot_prepare(frame,row.first_owner,ops,extent,&pending)==XRT_MANAGED_OOM && !pending && !*location.owner);
        xrt_billing_owner_fail_after(billing,SIZE_MAX);
        MANAGED(xrt_managed_slot_prepare(frame,row.first_owner,ops,extent,&pending));
        CHECK(pending && !*location.owner); MANAGED(xrt_managed_slot_abort(frame,row.first_owner));
        XrtBillingStats after=xrt_billing_owner_stats(billing); CHECK(after.bytes==before.bytes && after.blocks==before.blocks);
        pending=NULL; MANAGED(xrt_managed_slot_prepare(frame,row.first_owner,ops,extent,&pending));
        MANAGED(xrt_managed_slot_commit(frame,row.first_owner)); CHECK(*location.owner==pending);
        MANAGED(xrt_managed_slot_drop(frame,row.first_owner)); CHECK(!*location.owner);
        after=xrt_billing_owner_stats(billing); CHECK(after.bytes==before.bytes && after.blocks==before.blocks);
    }
    MANAGED(xrt_managed_root_destroy(&root));
}
static void project(XrLirModule *module,bool faults) {
    XrtBillingOwner *billing=NULL; MANAGED(xrt_billing_owner_new(NULL,SIZE_MAX,&billing));
    XrLirValueLayouts *set=layouts(module,billing); const XrLirOwnershipTypeFact *facts=NULL; uint32_t count=0;
    HOME(xr_lir_value_layouts_ownership(set,&facts,&count));
    XrLirOwnershipPlan *plan=NULL; OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){module,facts,count},&plan,NULL));
    size_t functions=xr_lir_function_count(module); CHECK(functions<=8); XrLirValueHomes *homes[8]={0};
    XrtValueKind workspace_kinds[8][8]={{0}};
    for (uint32_t f=0;f<functions;++f) {
        XrLirFunction function={0}; OK(xr_lir_function_at(module,f,&function));
        XrLirValueHomesRequest request={module,function,set,plan}; attempts=0;
        HOME(xr_lir_value_homes_create(&request,&homes[f])); size_t total=attempts;
        if (faults) {
            xr_lir_value_homes_destroy(homes[f]); homes[f]=NULL; CHECK(!live);
            for (size_t n=1;n<=total;++n) {
                attempts=0; fail_at=n;
                CHECK(xr_lir_value_homes_create(&request,&homes[f])==XR_LIR_VALUE_LAYOUT_OOM && !homes[f] && !live);
            }
            fail_at=SIZE_MAX; HOME(xr_lir_value_homes_create(&request,&homes[f]));
            printf("binding home allocation rollback sites: %zu\n",total);
        }
        CHECK(homes[f] && homes[f]->workspace_count<=8);
        for (size_t i=0;i<homes[f]->workspace_count;++i) {
            XrLirValueLayoutKey key; const XrtValuePlan *payload_plan=NULL; XrtValuePlanSpec payload_spec;
            HOME(xr_lir_value_layouts_at(set,homes[f]->workspaces[i].type-1,&key,&payload_plan));
            MANAGED(xrt_value_plan_view(payload_plan,&payload_spec)); workspace_kinds[f][i]=payload_spec.kind;
        }
    }
    XrtBackingTypes types={0}; MANAGED(xrt_backing_types_init(billing,&types));
    xr_lir_module_destroy(module); xr_lir_ownership_plan_destroy(plan); xr_lir_value_layouts_destroy(set);
    for (size_t f=0;f<functions;++f) { CHECK(homes[f]); check_projection(homes[f],billing,&types,workspace_kinds[f]); xr_lir_value_homes_destroy(homes[f]); }
    xrt_backing_types_drop(&types); CHECK(!live); XrtBillingStats stats=xrt_billing_owner_stats(billing); CHECK(stats.blocks==1); xrt_billing_owner_drop(billing);
}
int main(void) {
    const XrLirType elements[]={XR_LIR_I64,XR_LIR_STRING,XR_LIR_U8,XR_LIR_U64};
    for (size_t i=0;i<4;++i) {
        XrLirModule *module=NULL; OK(array_fixture_create(elements[i],&module)); project(module,false);
        module=NULL; OK(array_ref_fixture(elements[i],&module)); project(module,false);
        module=NULL; OK(array_repeat_fixture(elements[i],&module)); project(module,false);
        project(roles_fixture(elements[i],false),true);
    }
    project(loop_fixture(),true);
    project(map_fixture(1),true); project(map_state_fixture(),false); project(roles_fixture(XR_LIR_I64,true),true);
    CHECK(kinds==((1u<<XR_LIR_HOME_BINDING_LOCAL)|(1u<<XR_LIR_HOME_BINDING_PARAMETER)|
        (1u<<XR_LIR_HOME_BINDING_CELL_BORROW)|(1u<<XR_LIR_HOME_BINDING_STATE_BORROW)));
    CHECK(array_roles==254 && map_roles==255); array_cell_cfg(); array_block_cells();
    puts("binding homes: four Array families, Map roles, real pending slots, loop lifetime, input retirement and allocation rollback passed");
    return 0;
}
