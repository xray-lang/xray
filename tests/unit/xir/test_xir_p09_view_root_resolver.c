/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_view_root_resolver.c - Actual symbolic roots and storage paths
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_view_prepare_internal.h"
#include "xir/xxir_view_roots_internal.h"
#include "tuple_owner_observer.h"
enum { Q_ARRAY=256,Q_CELL,Q_SHARED,Q_MUT,Q_NESTED,Q_NESTED_CELL,Q_MI,Q_MI_CELL,
    Q_MF,Q_MF_CELL,Q_MS,Q_MS_CELL,Q_PAIR,Q_MT,Q_MT_CELL,Q_OBJ,Q_OPT,Q_WRAP,Q_REF_CALL,Q_CLOSURE,Q_KINDS=33 };
typedef struct ResolveFixture {
    XrXirTypeNode nodes[96];XrXirCallableParameter fields[96][3],ref;
    XrXirNominalField object_fields[2];XrXirNominalDeclaration nominal;XrXirNominalTable nominals;
    XrXirTypes types;XrXirType parameters[4][4],object_types[2];
    XrXirInstruction ops[4][192];XrXirBlock blocks[4];uint32_t operands[4][768];
    XrXirFunction functions[4];XrXirFunctionIdentity identities[4];XrXirSourceModule source;
    XrXirSlot slots[3];XrXirLiteral literals[3];XrXirDeclarations declarations;XrXirModule module;
    uint32_t bounds[2];XirViewRootRequest left,right;bool disjoint,rejected;
} ResolveFixture;
static uint32_t resolve_op(ResolveFixture *f,uint32_t body,XrXirInstruction op) {
    CHECK(f->functions[body].instruction_count<192);uint32_t i=f->functions[body].instruction_count++;
    f->ops[body][i]=op;return f->functions[body].parameter_count+i;
}
static uint32_t resolve_group(ResolveFixture *f,uint32_t body,const uint32_t *values,uint32_t count) {
    uint32_t at=f->functions[body].operand_count;CHECK(at+count<=768);
    if (count) memcpy(f->operands[body]+at,values,(size_t)count*sizeof(*values));
    f->functions[body].operand_count+=count;return at;
}
static void resolve_base(ResolveFixture *f) {
    memset(f,0,sizeof(*f));
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)Q_ARRAY};
    f->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_I64};
    f->nodes[3]=(XrXirTypeNode){.kind=XR_XIR_TYPE_MUT_SLICE,.element=XR_XIR_I64};
    f->nodes[4]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)Q_ARRAY};
    f->nodes[5]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)Q_NESTED};
    const XrXirType keys[]={XR_XIR_I64,XR_XIR_F64,XR_XIR_STRING,(XrXirType)Q_PAIR};
    for (uint32_t i=0;i<4;++i) {
        uint32_t at=i==3?13u:6u+i*2u;
        f->nodes[at]=(XrXirTypeNode){.kind=XR_XIR_TYPE_MAP,.element=keys[i],.result=(XrXirType)Q_ARRAY};
        f->nodes[at+1u]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)(256u+at)};
    }
    f->fields[12][0]=(XrXirCallableParameter){XR_XIR_UNIT,XR_PARAM_READ};
    f->fields[12][1]=(XrXirCallableParameter){XR_XIR_I64,XR_PARAM_READ};
    f->fields[12][2]=(XrXirCallableParameter){XR_XIR_F64,XR_PARAM_READ};
    f->nodes[12]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=f->fields[12],.parameter_count=3};
    for (uint32_t i=0;i<2;++i) {
        f->object_types[i]=(XrXirType)Q_ARRAY;
        f->object_fields[i]=(XrXirNominalField){{i?"right":"left",i?5u:4u},(XrXirType)Q_ARRAY,XR_XIR_FIELD_MUTABLE};
    }
    f->nominal=(XrXirNominalDeclaration){.module={"root",4},.name={"Holder",6},.exported=1,
        .kind=XR_XIR_NOMINAL_CLASS,.flags=XR_XIR_NOMINAL_FINAL,.fields=f->object_fields,.field_count=2};
    f->nominals=(XrXirNominalTable){&f->nominal,1,NULL};
    f->nodes[15]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,.nominal={0,NULL,0,f->object_types,2}};
    f->nodes[16]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)Q_SHARED};
    f->fields[17][0]=(XrXirCallableParameter){XR_XIR_UNIT,XR_PARAM_READ};
    f->fields[17][1]=(XrXirCallableParameter){(XrXirType)Q_SHARED,XR_PARAM_READ};
    f->fields[17][2]=(XrXirCallableParameter){(XrXirType)Q_OPT,XR_PARAM_READ};
    f->nodes[17]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=f->fields[17],.parameter_count=3};
    f->ref=(XrXirCallableParameter){(XrXirType)Q_CELL,XR_PARAM_REF};
    f->nodes[18]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.parameters=&f->ref,.parameter_count=1,
        .result=(XrXirType)Q_SHARED,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    f->nodes[19]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    f->types=(XrXirTypes){f->nodes,20,&f->nominals,NULL};
    for (uint32_t i=0;i<4;++i) f->functions[i]=(XrXirFunction){.name="body",.name_length=4,
        .blocks=f->blocks+i,.block_count=1,.instructions=f->ops[i]};
    f->source=(XrXirSourceModule){.name="root",.name_length=4,.initializer=3};
    f->literals[0]=(XrXirLiteral){"a\0b",3};f->literals[1]=(XrXirLiteral){"a\0b",3};f->literals[2]=(XrXirLiteral){"a\0c",3};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,
        .literals=f->literals,.literal_count=3,.root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=4,
        .declarations=&f->declarations,.types=&f->types,.linkage_kind=XR_XIR_LIBRARY};
}
static void resolve_parameters(ResolveFixture *f,uint32_t body,XrXirType first,XrXirType key) {
    f->parameters[body][0]=first;f->parameters[body][1]=f->parameters[body][2]=key;
    f->functions[body].parameter_count=key==XR_XIR_UNIT?1u:3u;f->functions[body].parameters=f->parameters[body];
}
static uint32_t resolve_view(ResolveFixture *f,uint32_t cell) {
    uint32_t group=resolve_group(f,0,(uint32_t[]){cell,f->bounds[0],f->bounds[1]},3);
    return resolve_op(f,0,(XrXirInstruction){.op=XR_XIR_VIEW_NEW,.type=(XrXirType)Q_SHARED,.args={group,3}});
}
static uint32_t resolve_index(ResolveFixture *f,XrXirType container,uint32_t key) {
    uint32_t base=resolve_op(f,0,(XrXirInstruction){.op=XR_XIR_CELL_PLACE,.type=container});
    uint32_t place=resolve_op(f,0,(XrXirInstruction){.op=XR_XIR_INDEX_PLACE,.type=(XrXirType)Q_ARRAY,.args={base,key}});
    uint32_t cell=resolve_op(f,0,(XrXirInstruction){.op=XR_XIR_CELL_PROJECT,.type=(XrXirType)Q_CELL,.args={place}});
    return resolve_view(f,cell);
}
static uint32_t resolve_tuple(ResolveFixture *f,uint32_t integer,uint32_t number) {
    uint32_t at=resolve_group(f,0,(uint32_t[]){integer,number},2);
    return resolve_op(f,0,(XrXirInstruction){.op=XR_XIR_TUPLE_NEW,.type=(XrXirType)Q_PAIR,.args={at,2}});
}
static void resolve_keys(ResolveFixture *f,uint32_t kind,uint32_t *a,uint32_t *b) {
    XrXirType type=kind<=6 || kind==29?XR_XIR_I64:kind==10 || kind==11 || kind==30?XR_XIR_STRING:
        kind==27?XR_XIR_F32:XR_XIR_F64;
    XrXirOp op=type==XR_XIR_STRING?XR_XIR_CONST_STRING:xr_xir_float_bits(type)?XR_XIR_CONST_FLOAT:XR_XIR_CONST_INT;
    int64_t left=0,right=1;
    if (kind==4) right=0;
    if (kind==7 || kind==27) right=kind==27?INT64_C(0x80000000):INT64_MIN;
    if (kind==12 || kind==13) right=INT64_MIN;
    if (kind==8 || kind==9) {left=INT64_C(0x7ff8000000000000);right=kind==8?left:INT64_C(0x3ff0000000000000);}
    if (kind==10 || kind==30) right=1;if (kind==11) right=2;
    *a=resolve_op(f,0,(XrXirInstruction){.op=op,.type=type,.immediate=left});
    *b=resolve_op(f,0,(XrXirInstruction){.op=op,.type=type,.immediate=right});
    if (kind==5 || kind==28) {*a=1;*b=2;}
    if (kind==29 || kind==30) *b=resolve_op(f,0,(XrXirInstruction){
        .op=kind==29?XR_XIR_SCALAR_COPY:XR_XIR_OWNED_RETAIN,.type=type,.args={*a}});
    if (kind==12 || kind==13) {
        uint32_t one=resolve_op(f,0,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=1});
        uint32_t two=resolve_op(f,0,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=kind==13?2:1});
        *a=resolve_tuple(f,one,*a);*b=resolve_tuple(f,two,*b);
    }
}
static void resolve_module(ResolveFixture *f,uint32_t kind,uint32_t *left,uint32_t *right) {
    f->declarations.slots=f->slots;f->declarations.slot_count=kind==19?3u:2u;
    for (uint32_t i=0;i<f->declarations.slot_count;++i)
        f->slots[i]=(XrXirSlot){.module=0,.type=(XrXirType)Q_CELL,.mutable=1};
    if (kind==19) f->slots[0]=(XrXirSlot){.type=XR_XIR_UNIT};
    uint32_t array=resolve_op(f,3,(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)Q_ARRAY});
    uint32_t a=resolve_op(f,3,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)Q_CELL,.args={array}});
    uint32_t b=resolve_op(f,3,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)Q_CELL,.args={array}});
    if (kind==19) {
        uint32_t at=resolve_group(f,3,(uint32_t[]){a,b},2);
        resolve_op(f,3,(XrXirInstruction){.op=XR_XIR_SLOT_GROUP_INIT,.args={at,2},.immediate=3});
    } else {
        resolve_op(f,3,(XrXirInstruction){.op=XR_XIR_SLOT_INIT,.args={a}});
        resolve_op(f,3,(XrXirInstruction){.op=XR_XIR_SLOT_INIT,.args={kind==17?a:b},.immediate=1});
    }
    if (kind==18) {
        uint32_t value=resolve_op(f,2,(XrXirInstruction){.op=XR_XIR_SLOT_LOAD,.type=(XrXirType)Q_CELL});
        resolve_op(f,2,(XrXirInstruction){.op=XR_XIR_SLOT_STORE,.args={value},.immediate=1});
    }
    *left=resolve_op(f,0,(XrXirInstruction){.op=XR_XIR_SLOT_LOAD,.type=(XrXirType)Q_CELL,.immediate=kind==19?1:0});
    *right=resolve_op(f,0,(XrXirInstruction){.op=XR_XIR_SLOT_LOAD,.type=(XrXirType)Q_CELL,.immediate=kind==19?2:1});
}
static void resolve_dag(ResolveFixture *f,uint32_t kind,uint32_t *a,uint32_t *b) {
    uint32_t left=resolve_op(f,0,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)Q_CELL});
    uint32_t right=resolve_op(f,0,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)Q_CELL});
    *a=resolve_view(f,left);*b=resolve_view(f,kind==26?right:left);
    XrXirType previous=(XrXirType)Q_SHARED;
    for (uint32_t i=20;i<90;++i) {
        f->fields[i][0]=(XrXirCallableParameter){XR_XIR_UNIT,XR_PARAM_READ};
        f->fields[i][1]=f->fields[i][2]=(XrXirCallableParameter){previous,XR_PARAM_READ};
        f->nodes[i]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=f->fields[i],.parameter_count=3};
        uint32_t at=resolve_group(f,0,(uint32_t[]){*a,*a},2);
        *a=resolve_op(f,0,(XrXirInstruction){.op=XR_XIR_TUPLE_NEW,.type=(XrXirType)(256u+i),.args={at,2}});
        at=resolve_group(f,0,(uint32_t[]){*b,*b},2);
        *b=resolve_op(f,0,(XrXirInstruction){.op=XR_XIR_TUPLE_NEW,.type=(XrXirType)(256u+i),.args={at,2}});
        previous=(XrXirType)(256u+i);
    }
    f->types.count=90;
}
static void resolve_special(ResolveFixture *f,uint32_t kind,uint32_t *a,uint32_t *b) {
    if (kind>=16 && kind<=19) {resolve_module(f,kind,a,b);return;}
    *a=resolve_op(f,0,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)Q_CELL,.args={kind==21?1u:0u}});
    *b=resolve_op(f,0,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)Q_CELL,.args={kind==21?1u:0u}});
    if (kind==1) *b=resolve_op(f,0,(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)Q_CELL,.args={*a}});
    if (kind==20) {
        f->functions[1].parameter_count=2;f->functions[1].parameters=f->parameters[1];
        f->parameters[1][0]=f->parameters[1][1]=(XrXirType)Q_CELL;
        uint32_t at=resolve_group(f,0,(uint32_t[]){*a,*b},2);
        resolve_op(f,0,(XrXirInstruction){.op=XR_XIR_CALL,.args={at,2},.immediate=1});
        at=resolve_group(f,0,(uint32_t[]){*b,*a},2);
        resolve_op(f,0,(XrXirInstruction){.op=XR_XIR_CALL,.args={at,2},.immediate=1});*a=0;*b=1;
    } else if (kind==21) {
        uint32_t at=resolve_group(f,0,a,1);
        *a=resolve_op(f,0,(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=(XrXirType)Q_SHARED,.args={at,1}});
    } else if (kind==22) *a=resolve_op(f,0,(XrXirInstruction){.op=XR_XIR_NULLABLE_NONE,.type=(XrXirType)Q_OPT});
    else if (kind==24) {
        f->functions[2].parameter_count=1;f->functions[2].parameters=f->parameters[2];f->parameters[2][0]=(XrXirType)Q_CELL;
        uint32_t at=resolve_group(f,0,a,1);
        resolve_op(f,0,(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)Q_CLOSURE,.args={at,1},.immediate=2});
    }
}
static void resolve_case(ResolveFixture *f,uint32_t kind) {
    resolve_base(f);uint32_t a=0,b=0;XrXirType first=(XrXirType)Q_ARRAY,key=XR_XIR_UNIT;
    if (kind==29 || kind==30) {
        f->module.stage=XR_XIR_LOWERED;f->types.count=12;f->types.nominals=NULL;
        f->module.linkage_kind=XR_XIR_PROGRAM;f->declarations.root_module=0;f->declarations.entry_function=2;
        f->functions[2].result=XR_XIR_I64;resolve_op(f,2,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64});
    }
    bool indexed=(kind>=3 && kind<=13) || (kind>=27 && kind<=30);
    if (indexed) {
        first=kind<=5?(XrXirType)Q_NESTED_CELL:kind==6 || kind==29?(XrXirType)Q_MI_CELL:
            kind==10 || kind==11 || kind==30?(XrXirType)Q_MS_CELL:kind==12 || kind==13?(XrXirType)Q_MT_CELL:
            (XrXirType)Q_MF_CELL;
        key=kind==28?XR_XIR_F64:XR_XIR_I64;
        if (kind==27) f->nodes[8].element=XR_XIR_F32;
    } else if (kind==2) {first=(XrXirType)Q_CELL;key=(XrXirType)Q_CELL;}
    else if (kind==14 || kind==15) {first=(XrXirType)Q_OBJ;key=(XrXirType)Q_OBJ;}
    else if (kind==21) {first=(XrXirType)Q_REF_CALL;key=(XrXirType)Q_ARRAY;}
    else if (kind==23) first=(XrXirType)Q_WRAP;
    else if (kind==31 || kind==32) {first=kind==31?(XrXirType)Q_ARRAY:(XrXirType)Q_CELL;key=(XrXirType)Q_OBJ;}
    resolve_parameters(f,0,first,key);
    f->bounds[0]=resolve_op(f,0,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64});
    f->bounds[1]=resolve_op(f,0,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=1});
    if (indexed) {
        resolve_keys(f,kind,&a,&b);
        XrXirType container=f->nodes[(uint32_t)first-256u].element;
        a=resolve_index(f,container,a);b=resolve_index(f,container,b);
    } else if (kind==25 || kind==26) {
        resolve_dag(f,kind,&a,&b);
    } else if (kind==2) {a=0;b=1;}
    else if (kind==14 || kind==15) {
        for (uint32_t i=0;i<2;++i) {
            uint32_t base=resolve_op(f,0,(XrXirInstruction){.op=XR_XIR_OBJECT_PLACE,.type=(XrXirType)Q_OBJ,.args={i}});
            uint32_t place=resolve_op(f,0,(XrXirInstruction){.op=XR_XIR_FIELD_PLACE,.type=(XrXirType)Q_ARRAY,
                .args={base},.immediate=i && kind==14?1:0});
            uint32_t cell=resolve_op(f,0,(XrXirInstruction){.op=XR_XIR_CELL_PROJECT,.type=(XrXirType)Q_CELL,.args={place}});
            if (i) b=resolve_view(f,cell);else a=resolve_view(f,cell);
        }
    } else if (kind==31 || kind==32) {
        uint32_t cell=kind==31?resolve_op(f,0,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)Q_CELL}):0;
        a=resolve_view(f,cell);
        uint32_t base=resolve_op(f,0,(XrXirInstruction){.op=XR_XIR_OBJECT_PLACE,.type=(XrXirType)Q_OBJ,.args={1}});
        uint32_t place=resolve_op(f,0,(XrXirInstruction){.op=XR_XIR_FIELD_PLACE,.type=(XrXirType)Q_ARRAY,.args={base}});
        cell=resolve_op(f,0,(XrXirInstruction){.op=XR_XIR_CELL_PROJECT,.type=(XrXirType)Q_CELL,.args={place}});
        b=resolve_view(f,cell);
    } else if (kind==23) {a=b=0;}
    else resolve_special(f,kind,&a,&b);
    uint32_t function=kind==20?1u:0u;
    f->left=(XirViewRootRequest){function,a,UINT32_MAX,NULL,0};f->right=(XirViewRootRequest){function,b,UINT32_MAX,NULL,0};
    f->disjoint=kind==0 || kind==3 || kind==6 || kind==9 || kind==11 || kind==14 ||
        kind==16 || kind==19 || kind==22 || kind==26 || kind==31;f->rejected=kind==24;
    for (uint32_t i=0;i<4;++i) {
        resolve_op(f,i,(XrXirInstruction){.op=XR_XIR_RETURN});f->blocks[i].count=f->functions[i].instruction_count;
        if (f->functions[i].operand_count) f->functions[i].operands=f->operands[i];
    }
}
typedef struct ResolveObserved { XirViewRootResolver *resolver;const XirViewRootRequest *request;uint32_t count; } ResolveObserved;
static XrXirStatus resolve_observe(void *user,const XrXirCompileContext *context,const XrXirViewRootEvent *event) {
    ResolveObserved *o=user;(void)context;CHECK(event->kind<=XR_XIR_VIEW_ROOT_UNRESOLVED);++o->count;
    CHECK(xir_compile_view_root_resolver_query(o->resolver,o->request,NULL,NULL,NULL)==XR_XIR_BAD_STAGE);
    bool disjoint=true;CHECK(xir_compile_view_roots_disjoint(o->resolver,o->request,o->request,&disjoint,NULL)==XR_XIR_BAD_STAGE && !disjoint);
    return XR_XIR_OK;
}
static XrXirStatus resolve_stop(void *user,const XrXirCompileContext *context,const XrXirViewRootEvent *event) {
    (void)context;(void)event;++*(uint32_t *)user;return XR_XIR_BAD_VALUE;
}
static XrXirStatus resolve_sequence(const XrXirCompileContext *context,uint32_t kind) {
    ResolveFixture f;resolve_case(&f,kind);uint32_t selection[]={2,UINT32_MAX},other=1;
    if (kind==23) {f.left.selection=selection;f.left.selection_count=2;f.right.selection=&other;f.right.selection_count=1;}
    unsigned char before[sizeof(f)];memcpy(before,&f,sizeof(f));XrXirConstruction *construction=NULL;
    XirViewRootResolver *resolver=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_construction(context,&f.types,&construction);if (status!=XR_XIR_OK) goto done;
    status=xir_compile_view_prepare_ordinary(context,&f.module,construction,&diagnostic);
    if (status==XR_XIR_BUDGET || status==XR_XIR_OUT_OF_MEMORY) goto done;
    if (kind==12 || kind==13) {CHECK(status==XR_XIR_BAD_TYPE);goto done;}
    if (kind==18) {CHECK(status==XR_XIR_BAD_STRUCTURE);goto done;}
    if (status!=XR_XIR_OK) fprintf(stderr,"ordinary resolver kind=%u status=%u f=%u b=%u i=%u\n",kind,status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_OK);
    XrXirCompileContext bounded=*context;bounded.limits.frame_bytes=1;
    CHECK(xir_compile_view_root_resolver_new(&bounded,&f.module,&resolver,NULL)==XR_XIR_BUDGET && !resolver);
    status=xir_compile_view_root_resolver_new(context,&f.module,&resolver,&diagnostic);
    if (status!=XR_XIR_OK) goto done;
    ResolveObserved observed={resolver,&f.left,0};
    status=xir_compile_view_root_resolver_query(resolver,&f.left,resolve_observe,&observed,&diagnostic);
    if (status==XR_XIR_BUDGET || status==XR_XIR_OUT_OF_MEMORY) goto done;
    if (f.rejected) {CHECK(status==XR_XIR_BAD_VALUE);goto done;}
    CHECK(status==XR_XIR_OK && observed.count);if (kind==20) CHECK(observed.count==1);
    bool disjoint=!f.disjoint;status=xir_compile_view_roots_disjoint(resolver,&f.left,&f.right,&disjoint,&diagnostic);
    if (status!=XR_XIR_OK) {CHECK(!disjoint);goto done;}
    if (disjoint!=f.disjoint) fprintf(stderr,"resolver relation kind=%u actual=%u expected=%u\n",kind,disjoint,f.disjoint);
    CHECK(disjoint==f.disjoint);
    uint32_t stopped=0;status=xir_compile_view_root_resolver_query(resolver,&f.left,resolve_stop,&stopped,&diagnostic);
    if (status==XR_XIR_BUDGET || status==XR_XIR_OUT_OF_MEMORY) goto done;
    CHECK(status==XR_XIR_BAD_VALUE && stopped==1);
    status=xir_compile_view_root_resolver_query(resolver,&f.right,NULL,NULL,&diagnostic);
    if (status!=XR_XIR_OK) goto done;
    XirViewRootRequest invalid=f.left;invalid.value=UINT32_MAX;disjoint=true;
    CHECK(xir_compile_view_roots_disjoint(resolver,&invalid,&f.right,&disjoint,&diagnostic)==XR_XIR_BAD_STRUCTURE && !disjoint);
done:CHECK(!memcmp(before,&f,sizeof(f)));xir_compile_view_root_resolver_free(resolver);
    xr_xir_compile_construction_free(construction);return status;
}
static void resolve_resources(uint32_t kind,bool normal) {
    XrXirStatus expected=kind==12 || kind==13?XR_XIR_BAD_TYPE:kind==18?XR_XIR_BAD_STRUCTURE:
        kind==24?XR_XIR_BAD_VALUE:XR_XIR_OK;
    XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;size_t first=attempts;
    CHECK(resolve_sequence(&c,kind)==expected);size_t points=attempts-first;XrCompileResourceStats measured=stats(&c);
    CHECK(points && points<4096);CHECK(resolve_sequence(&c,kind)==expected);owner_free(&c,baseline);
    if (!normal) for (size_t p=0;p<points;++p) {
        c=owner_new(caps());baseline=stats(&c).live_bytes;fail_at=attempts+p;injected=false;
        XrXirStatus status=resolve_sequence(&c,kind);fail_at=SIZE_MAX;
        if (status!=XR_XIR_OUT_OF_MEMORY || !injected) fprintf(stderr,"resolver kind=%u FI=%zu/%zu status=%u\n",kind,p,points,status);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && injected);owner_free(&c,baseline);
    }
    if (!normal) for (uint32_t axis=0;axis<3;++axis) for (uint32_t delta=0;delta<3;++delta) {
        XrCompileResourceLimits limits=caps();uint64_t *bound=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        *bound=(axis==0?measured.allocated_bytes:axis==1?measured.peak_bytes:measured.work)+delta-1u;
        c=owner_new(limits);baseline=stats(&c).live_bytes;
        CHECK(resolve_sequence(&c,kind)==(delta?expected:XR_XIR_BUDGET));owner_free(&c,baseline);
    }
    printf("Root resolver kind=%u: %zu allocation points owner2/physical0%s PASS\n",kind,points,
        normal?"":"/all actual failures/three axes exact+minus1+plus1");
}
int main(int argc,char **argv) {
    bool normal=argc==2 && !strcmp(argv[1],"--normal");CHECK(argc==1 || normal);setvbuf(stdout,NULL,_IONBF,0);
    for (uint32_t kind=0;kind<Q_KINDS;++kind) resolve_resources(kind,normal);
    puts("Actual root replay, semantic Map selectors and unknown object identities grant no access permission");return 0;
}
