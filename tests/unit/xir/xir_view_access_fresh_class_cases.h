/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_view_access_fresh_class_cases.h - Fresh closed objects beside entry borrows
 */
#ifndef XIR_VIEW_ACCESS_FRESH_CLASS_CASES_H
#define XIR_VIEW_ACCESS_FRESH_CLASS_CASES_H

enum { FC_OBJECT=256,FC_CELL,FC_SHARED,FC_MUT,FC_INNER,FC_ARRAY,FC_OBJECT_ARRAY,FC_CAPTURE,FC_CALLABLE };
typedef enum FreshClassCase {
    FC_DIRECT,FC_FACTORY,FC_CLASS_COPY,FC_MUT_SCALAR,FC_LOOP_READ,FC_LOOP_ALLOCATION,FC_LOOP_CELL,
    FC_REBOUND,FC_CELL_COPY,FC_CELL_LOCAL_ALIAS,FC_CELL_CAPTURE,FC_CELL_CALL,FC_PHI_METADATA,FC_CLEANUP_METADATA,
    FC_OLD_FACTORY,FC_BRANCH_FACTORY,FC_NESTED_FIELD,FC_ARRAY_FIELD,FC_OBJECT_ARRAY_FIELD,
    FC_INDIRECT,FC_FACTORY_PUBLICATION,FC_CLASS_PHI,FC_CASE_COUNT
} FreshClassCase;
typedef struct FreshClassFixture {
    XrXirTypeNode nodes[9];XrXirNominalField fields[3],inner_field;
    XrXirNominalDeclaration nominals[2];XrXirNominalTable nominal_table;XrXirType field_types[3],inner_type;
    XrXirTypes types;
    XrXirType parameters[6][3];XrXirInstruction ops[6][96];XrXirBlock blocks[6][8];uint32_t operands[6][128];
    XrXirFunction functions[6];XrXirFunctionIdentity identities[6];
    XrXirSourceModule source;XrXirLiteral literal;XrXirSlot slot;XrXirDeclarations declarations;XrXirModule module;
    uint32_t cell,object,access;XrXirStatus expected;
} FreshClassFixture;
static uint32_t fresh_class_op(FreshClassFixture *f,uint32_t body,XrXirInstruction op) {
    XrXirFunction *fn=f->functions+body;CHECK(fn->instruction_count<96);
    uint32_t i=fn->instruction_count++;f->ops[body][i]=op;return fn->parameter_count+i;
}
static uint32_t fresh_class_group(FreshClassFixture *f,uint32_t body,const uint32_t *values,uint32_t count) {
    XrXirFunction *fn=f->functions+body;uint32_t at=fn->operand_count;CHECK(at+count<=128);
    if (count) memcpy(f->operands[body]+at,values,count*sizeof(*values));fn->operand_count+=count;return at;
}
static uint32_t fresh_class_new(FreshClassFixture *f,uint32_t body,uint32_t first,uint32_t second) {
    uint32_t values[3]={first,second,0},count=f->nominals[0].field_count;
    if (count==3) values[2]=fresh_class_op(f,body,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7});
    uint32_t at=fresh_class_group(f,body,values,count);
    return fresh_class_op(f,body,(XrXirInstruction){.op=XR_XIR_CLASS_NEW,.type=(XrXirType)FC_OBJECT,.args={at,count}});
}
static void fresh_class_base(FreshClassFixture *f,FreshClassCase kind) {
    memset(f,0,sizeof(*f));f->field_types[0]=f->field_types[1]=XR_XIR_STRING;f->field_types[2]=XR_XIR_I64;
    if (kind==FC_NESTED_FIELD) f->field_types[1]=(XrXirType)FC_INNER;
    if (kind==FC_ARRAY_FIELD) f->field_types[1]=(XrXirType)FC_ARRAY;
    if (kind==FC_OBJECT_ARRAY_FIELD) f->field_types[1]=(XrXirType)FC_OBJECT_ARRAY;
    for (uint32_t p=0;p<3;++p) f->fields[p]=(XrXirNominalField){{p==0?"first":p==1?"second":"number",p==0?5u:6u},f->field_types[p],XR_XIR_FIELD_MUTABLE};
    f->inner_field=(XrXirNominalField){{"text",4},XR_XIR_STRING,XR_XIR_FIELD_MUTABLE};f->inner_type=XR_XIR_STRING;
    f->nominals[0]=(XrXirNominalDeclaration){.module={"fixture",7},.name={"UnrelatedObject",15},.exported=1,
        .fields=f->fields,.field_count=kind==FC_MUT_SCALAR?3u:2u,.kind=XR_XIR_NOMINAL_CLASS,.flags=XR_XIR_NOMINAL_FINAL};
    f->nominals[1]=(XrXirNominalDeclaration){.module={"fixture",7},.name={"Nested",6},.exported=1,
        .fields=&f->inner_field,.field_count=1,.kind=XR_XIR_NOMINAL_CLASS,.flags=XR_XIR_NOMINAL_FINAL};
    f->nominal_table=(XrXirNominalTable){f->nominals,2,NULL};
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,.nominal={0,NULL,0,f->field_types,f->nominals[0].field_count}};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)FC_OBJECT};
    f->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_U8};
    f->nodes[3]=(XrXirTypeNode){.kind=XR_XIR_TYPE_MUT_SLICE,.element=XR_XIR_U8};
    f->nodes[4]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,.nominal={1,NULL,0,&f->inner_type,1}};
    f->nodes[5]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_U8};
    f->nodes[6]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)FC_INNER};
    f->nodes[7]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    f->nodes[8]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,.result=(XrXirType)FC_OBJECT,
        .flags=XR_XIR_CALLABLE_ROOT_NONE|XR_XIR_CALLABLE_NO_SUSPEND};
    f->types=(XrXirTypes){f->nodes,9,&f->nominal_table,NULL};
    for (uint32_t b=0;b<5;++b) f->functions[b]=(XrXirFunction){.name="work",.name_length=4,
        .parameters=f->parameters[b],.blocks=f->blocks[b],.block_count=1,.instructions=f->ops[b],
        .operands=f->operands[b],.error_type=XR_XIR_NEVER};
    f->parameters[0][0]=(XrXirType)(kind==FC_MUT_SCALAR?FC_MUT:FC_SHARED);f->parameters[0][1]=XR_XIR_BOOL;
    f->functions[0].parameter_count=2;
    if (kind==FC_NESTED_FIELD || kind==FC_ARRAY_FIELD || kind==FC_OBJECT_ARRAY_FIELD || kind==FC_CLASS_PHI) {
        f->parameters[0][2]=kind==FC_CLASS_PHI?(XrXirType)FC_OBJECT:f->field_types[1];f->functions[0].parameter_count=3;
    }
    f->parameters[2][0]=(XrXirType)FC_OBJECT;f->parameters[2][1]=f->parameters[0][0];f->functions[2].parameter_count=2;
    f->parameters[4][0]=(XrXirType)FC_CELL;f->functions[4].parameter_count=1;
    f->identities[1].promises=XR_XIR_FUNCTION_NO_SUSPEND;
    f->literal=(XrXirLiteral){"value",5};f->source=(XrXirSourceModule){.name="fixture",.name_length=7,.initializer=3};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,
        .literals=&f->literal,.literal_count=1,.root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=5,
        .declarations=&f->declarations,.types=&f->types,.linkage_kind=XR_XIR_LIBRARY};
    f->expected=kind==FC_DIRECT || kind==FC_FACTORY || kind==FC_CLASS_COPY || kind==FC_MUT_SCALAR ||
        kind==FC_LOOP_READ || kind==FC_PHI_METADATA || kind==FC_CLEANUP_METADATA?XR_XIR_OK:XR_XIR_UNRESOLVED;
}
static void fresh_class_factory(FreshClassFixture *f,FreshClassCase kind) {
    bool slot=kind==FC_OLD_FACTORY || kind==FC_BRANCH_FACTORY || kind==FC_FACTORY_PUBLICATION;
    if (slot) {
        f->slot=(XrXirSlot){0,(XrXirType)FC_OBJECT,kind==FC_FACTORY_PUBLICATION};f->declarations.slots=&f->slot;f->declarations.slot_count=1;
        uint32_t text=fresh_class_op(f,3,(XrXirInstruction){.op=XR_XIR_CONST_STRING,.type=XR_XIR_STRING});
        uint32_t object=fresh_class_new(f,3,text,text);
        fresh_class_op(f,3,(XrXirInstruction){.op=XR_XIR_SLOT_INIT,.args={object}});
    }
    fresh_class_op(f,3,(XrXirInstruction){.op=XR_XIR_RETURN});
    fresh_class_op(f,2,(XrXirInstruction){.op=XR_XIR_RETURN});
    fresh_class_op(f,4,(XrXirInstruction){.op=XR_XIR_RETURN});
    if (kind==FC_NESTED_FIELD || kind==FC_ARRAY_FIELD || kind==FC_OBJECT_ARRAY_FIELD) {
        fresh_class_op(f,1,(XrXirInstruction){.op=XR_XIR_RETURN});return;
    }
    f->functions[1].result=(XrXirType)FC_OBJECT;
    if (kind==FC_OLD_FACTORY) {
        f->functions[1].name="init";f->functions[1].name_length=4;
        f->identities[1].nominal_owner=1;f->identities[1].method_kind=XR_XIR_CONSTRUCTOR;
        uint32_t old=fresh_class_op(f,1,(XrXirInstruction){.op=XR_XIR_SLOT_LOAD,.type=(XrXirType)FC_OBJECT});
        fresh_class_op(f,1,(XrXirInstruction){.op=XR_XIR_RETURN,.args={old}});return;
    }
    uint32_t values[2];
    for (uint32_t p=0;p<2;++p) {
        uint32_t text=fresh_class_op(f,1,(XrXirInstruction){.op=XR_XIR_CONST_STRING,.type=XR_XIR_STRING});
        uint32_t local=fresh_class_op(f,1,(XrXirInstruction){.op=XR_XIR_LOCAL_UNINIT,.type=XR_XIR_STRING});
        fresh_class_op(f,1,(XrXirInstruction){.op=XR_XIR_LOCAL_WRITE,.args={local,text}});
        values[p]=fresh_class_op(f,1,(XrXirInstruction){.op=XR_XIR_LOCAL_READ,.type=XR_XIR_STRING,.args={local}});
    }
    if (kind==FC_FACTORY) {
        uint32_t scalar=fresh_class_op(f,1,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=1});
        fresh_class_op(f,1,(XrXirInstruction){.op=XR_XIR_COPY,.type=XR_XIR_I64,.args={scalar}});
    }
    if (kind==FC_FACTORY_PUBLICATION) {
        uint32_t published=fresh_class_new(f,1,values[0],values[1]);
        fresh_class_op(f,1,(XrXirInstruction){.op=XR_XIR_SLOT_STORE,.args={published}});
    }
    if (kind==FC_BRANCH_FACTORY) {
        uint32_t condition=fresh_class_op(f,1,(XrXirInstruction){.op=XR_XIR_CONST_BOOL,.type=XR_XIR_BOOL});
        fresh_class_op(f,1,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={condition},.targets={1,2}});
        f->blocks[1][0].count=f->functions[1].instruction_count;
        f->blocks[1][1].first=f->functions[1].instruction_count;
    }
    uint32_t object=fresh_class_new(f,1,values[0],values[1]);
    fresh_class_op(f,1,(XrXirInstruction){.op=XR_XIR_RETURN,.args={object}});
    if (kind==FC_BRANCH_FACTORY) {
        f->blocks[1][1].count=f->functions[1].instruction_count-f->blocks[1][1].first;
        f->blocks[1][2].first=f->functions[1].instruction_count;
        object=fresh_class_op(f,1,(XrXirInstruction){.op=XR_XIR_SLOT_LOAD,.type=(XrXirType)FC_OBJECT});
        fresh_class_op(f,1,(XrXirInstruction){.op=XR_XIR_RETURN,.args={object}});
        f->blocks[1][2].count=2;f->functions[1].block_count=3;
    }
}
static void fresh_class_alias(FreshClassFixture *f,FreshClassCase kind) {
    if (kind==FC_CELL_COPY) fresh_class_op(f,0,(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)FC_CELL,.args={f->cell}});
    if (kind==FC_CELL_LOCAL_ALIAS) fresh_class_op(f,0,(XrXirInstruction){.op=XR_XIR_LOCAL_NEW,.type=(XrXirType)FC_CELL,.args={f->cell}});
    if (kind==FC_CLEANUP_METADATA) {
        for (uint32_t p=0;p<6;++p)
            fresh_class_op(f,0,(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)FC_SHARED});
        uint32_t at=fresh_class_group(f,0,&f->cell,1);
        fresh_class_op(f,0,(XrXirInstruction){.op=XR_XIR_VIEW_CLEANUP,.args={at,1}});
    }
}
static void fresh_class_phi_metadata(FreshClassFixture *f) {
    /* The actual predecessor number equals the Cell SSA identity. */
    uint32_t predecessor=f->cell;CHECK(predecessor+1u<8);
    fresh_class_op(f,0,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}});
    f->blocks[0][0].count=f->functions[0].instruction_count;
    for (uint32_t b=1;b<=predecessor;++b) {
        f->blocks[0][b].first=f->functions[0].instruction_count;f->blocks[0][b].count=1;
        fresh_class_op(f,0,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={b+1u}});
    }
    f->blocks[0][predecessor+1u].first=f->functions[0].instruction_count;
    uint32_t at=fresh_class_group(f,0,(uint32_t[]){predecessor,f->functions[0].parameter_count},2);
    fresh_class_op(f,0,(XrXirInstruction){.op=XR_XIR_PHI,.type=XR_XIR_STRING,.args={at,2}});
    f->functions[0].block_count=predecessor+2u;
}
static uint32_t fresh_class_receiver(FreshClassFixture *f,FreshClassCase kind) {
    uint32_t object=fresh_class_op(f,0,(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=(XrXirType)FC_OBJECT,.args={f->cell}});
    if (kind==FC_CLASS_COPY) object=fresh_class_op(f,0,(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)FC_OBJECT,.args={object}});
    if (kind!=FC_CLASS_PHI) return object;
    fresh_class_op(f,0,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={1},.targets={1,2}});
    f->blocks[0][0].count=f->functions[0].instruction_count;
    for (uint32_t b=1;b<3;++b) {
        f->blocks[0][b].first=f->functions[0].instruction_count;f->blocks[0][b].count=1;
        fresh_class_op(f,0,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3}});
    }
    f->blocks[0][3].first=f->functions[0].instruction_count;f->functions[0].block_count=4;
    uint32_t at=fresh_class_group(f,0,(uint32_t[]){1,object,2,2},4);
    return fresh_class_op(f,0,(XrXirInstruction){.op=XR_XIR_PHI,.type=(XrXirType)FC_OBJECT,.args={at,4}});
}
static void fresh_class_fixture(FreshClassFixture *f,FreshClassCase kind) {
    fresh_class_base(f,kind);fresh_class_factory(f,kind);
    bool loop=kind==FC_LOOP_READ || kind==FC_LOOP_ALLOCATION || kind==FC_LOOP_CELL;
    if (kind==FC_LOOP_ALLOCATION) {
        fresh_class_op(f,0,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}});
        f->blocks[0][0].count=1;f->blocks[0][1].first=1;
    }
    uint32_t first=fresh_class_op(f,0,(XrXirInstruction){.op=XR_XIR_CONST_STRING,.type=XR_XIR_STRING});
    uint32_t second=fresh_class_op(f,0,(XrXirInstruction){.op=XR_XIR_CONST_STRING,.type=XR_XIR_STRING});
    if (kind==FC_NESTED_FIELD || kind==FC_ARRAY_FIELD || kind==FC_OBJECT_ARRAY_FIELD) second=2;
    bool factory=kind==FC_FACTORY || kind==FC_CLASS_COPY || kind==FC_OLD_FACTORY || kind==FC_BRANCH_FACTORY || kind==FC_FACTORY_PUBLICATION;
    if (kind==FC_INDIRECT) {
        uint32_t callee=fresh_class_op(f,0,(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)FC_CALLABLE,.immediate=1});
        uint32_t at=fresh_class_group(f,0,&callee,1);
        f->object=fresh_class_op(f,0,(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=(XrXirType)FC_OBJECT,.args={at,1}});
    } else f->object=factory?fresh_class_op(f,0,(XrXirInstruction){.op=XR_XIR_CALL,.type=(XrXirType)FC_OBJECT,.immediate=1}):
        fresh_class_new(f,0,first,second);
    if (kind==FC_LOOP_CELL) {
        fresh_class_op(f,0,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}});
        f->blocks[0][0].count=f->functions[0].instruction_count;f->blocks[0][1].first=f->functions[0].instruction_count;
    }
    f->cell=fresh_class_op(f,0,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)FC_CELL,.args={f->object}});
    fresh_class_alias(f,kind);
    if (kind==FC_LOOP_READ) {
        fresh_class_op(f,0,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}});
        f->blocks[0][0].count=f->functions[0].instruction_count;f->blocks[0][1].first=f->functions[0].instruction_count;
    }
    if (kind==FC_PHI_METADATA) fresh_class_phi_metadata(f);
    uint32_t object=fresh_class_receiver(f,kind),at=fresh_class_group(f,0,(uint32_t[]){object,0},2);
    f->access=f->functions[0].instruction_count;
    fresh_class_op(f,0,(XrXirInstruction){.op=XR_XIR_CALL,.args={at,2},.immediate=2});
    fresh_class_op(f,0,(XrXirInstruction){.op=XR_XIR_VIEW_LEN,.type=XR_XIR_I64});
    if (loop) {
        fresh_class_op(f,0,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={1},.targets={1,2}});
        f->blocks[0][1].count=f->functions[0].instruction_count-f->blocks[0][1].first;
        f->blocks[0][2].first=f->functions[0].instruction_count;f->blocks[0][2].count=1;f->functions[0].block_count=3;
    }
    if (kind==FC_REBOUND) fresh_class_op(f,0,(XrXirInstruction){.op=XR_XIR_CELL_WRITE,.args={f->cell,f->object}});
    if (kind==FC_CELL_CALL) {
        uint32_t ref=fresh_class_group(f,0,&f->cell,1);
        fresh_class_op(f,0,(XrXirInstruction){.op=XR_XIR_CALL,.args={ref,1},.immediate=4});
    }
    if (kind==FC_CELL_CAPTURE) {
        uint32_t capture=fresh_class_group(f,0,&f->cell,1);
        fresh_class_op(f,0,(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)FC_CAPTURE,.args={capture,1},.immediate=4});
    }
    fresh_class_op(f,0,(XrXirInstruction){.op=XR_XIR_RETURN});
    for (uint32_t b=0;b<5;++b) {
        XrXirFunction *fn=f->functions+b;uint32_t last=fn->block_count-1u;
        fn->parameters=fn->parameter_count?f->parameters[b]:NULL;
        fn->operands=fn->operand_count?f->operands[b]:NULL;
        if (!f->blocks[b][last].count) f->blocks[b][last].count=fn->instruction_count-f->blocks[b][last].first;
    }
}
static XrXirNominalTable *fresh_class_physical(const XrXirCompileContext *context,FreshClassFixture *f) {
    CHECK(xr_xir_compile_types_structure_verify(context,&f->types)==XR_XIR_OK);
    XrXirNominalTable *identities=NULL;
    CHECK(xr_xir_compile_nominal_project(context,&f->nominal_table,&identities)==XR_XIR_OK);
    f->types.nominals=identities;f->module.stage=XR_XIR_LOWERED;f->module.linkage_kind=XR_XIR_PROGRAM;
    f->declarations.root_module=0;f->declarations.entry_function=5;f->module.function_count=6;
    f->functions[5]=(XrXirFunction){.name="entry",.name_length=5,.result=XR_XIR_I64,.error_type=XR_XIR_NEVER,
        .blocks=f->blocks[5],.block_count=1,.instructions=f->ops[5]};
    uint32_t zero=fresh_class_op(f,5,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64});
    fresh_class_op(f,5,(XrXirInstruction){.op=XR_XIR_RETURN,.args={zero}});
    f->blocks[5][0].count=f->functions[5].instruction_count;
    for (uint32_t b=0;b<f->module.function_count;++b) for (uint32_t i=0;i<f->functions[b].instruction_count;++i) {
        XrXirInstruction *op=f->ops[b]+i;
        if (op->op==XR_XIR_COPY) op->op=xr_xir_type_is_owned(&f->types,op->type)?XR_XIR_OWNED_RETAIN:XR_XIR_SCALAR_COPY;
        else if (op->op>=XR_XIR_LOCAL_NEW && op->op<=XR_XIR_LOCAL_WRITE) {
            XrXirType type=op->op==XR_XIR_LOCAL_WRITE?xr_xir_operand_type(f->functions+b,op->args[0]):op->type;
            op->op=(XrXirOp)((xr_xir_type_is_owned(&f->types,type)?XR_XIR_OWNED_LOCAL_NEW:XR_XIR_SCALAR_LOCAL_NEW)+op->op-XR_XIR_LOCAL_NEW);
        }
    }
    return identities;
}
static void fresh_class_case(FreshClassCase kind,bool physical) {
    FreshClassFixture f;fresh_class_fixture(&f,kind);
    XrXirCompileContext context=owner_new(caps());uint64_t baseline=stats(&context).live_bytes;
    XrXirNominalTable *identities=physical?fresh_class_physical(&context,&f):NULL;
    unsigned char before[sizeof(f)];memcpy(before,&f,sizeof(f));
    XrXirConstruction *construction=NULL;XrXirDiagnostic diagnostic={0};const char *stage="types and construction";
    XrXirStatus status=xir_fixture_module_construction(&context,&f.module,&construction);
    if (status==XR_XIR_OK) {stage="ordinary";status=xir_compile_view_prepare_ordinary(&context,&f.module,construction,&diagnostic);}
    if (status==XR_XIR_OK) {stage="return origins";status=xir_compile_view_return_origins_verified(&context,&f.module,&diagnostic);}
    if (status==XR_XIR_OK) {stage="affine";status=xir_compile_view_affine_verified(&context,&f.module,&diagnostic);}
    if (status!=XR_XIR_OK) fprintf(stderr,"fresh Class prefix case=%u physical=%u stage=%s status=%u at %u/%u/%u\n",kind,(unsigned)physical,stage,status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_OK);
    if (!physical && (kind==FC_FACTORY || kind==FC_ARRAY_FIELD)) {
        /* This small outer frame exercises resolver initialization rejection. */
        XrXirCompileContext denied=context;denied.limits.frame_bytes=64;
        CHECK(xir_compile_view_access_verified(&denied,&f.module,&diagnostic)==XR_XIR_BUDGET);
    }
    status=xir_compile_view_access_verified(&context,&f.module,&diagnostic);
    if (status!=f.expected || (status!=XR_XIR_OK && (diagnostic.function!=0 || diagnostic.instruction!=f.access)))
        fprintf(stderr,"fresh Class access case=%u physical=%u status=%u expected=%u at %u/%u/%u\n",kind,(unsigned)physical,status,f.expected,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==f.expected);
    if (status!=XR_XIR_OK) CHECK(diagnostic.function==0 && diagnostic.instruction==f.access);
    CHECK(!memcmp(before,&f,sizeof(f)));xr_xir_compile_construction_free(construction);
    xr_xir_compile_nominal_free(identities);owner_free(&context,baseline);
}
static void fresh_class_aggregate_rejected(void) {
    FreshClassFixture f;fresh_class_base(&f,FC_DIRECT);
    XrXirCallableParameter field={(XrXirType)FC_CELL,XR_PARAM_READ};
    f.nodes[7]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=&field,.parameter_count=1};
    XrXirCompileContext context=owner_new(caps());uint64_t baseline=stats(&context).live_bytes;
    CHECK(xr_xir_compile_types_structure_verify(&context,&f.types)==XR_XIR_BAD_TYPE);
    owner_free(&context,baseline);
}
static void fresh_class_cell_return_rejected(void) {
    FreshClassFixture f;fresh_class_fixture(&f,FC_DIRECT);f.functions[0].result=(XrXirType)FC_CELL;
    f.ops[0][f.functions[0].instruction_count-1u].args[0]=f.cell;
    XrXirCompileContext context=owner_new(caps());uint64_t baseline=stats(&context).live_bytes;
    XrXirConstruction *construction=NULL;XrXirDiagnostic diagnostic={0};
    CHECK(xir_fixture_module_construction(&context,&f.module,&construction)==XR_XIR_OK);
    CHECK(xir_compile_view_prepare_ordinary(&context,&f.module,construction,&diagnostic)==XR_XIR_BAD_TYPE);
    xr_xir_compile_construction_free(construction);owner_free(&context,baseline);
}
static void fresh_class_cases(void) {
    fresh_class_aggregate_rejected();
    fresh_class_cell_return_rejected();
    for (FreshClassCase kind=0;kind<FC_CASE_COUNT;++kind) {fresh_class_case(kind,false);fresh_class_case(kind,true);}
    puts("Fresh closed Class fields and stable private Cells keep unsupported aliases unresolved");
}
#endif // XIR_VIEW_ACCESS_FRESH_CLASS_CASES_H
