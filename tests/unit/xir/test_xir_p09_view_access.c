/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_view_access.c - Actual loan ancestry and retained-place conflicts
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_view_prepare_internal.h"
#include "xir/xxir_view_access_internal.h"
#include "xir/xxir_view_may_owned_internal.h"
#include "xir/xxir_view_types_internal.h"
#include "tuple_owner_observer.h"
enum { A_ARRAY=256,A_CELL,A_SHARED,A_MUT,A_OPTIONAL,A_TUPLE,A_NESTED,A_NESTED_CELL,A_OPTIONAL_MUT,
    A_MIXED,A_MUT_PAIR,A_NESTED_TUPLE,A_OPTIONAL_TUPLE,A_CALLABLE,A_OPTIONAL_MUT_PAIR,A_SINGLE_TUPLE,A_CASES=188 };
typedef struct AccessFixture {
    XrXirTypeNode nodes[86];XrXirCallableParameter fields[2],mixed[2],mut_pair[2],nested[2],single_field[2],entry[2],call,dag[70][3];
    XrXirTypes types;XrXirType parameters[3],callee,callees[2];
    XrXirInstruction ops[256],other[2];XrXirBlock blocks[80],single[2];uint32_t operands[256];
    XrXirFunction functions[3];XrXirSourceModule source;XrXirFunctionIdentity identities[3];
    XrXirDeclarations declarations;XrXirModule module;uint32_t root,zero,one,bad;XrXirStatus expected,prefix_expected;
} AccessFixture;
static uint32_t access_op(AccessFixture *f,XrXirInstruction op) {
    CHECK(f->functions[0].instruction_count<256);uint32_t i=f->functions[0].instruction_count++;
    f->ops[i]=op;return f->functions[0].parameter_count+i;
}
static uint32_t access_group(AccessFixture *f,const uint32_t *values,uint32_t count) {
    uint32_t at=f->functions[0].operand_count;CHECK(at+count<=256);
    if (count) memcpy(f->operands+at,values,(size_t)count*sizeof(*values));f->functions[0].operand_count+=count;return at;
}
static uint32_t access_view(AccessFixture *f,uint32_t source,bool child,bool mut) {
    uint32_t at=access_group(f,(uint32_t[]){source,f->zero,f->one},3);
    return access_op(f,(XrXirInstruction){.op=child?XR_XIR_VIEW_REBORROW:XR_XIR_VIEW_NEW,
        .type=(XrXirType)(mut?A_MUT:A_SHARED),.args={at,3}});
}
static void access_end(AccessFixture *f,uint32_t value) {access_op(f,(XrXirInstruction){.op=XR_XIR_VIEW_END,.args={value}});}
static void access_get(AccessFixture *f,uint32_t value) {
    access_op(f,(XrXirInstruction){.op=XR_XIR_VIEW_GET,.type=XR_XIR_I64,.args={value,f->zero}});
}
static void access_set(AccessFixture *f,uint32_t value) {
    uint32_t at=access_group(f,(uint32_t[]){value,f->zero,f->one},3);
    access_op(f,(XrXirInstruction){.op=XR_XIR_VIEW_SET,.args={at,3}});
}
static void access_bad(AccessFixture *f,XrXirStatus status) {f->bad=f->functions[0].instruction_count;f->expected=status;}
static void access_cell_read(AccessFixture *f) {access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=(XrXirType)A_ARRAY,.args={f->root}});}
static void access_cell_write(AccessFixture *f) {
    uint32_t array=access_op(f,(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)A_ARRAY});
    access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_WRITE,.args={f->root,array}});
}
static void access_base(AccessFixture *f,uint32_t kind) {
    memset(f,0,sizeof(*f));f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)A_ARRAY};
    f->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_I64};
    f->nodes[3]=(XrXirTypeNode){.kind=XR_XIR_TYPE_MUT_SLICE,.element=XR_XIR_I64};
    f->nodes[4]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)A_SHARED};
    f->fields[0]=f->fields[1]=(XrXirCallableParameter){(XrXirType)A_SHARED,XR_PARAM_READ};
    f->nodes[5]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=f->fields,.parameter_count=2};
    f->nodes[6]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)A_ARRAY};
    f->nodes[7]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)A_NESTED};
    f->nodes[8]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)A_MUT};
    f->mixed[0]=(XrXirCallableParameter){(XrXirType)A_MUT,XR_PARAM_READ};f->mixed[1]=f->fields[0];
    f->mut_pair[0]=f->mut_pair[1]=f->mixed[0];
    f->nested[0]=f->nested[1]=(XrXirCallableParameter){(XrXirType)A_TUPLE,XR_PARAM_READ};
    f->nodes[9]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=f->mixed,.parameter_count=2};
    f->nodes[10]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=f->mut_pair,.parameter_count=2};
    f->nodes[11]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=f->nested,.parameter_count=2};
    f->nodes[12]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)A_TUPLE};
    f->call=(XrXirCallableParameter){(XrXirType)A_OPTIONAL_MUT,XR_PARAM_READ};
    f->nodes[13]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,.parameters=&f->call,.parameter_count=1,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    f->nodes[14]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)A_MUT_PAIR};
    f->single_field[0]=(XrXirCallableParameter){XR_XIR_UNIT,XR_PARAM_READ};
    f->single_field[1]=(XrXirCallableParameter){(XrXirType)A_OPTIONAL_MUT,XR_PARAM_READ};
    f->nodes[15]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=f->single_field,.parameter_count=2};
    f->types=(XrXirTypes){f->nodes,16,NULL,NULL};f->parameters[0]=(XrXirType)(kind>=21 && kind<=24?A_NESTED_CELL:A_CELL);
    uint32_t count=1;if (kind>=27 && kind<=29) {count=2;f->parameters[1]=(XrXirType)(kind==27?A_CELL:A_SHARED);}
    if (kind==30 || kind==31 || kind==45 || kind==46 || kind>=74) {count=2;f->parameters[0]=XR_XIR_BOOL;f->parameters[1]=(XrXirType)A_CELL;f->root=1;}
    if (kind>=56 && kind<=59) f->parameters[0]=(XrXirType)(kind==56?A_TUPLE:kind==57?A_MIXED:kind==58?A_OPTIONAL:A_OPTIONAL_MUT);
    if (kind==72 || kind==73) f->parameters[0]=(XrXirType)(kind==72?A_MUT_PAIR:A_SINGLE_TUPLE);
    if (kind==66) {count=2;f->parameters[1]=(XrXirType)A_CELL;}
    if (kind>=102 && kind<116) {
        count=kind>=106 && kind<=108?1:2;
        f->parameters[0]=(XrXirType)(kind==103?A_SHARED:kind==106 || kind==109?A_MUT_PAIR:
            kind==107?A_MIXED:kind==108?A_OPTIONAL_MUT_PAIR:kind==110?A_CELL:A_MUT);
        f->parameters[1]=(XrXirType)(kind==104?A_SHARED:kind==109?A_MIXED:A_MUT);
        if (kind==110) f->root=0;
    }
    if (kind>=112 && kind<116) {
        count=2;f->parameters[0]=(XrXirType)A_CELL;
        f->parameters[1]=(XrXirType)(kind==113 || kind==115?A_MUT:A_CELL);f->root=0;
    }
    if (kind==122 || kind==123 || kind==130 || kind==131)
        f->parameters[1]=(XrXirType)(kind==122 || kind==130?A_SHARED:A_MUT);
    f->functions[0]=(XrXirFunction){.name="access",.name_length=6,.parameters=f->parameters,.parameter_count=count,
        .blocks=f->blocks,.block_count=1,.instructions=f->ops};
    for (uint32_t i=0;i<2;++i) {
        f->other[i]=(XrXirInstruction){.op=XR_XIR_RETURN};f->single[i]=(XrXirBlock){.count=1};
        f->functions[i+1]=(XrXirFunction){.name="body",.name_length=4,.blocks=f->single+i,.block_count=1,
            .instructions=f->other+i,.instruction_count=1};
    }
    if (kind>=112 && kind<115) {
        f->entry[0]=(XrXirCallableParameter){f->parameters[0],XR_PARAM_REF};
        f->entry[1]=(XrXirCallableParameter){f->parameters[1],kind==113?XR_PARAM_READ:XR_PARAM_REF};
        f->nodes[13].parameters=f->entry;f->nodes[13].parameter_count=2;
        f->other[0]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)A_CALLABLE,.immediate=0};
        f->functions[1].instruction_count=2;f->single[0].count=2;
    }
    for (uint32_t i=0;i<3;++i) f->functions[i].error_type=XR_XIR_NEVER;
    if (kind==64) f->functions[2].error_type=XR_XIR_ERROR;
    f->callee=(XrXirType)(kind==25?A_MUT:A_SHARED);f->functions[2].parameters=&f->callee;f->functions[2].parameter_count=1;
    f->source=(XrXirSourceModule){.name="root",.name_length=4,.initializer=1};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,
        .root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=3,
        .declarations=&f->declarations,.types=&f->types,.linkage_kind=XR_XIR_LIBRARY};
    f->zero=access_op(f,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64});
    f->one=access_op(f,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=1});f->bad=UINT32_MAX;
}
static void access_simple(AccessFixture *f,uint32_t kind) {
    bool mut=kind==2 || kind==3 || kind==9;uint32_t view=access_view(f,f->root,false,mut);
    if (kind==0) access_cell_read(f);
    else if (kind==1 || kind==3) {access_bad(f,XR_XIR_BAD_VALUE);++f->bad;access_cell_write(f);}
    else if (kind==2) {access_bad(f,XR_XIR_BAD_VALUE);access_cell_read(f);}
    else if (kind==4) {access_end(f,view);access_cell_write(f);}
    else if (kind==5 || kind==6) {
        uint32_t copy=access_op(f,(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)A_SHARED,.args={view}});access_end(f,view);
        if (kind==6) access_end(f,copy);else {access_bad(f,XR_XIR_BAD_VALUE);++f->bad;}access_cell_write(f);
    } else if (kind==7) {access_view(f,f->root,false,false);access_cell_read(f);}
    else {access_bad(f,XR_XIR_BAD_VALUE);access_view(f,f->root,false,kind==8);}
}
static void access_children(AccessFixture *f,uint32_t kind) {
    uint32_t parent=access_view(f,f->root,false,true),child=access_view(f,parent,true,kind>=14 && kind<=16);
    if (kind==10) access_get(f,parent);
    else if (kind==11 || kind==14) {access_bad(f,XR_XIR_BAD_VALUE);if (kind==11) access_set(f,parent);else access_get(f,parent);}
    else if (kind==12 || kind==16) {access_end(f,child);access_set(f,parent);}
    else if (kind==13) {
        access_op(f,(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)A_SHARED,.args={child}});access_end(f,child);
        access_bad(f,XR_XIR_BAD_VALUE);access_set(f,parent);
    } else if (kind==15) access_set(f,child);
    else if (kind==17) {access_end(f,parent);access_get(f,child);}
    else {
        access_end(f,child);child=access_view(f,parent,true,true);
        uint32_t grand=access_view(f,child,true,false);
        if (kind==32) {access_bad(f,XR_XIR_BAD_VALUE);access_get(f,parent);}
        else if (kind==33) {access_get(f,child);access_end(f,grand);access_end(f,child);access_set(f,parent);}
        else {access_end(f,parent);access_end(f,child);access_get(f,grand);}
    }
}
static void access_places(AccessFixture *f,uint32_t kind) {
    bool nested=kind>=21;XrXirType type=(XrXirType)(nested?A_NESTED:A_ARRAY);
    uint32_t place=access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_PLACE,.type=type,.args={f->root}}),other=place,root=f->root;
    if (nested) {
        place=access_op(f,(XrXirInstruction){.op=XR_XIR_INDEX_PLACE,.type=(XrXirType)A_ARRAY,.args={place,f->zero}});
        root=access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_PROJECT,.type=(XrXirType)A_CELL,.args={place}});
        other=access_op(f,(XrXirInstruction){.op=XR_XIR_INDEX_PLACE,.type=(XrXirType)A_ARRAY,
            .args={f->functions[0].parameter_count+2u,kind==22 || kind==24?f->one:f->zero}});
    }
    access_view(f,root,false,kind==20 || kind==23 || kind==24);
    if (kind==19 || kind==20 || kind==21 || kind==23) access_bad(f,XR_XIR_BAD_VALUE);
    if (kind==20) access_op(f,(XrXirInstruction){.op=XR_XIR_ARRAY_GET,.type=XR_XIR_I64,.args={place,f->zero}});
    else if (kind==21 || kind==22) access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_PROJECT,.type=(XrXirType)A_CELL,.args={other}});
    else {
        uint32_t at=access_group(f,(uint32_t[]){other,f->zero,f->one},3);access_op(f,(XrXirInstruction){.op=XR_XIR_ARRAY_SET,.args={at,3}});
    }
}
static void access_fresh(AccessFixture *f,uint32_t kind) {
    uint32_t array=access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=(XrXirType)A_ARRAY,.args={f->root}});
    uint32_t a=access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)A_CELL,.args={array}});
    uint32_t b=access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)A_CELL,.args={array}});
    if (kind==28) {access_view(f,a,false,true);return;}
    a=access_view(f,a,false,true);b=access_view(f,b,false,true);access_set(f,a);access_set(f,b);
}
static void access_phi(AccessFixture *f,bool loop) {
    if (loop) {
        uint32_t parent=access_view(f,f->root,false,true);access_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}});
        uint32_t first=f->functions[0].instruction_count,child=access_view(f,parent,true,true);
        access_bad(f,XR_XIR_BAD_VALUE);access_get(f,parent);access_end(f,child);
        access_op(f,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={1,2}});uint32_t end=f->functions[0].instruction_count;
        access_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});f->functions[0].block_count=3;
        f->blocks[0]=(XrXirBlock){.count=first};f->blocks[1]=(XrXirBlock){.first=first,.count=end-first};f->blocks[2]=(XrXirBlock){.first=end,.count=1};return;
    }
    uint32_t array=access_op(f,(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)A_ARRAY});
    uint32_t a=access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)A_CELL,.args={array}});
    uint32_t b=access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)A_CELL,.args={array}});
    a=access_view(f,a,false,true);b=access_view(f,b,false,true);
    access_op(f,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={1,2}});uint32_t first=f->functions[0].instruction_count;
    access_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3}});access_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3}});
    uint32_t join=f->functions[0].instruction_count,at=access_group(f,(uint32_t[]){1,a,2,b},4);
    a=access_op(f,(XrXirInstruction){.op=XR_XIR_PHI,.type=(XrXirType)A_MUT,.args={at,4}});
    at=access_group(f,(uint32_t[]){1,b,2,f->operands[at+1u]},4);
    b=access_op(f,(XrXirInstruction){.op=XR_XIR_PHI,.type=(XrXirType)A_MUT,.args={at,4}});
    access_set(f,a);access_set(f,b);access_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});f->functions[0].block_count=4;
    f->blocks[0]=(XrXirBlock){.count=first};f->blocks[1]=(XrXirBlock){.first=first,.count=1};
    f->blocks[2]=(XrXirBlock){.first=first+1u,.count=1};f->blocks[3]=(XrXirBlock){.first=join,.count=f->functions[0].instruction_count-join};
}
static void access_presence(AccessFixture *f,uint32_t kind) {
    uint32_t parent=access_view(f,f->root,false,true);
    uint32_t array=access_op(f,(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)A_ARRAY});
    uint32_t cell=access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)A_CELL,.args={array}});
    uint32_t other=access_view(f,cell,false,true);
    access_op(f,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={1,2}});
    uint32_t first=f->functions[0].instruction_count;access_view(f,kind==45?parent:other,true,false);
    access_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3}});uint32_t second=f->functions[0].instruction_count;
    access_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3}});uint32_t join=f->functions[0].instruction_count;
    uint32_t at=access_group(f,(uint32_t[]){1,other,2,parent},4);
    uint32_t selected=access_op(f,(XrXirInstruction){.op=XR_XIR_PHI,.type=(XrXirType)A_MUT,.args={at,4}});
    if (kind==46) access_bad(f,XR_XIR_UNRESOLVED);access_set(f,selected);
    access_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});f->functions[0].block_count=4;
    f->blocks[0]=(XrXirBlock){.count=first};f->blocks[1]=(XrXirBlock){.first=first,.count=second-first};
    f->blocks[2]=(XrXirBlock){.first=second,.count=join-second};
    f->blocks[3]=(XrXirBlock){.first=join,.count=f->functions[0].instruction_count-join};
}
static void access_wrappers(AccessFixture *f,uint32_t kind) {
    if (kind==35 || kind==42) {
        access_op(f,(XrXirInstruction){.op=XR_XIR_NULLABLE_NONE,.type=(XrXirType)A_OPTIONAL});
        if (kind==35) access_cell_write(f);else {uint32_t mut=access_view(f,f->root,false,true);access_set(f,mut);}return;
    }
    uint32_t parent=UINT32_MAX;
    if (kind==39 || kind==40 || kind==41 || kind==43) parent=access_view(f,f->root,false,true);
    uint32_t view=access_view(f,parent==UINT32_MAX?f->root:parent,parent!=UINT32_MAX,kind==43);
    if (kind==43) {
        access_op(f,(XrXirInstruction){.op=XR_XIR_NULLABLE_SOME,.type=(XrXirType)A_OPTIONAL_MUT,.args={view}});
        access_bad(f,XR_XIR_BAD_VALUE);access_get(f,parent);return;
    }
    bool tuple=kind==37 || kind==38 || kind==41;
    uint32_t wrapped=tuple?access_op(f,(XrXirInstruction){.op=XR_XIR_TUPLE_NEW,.type=(XrXirType)A_TUPLE,
        .args={access_group(f,(uint32_t[]){view,view},2),2}}):
        access_op(f,(XrXirInstruction){.op=XR_XIR_NULLABLE_SOME,.type=(XrXirType)A_OPTIONAL,.args={view}});
    access_end(f,view);
    if (kind==38 || kind==40) access_end(f,wrapped);
    if (kind==41) {
        access_end(f,parent);uint32_t field=access_op(f,(XrXirInstruction){.op=XR_XIR_TUPLE_FIELD,
            .type=(XrXirType)A_SHARED,.args={wrapped},.immediate=1});access_get(f,field);
    } else if (parent!=UINT32_MAX) {
        if (kind==39) access_bad(f,XR_XIR_BAD_VALUE);access_set(f,parent);
    } else {if (kind!=38) {access_bad(f,XR_XIR_BAD_VALUE);++f->bad;}access_cell_write(f);}
}
static uint32_t access_tuple(AccessFixture *f,XrXirType type,uint32_t left,uint32_t right) {
    uint32_t at=access_group(f,(uint32_t[]){left,right},2);
    return access_op(f,(XrXirInstruction){.op=XR_XIR_TUPLE_NEW,.type=type,.args={at,2}});
}
static uint32_t access_some(AccessFixture *f,XrXirType type,uint32_t value) {
    return access_op(f,(XrXirInstruction){.op=XR_XIR_NULLABLE_SOME,.type=type,.args={value}});
}
static uint32_t access_fresh_leaf(AccessFixture *f,bool mut) {
    uint32_t array=access_op(f,(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)A_ARRAY});
    uint32_t cell=access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)A_CELL,.args={array}});
    return access_view(f,cell,false,mut);
}
static uint32_t access_shared_dag(AccessFixture *f,uint32_t first,bool retire) {
    XrXirType previous=(XrXirType)A_SHARED;
    for (uint32_t i=0;i<70;++i) {
        f->dag[i][0]=(XrXirCallableParameter){XR_XIR_UNIT,XR_PARAM_READ};
        f->dag[i][1]=f->dag[i][2]=(XrXirCallableParameter){previous,XR_PARAM_READ};
        f->nodes[16u+i]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=f->dag[i],.parameter_count=3};
        previous=(XrXirType)(256u+16u+i);uint32_t next=access_tuple(f,previous,first,first);
        if (retire) access_end(f,first);first=next;
    }
    f->types.count=86;return first;
}
static uint32_t access_shared_argument(AccessFixture *f,uint32_t kind) {
    uint32_t first=access_view(f,f->root,false,false);
    if (kind==47) return access_some(f,(XrXirType)A_OPTIONAL,first);
    if (kind==71) return access_shared_dag(f,first,false);
    uint32_t second=kind==66?access_view(f,1,false,false):first;
    uint32_t tuple=access_tuple(f,(XrXirType)A_TUPLE,first,second);
    if (kind==60) return access_tuple(f,(XrXirType)A_NESTED_TUPLE,tuple,tuple);
    return kind==61?access_some(f,(XrXirType)A_OPTIONAL_TUPLE,tuple):tuple;
}
static uint32_t access_mutable_argument(AccessFixture *f,uint32_t kind) {
    uint32_t parent=access_view(f,f->root,false,true);
    if (kind==49 || kind==63 || kind==64 || kind==68) return access_some(f,(XrXirType)A_OPTIONAL_MUT,parent);
    if (kind==52 || kind==53 || kind==55 || kind==69) {
        uint32_t child=access_view(f,parent,true,false);
        if (kind==52) return access_tuple(f,(XrXirType)A_MIXED,parent,child);
        uint32_t tuple=kind==55?access_some(f,(XrXirType)A_OPTIONAL,child):access_tuple(f,(XrXirType)A_TUPLE,child,child);
        if (kind==53) return tuple;access_end(f,child);
        return kind==55?parent:access_some(f,(XrXirType)A_OPTIONAL_MUT,parent);
    }
    if (kind==54) parent=access_view(f,parent,true,true);
    if (kind==70) access_view(f,parent,true,false);
    bool mixed=kind==51 || kind==54;uint32_t other=access_fresh_leaf(f,!mixed);
    uint32_t pair=access_tuple(f,(XrXirType)(mixed?A_MIXED:A_MUT_PAIR),parent,other);
    return kind==62 || kind==70?access_some(f,(XrXirType)A_OPTIONAL_MUT_PAIR,pair):pair;
}
static void access_aggregate_call(AccessFixture *f,uint32_t value,uint32_t kind) {
    f->callee=xr_xir_operand_type(&f->functions[0],value);uint32_t values[2]={value,value},count=1,callee=2;
    if (kind==67 || kind==68) {
        count=2;f->callees[0]=f->callees[1]=f->callee;f->functions[2].parameters=f->callees;f->functions[2].parameter_count=2;
        if (kind==67) values[1]=access_op(f,(XrXirInstruction){.op=XR_XIR_COPY,.type=f->callee,.args={value}});
        else {f->prefix_expected=f->expected=XR_XIR_BAD_VALUE;}
    }
    if (kind==63) callee=access_op(f,(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)A_CALLABLE,.immediate=2});
    uint32_t at=access_group(f,values,count),instruction=f->functions[0].instruction_count;
    if (kind==68) f->bad=instruction;
    if (kind==52 || kind==70) access_bad(f,XR_XIR_UNRESOLVED);
    if (kind==55 || kind==69) access_bad(f,XR_XIR_BAD_VALUE);
    access_op(f,(XrXirInstruction){.op=kind==64?XR_XIR_INVOKE:kind==63?XR_XIR_CALL_INDIRECT:XR_XIR_CALL,
        .args={at,count},.targets={kind==64?1u:0u,kind==64?2u:0u},.immediate=callee});
    if (kind!=64) return;
    uint32_t first=f->functions[0].instruction_count;
    access_op(f,(XrXirInstruction){.op=XR_XIR_INVOKE_DISCARD,.immediate=instruction});access_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});
    access_op(f,(XrXirInstruction){.op=XR_XIR_INVOKE_ERROR,.type=XR_XIR_ERROR,.immediate=instruction});access_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});
    f->functions[0].block_count=3;f->blocks[0]=(XrXirBlock){.count=first};
    f->blocks[1]=(XrXirBlock){.first=first,.count=2};f->blocks[2]=(XrXirBlock){.first=first+2u,.count=2};
}
static void access_aggregate(AccessFixture *f,uint32_t kind) {
    uint32_t value=0;
    if ((kind>=56 && kind<=59) || kind==72 || kind==73) value=0;
    else if (kind==47 || kind==48 || kind==60 || kind==61 || kind==66 || kind==67 || kind==71)
        value=access_shared_argument(f,kind);
    else if (kind==65) value=access_op(f,(XrXirInstruction){.op=XR_XIR_NULLABLE_NONE,.type=(XrXirType)A_OPTIONAL_MUT});
    else value=access_mutable_argument(f,kind);
    access_aggregate_call(f,value,kind);
}
static void access_prefix_epoch(AccessFixture *f,uint32_t kind) {
    uint32_t parent=access_view(f,f->root,false,true),initial=0;
    if (kind==83) initial=access_view(f,parent,true,true);
    access_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}});
    uint32_t first=f->functions[0].instruction_count,child=0,split=0;
    if (kind==83) {
        uint32_t at=access_group(f,(uint32_t[]){0,initial,1,0},4);
        child=access_op(f,(XrXirInstruction){.op=XR_XIR_PHI,.type=(XrXirType)A_MUT,.args={at,4}});
        access_bad(f,XR_XIR_UNRESOLVED);access_set(f,child);
        f->operands[at+3u]=access_view(f,child,true,true);
    } else {
        bool mut=kind==74 || kind==77 || kind==78 || kind>=82;
        if (kind==87) access_bad(f,XR_XIR_UNRESOLVED);
        child=access_view(f,parent,true,mut);
        if (kind==74) access_set(f,child);
        else if (kind==75) access_get(f,parent);
        else if (kind==76) {access_bad(f,XR_XIR_BAD_VALUE);access_set(f,parent);}
        else if (kind==77) {access_end(f,child);access_set(f,parent);child=UINT32_MAX;}
        else if (kind==78) {
            uint32_t wrapped=access_some(f,(XrXirType)A_OPTIONAL_MUT,child);child=UINT32_MAX;
            access_bad(f,XR_XIR_BAD_VALUE);access_get(f,parent);access_end(f,wrapped);
        } else if (kind>=79 && kind<=81) {
            uint32_t wrapped=kind==79?access_op(f,(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)A_SHARED,.args={child}}):
                kind==80?access_some(f,(XrXirType)A_OPTIONAL,child):access_tuple(f,(XrXirType)A_TUPLE,child,child);
            access_end(f,child);child=UINT32_MAX;access_bad(f,XR_XIR_BAD_VALUE);access_set(f,parent);
            access_end(f,wrapped);
        } else if (kind==82) {
            access_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={2}});split=f->functions[0].instruction_count;
            access_bad(f,XR_XIR_BAD_VALUE);access_get(f,parent);
        } else if (kind==84 || kind==85) {
            uint32_t grand=access_view(f,child,true,true);
            if (kind==84) access_set(f,grand);else {access_bad(f,XR_XIR_BAD_VALUE);access_get(f,parent);}
            access_end(f,grand);
        } else if (kind==87) {access_some(f,(XrXirType)A_OPTIONAL_MUT,child);child=UINT32_MAX;}
        else access_op(f,(XrXirInstruction){.op=XR_XIR_VIEW_LEN,.type=XR_XIR_I64,.args={child}});
        if (child!=UINT32_MAX) access_end(f,child);
    }
    uint32_t exit=split?3:2;
    access_op(f,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={1,exit}});
    uint32_t end=f->functions[0].instruction_count;access_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});
    f->functions[0].block_count=exit+1u;f->blocks[0]=(XrXirBlock){.count=first};
    f->blocks[1]=(XrXirBlock){.first=first,.count=(split?split:end)-first};
    if (split) f->blocks[2]=(XrXirBlock){.first=split,.count=end-split};
    f->blocks[exit]=(XrXirBlock){.first=end,.count=1};
}
static void access_wrapped_epoch(AccessFixture *f,uint32_t kind) {
    uint32_t parent=kind==94?UINT32_MAX:access_view(f,f->root,false,true);
    access_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}});uint32_t first=f->functions[0].instruction_count;
    if (kind==94) {
        uint32_t array=access_op(f,(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)A_ARRAY});
        f->root=access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)A_CELL,.args={array}});
    }
    bool mut=kind>=90 && kind<=94;
    uint32_t child=access_view(f,kind==94?f->root:parent,kind!=94,mut),wrapped=child;
    if (kind==88) {
        uint32_t copy=access_op(f,(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)A_SHARED,.args={child}});
        uint32_t some=access_some(f,(XrXirType)A_OPTIONAL,copy);
        uint32_t leaf=access_op(f,(XrXirInstruction){.op=XR_XIR_NULLABLE_UNWRAP,.type=(XrXirType)A_SHARED,.args={some}});
        wrapped=access_op(f,(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)A_SHARED,.args={leaf}});
        access_end(f,child);access_end(f,copy);access_end(f,some);access_end(f,leaf);
        access_bad(f,XR_XIR_BAD_VALUE);access_set(f,parent);
    } else if (kind==89) {
        uint32_t tuple=access_tuple(f,(XrXirType)A_TUPLE,child,child);
        wrapped=access_op(f,(XrXirInstruction){.op=XR_XIR_TUPLE_FIELD,.type=(XrXirType)A_SHARED,.args={tuple},.immediate=1});
        access_end(f,child);access_end(f,tuple);access_get(f,wrapped);
    } else if (kind>=90 && kind<=92) {
        wrapped=access_some(f,(XrXirType)A_OPTIONAL_MUT,child);
        if (kind==91) {
            wrapped=access_op(f,(XrXirInstruction){.op=XR_XIR_TUPLE_NEW,.type=(XrXirType)A_SINGLE_TUPLE,
                .args={access_group(f,&wrapped,1),1}});
        }
        uint32_t argument=kind==92?parent:wrapped;f->callee=xr_xir_operand_type(f->functions,argument);
        if (kind==92) access_bad(f,XR_XIR_BAD_VALUE);
        access_op(f,(XrXirInstruction){.op=XR_XIR_CALL,.args={access_group(f,&argument,1),1},.immediate=2});
    } else if (kind==93) {
        uint32_t grand=access_view(f,child,true,false),tuple=access_tuple(f,(XrXirType)A_TUPLE,grand,grand);
        wrapped=access_some(f,(XrXirType)A_OPTIONAL_TUPLE,tuple);
        access_end(f,grand);access_end(f,tuple);access_end(f,child);access_bad(f,XR_XIR_BAD_VALUE);access_set(f,parent);
    } else if (kind==94) {access_get(f,child);access_bad(f,XR_XIR_BAD_VALUE);access_cell_read(f);}
    else {wrapped=access_shared_dag(f,child,true);access_bad(f,XR_XIR_BAD_VALUE);access_set(f,parent);}
    access_end(f,wrapped);
    if (kind==90 || kind==91) access_set(f,parent);
    access_op(f,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={1,2}});
    uint32_t end=f->functions[0].instruction_count;access_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});
    f->functions[0].block_count=3;f->blocks[0]=(XrXirBlock){.count=first};
    f->blocks[1]=(XrXirBlock){.first=first,.count=end-first};f->blocks[2]=(XrXirBlock){.first=end,.count=1};
}
static void access_root_epoch(AccessFixture *f,uint32_t kind) {
    uint32_t initial=0,phi=UINT32_MAX,at=0;
    if (kind==120 || kind==121 || kind==125) {
        uint32_t array=access_op(f,(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)A_ARRAY});
        initial=access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)A_CELL,.args={array}});
        if (kind==121) initial=access_view(f,initial,false,false);
    }
    access_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}});uint32_t first=f->functions[0].instruction_count;
    if (kind==120 || kind==121 || kind==125) {
        at=access_group(f,(uint32_t[]){0,initial,1,0},4);
        phi=access_op(f,(XrXirInstruction){.op=XR_XIR_PHI,.type=(XrXirType)(kind==121?A_SHARED:A_CELL),.args={at,4}});
    }
    uint32_t array=access_op(f,(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)A_ARRAY});
    f->root=access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)A_CELL,.args={array}});
    if (kind==116) f->root=access_op(f,(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)A_CELL,.args={f->root}});
    if (kind==124 || kind==125) {
        uint32_t local=access_op(f,(XrXirInstruction){.op=XR_XIR_LOCAL_NEW,.type=(XrXirType)A_CELL,.args={f->root}});
        if (kind==125) {
            f->operands[at+3u]=f->root;
            access_op(f,(XrXirInstruction){.op=XR_XIR_LOCAL_WRITE,.args={local,phi}});
        }
        f->root=access_op(f,(XrXirInstruction){.op=XR_XIR_LOCAL_READ,.type=(XrXirType)A_CELL,.args={local}});
    }
    if (kind==121) access_bad(f,XR_XIR_UNRESOLVED);
    uint32_t child=access_view(f,f->root,false,kind==116 || kind==120 || kind==121 || kind==122 || kind>=124);
    if (kind==116 || kind==124 || kind==125) {access_bad(f,kind==125?XR_XIR_UNRESOLVED:XR_XIR_BAD_VALUE);access_cell_read(f);}
    else if (kind==117 || kind==118) {
        if (kind==118) {
            uint32_t copy=access_op(f,(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)A_SHARED,.args={child}});
            access_end(f,child);child=copy;
        }
        access_bad(f,XR_XIR_BAD_VALUE);++f->bad;access_cell_write(f);
    } else if (kind==119) {access_end(f,child);child=UINT32_MAX;access_cell_write(f);}
    else if (kind==120) {
        f->operands[at+3u]=f->root;
        access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=(XrXirType)A_ARRAY,.args={phi}});
    } else if (kind>=122) {access_get(f,child);access_get(f,1);}
    if (child!=UINT32_MAX) access_end(f,child);
    if (kind==121) {f->operands[at+3u]=access_view(f,f->root,false,false);access_end(f,phi);}
    access_op(f,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={1,2}});
    uint32_t end=f->functions[0].instruction_count;access_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});
    f->functions[0].block_count=3;f->blocks[0]=(XrXirBlock){.count=first};
    f->blocks[1]=(XrXirBlock){.first=first,.count=end-first};f->blocks[2]=(XrXirBlock){.first=end,.count=1};
}
static void access_root_blocks(AccessFixture *f,uint32_t kind) {
    uint32_t initial=0,phi=UINT32_MAX,at=0,local=UINT32_MAX;
    if (kind>=133) {
        uint32_t array=access_op(f,(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)A_ARRAY});
        initial=access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)A_CELL,.args={array}});
        if (kind==135) initial=access_view(f,initial,false,false);
    }
    access_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}});uint32_t first=f->functions[0].instruction_count;
    if (kind>=133) {
        at=access_group(f,(uint32_t[]){0,initial,2,0},4);
        phi=access_op(f,(XrXirInstruction){.op=XR_XIR_PHI,.type=(XrXirType)(kind==135?A_SHARED:A_CELL),.args={at,4}});
    }
    uint32_t array=access_op(f,(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)A_ARRAY});
    f->root=access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)A_CELL,.args={array}});
    if (kind==132 || kind==133) {
        local=access_op(f,(XrXirInstruction){.op=XR_XIR_LOCAL_NEW,.type=(XrXirType)A_CELL,.args={f->root}});
        if (kind==133) {f->operands[at+3u]=f->root;access_op(f,(XrXirInstruction){.op=XR_XIR_LOCAL_WRITE,.args={local,phi}});}
    }
    uint32_t child=UINT32_MAX;
    if (kind<=129 || kind==134) child=access_view(f,f->root,false,kind==126 || kind==134);
    if (kind==129) {
        uint32_t copy=access_op(f,(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)A_SHARED,.args={child}});
        access_end(f,child);child=copy;
    }
    access_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={2}});uint32_t second=f->functions[0].instruction_count;
    if (kind==132 || kind==133) f->root=access_op(f,(XrXirInstruction){.op=XR_XIR_LOCAL_READ,.type=(XrXirType)A_CELL,.args={local}});
    if (kind==135) access_bad(f,XR_XIR_UNRESOLVED);
    if (child==UINT32_MAX) child=access_view(f,f->root,false,kind!=131);
    if (kind==126 || kind==132 || kind==133) {access_bad(f,kind==133?XR_XIR_UNRESOLVED:XR_XIR_BAD_VALUE);access_cell_read(f);}
    else if (kind==127 || kind==129) {access_bad(f,XR_XIR_BAD_VALUE);++f->bad;access_cell_write(f);}
    else if (kind==128) {access_end(f,child);child=UINT32_MAX;access_cell_write(f);}
    else if (kind==130 || kind==131) {access_get(f,child);access_get(f,1);}
    else if (kind==134) {
        f->operands[at+3u]=f->root;
        access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=(XrXirType)A_ARRAY,.args={phi}});
    }
    if (child!=UINT32_MAX) access_end(f,child);
    if (kind==135) {f->operands[at+3u]=access_view(f,f->root,false,false);access_end(f,phi);}
    access_op(f,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={1,3}});
    uint32_t end=f->functions[0].instruction_count;access_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});
    f->functions[0].block_count=4;f->blocks[0]=(XrXirBlock){.count=first};
    f->blocks[1]=(XrXirBlock){.first=first,.count=second-first};f->blocks[2]=(XrXirBlock){.first=second,.count=end-second};
    f->blocks[3]=(XrXirBlock){.first=end,.count=1};
}
static void access_root_panic(AccessFixture *f,uint32_t kind) {
    access_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}});uint32_t first=f->functions[0].instruction_count;
    uint32_t array=access_op(f,(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)A_ARRAY});
    f->root=access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)A_CELL,.args={array}});
    uint32_t held=access_view(f,f->root,false,false);
    if (kind==137) {
        uint32_t copy=access_op(f,(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)A_SHARED,.args={held}});
        access_end(f,held);held=copy;
    }
    access_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={2}});uint32_t second=f->functions[0].instruction_count;
    access_get(f,held);access_end(f,held);
    access_op(f,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={1,4}});
    uint32_t handler=f->functions[0].instruction_count;
    access_op(f,(XrXirInstruction){.op=XR_XIR_PANIC_CATCH,.type=XR_XIR_PANIC_INFO});
    access_bad(f,XR_XIR_BAD_VALUE);++f->bad;access_cell_write(f);access_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});
    uint32_t end=f->functions[0].instruction_count;access_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});
    f->functions[0].block_count=5;f->blocks[0]=(XrXirBlock){.count=first};
    f->blocks[1]=(XrXirBlock){.first=first,.count=second-first};
    f->blocks[2]=(XrXirBlock){.first=second,.count=handler-second,.panic=3};
    f->blocks[3]=(XrXirBlock){.first=handler,.count=end-handler};f->blocks[4]=(XrXirBlock){.first=end,.count=1};
}
static void access_scope_blocks(AccessFixture *f,uint32_t kind) {
    uint32_t parent=access_view(f,f->root,false,true);
    access_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}});uint32_t first=f->functions[0].instruction_count;
    uint32_t child=access_view(f,parent,true,kind==138 || kind>=144),held=child;
    if (kind==142) {
        held=access_op(f,(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)A_SHARED,.args={child}});
        access_end(f,child);
    } else if (kind==143) {
        held=access_tuple(f,(XrXirType)A_TUPLE,child,child);access_end(f,child);
    } else if (kind==145) held=access_view(f,child,true,false);
    access_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={2}});uint32_t second=f->functions[0].instruction_count;
    if (kind==138) {access_bad(f,XR_XIR_BAD_VALUE);access_get(f,parent);}
    else if (kind==145) {access_bad(f,XR_XIR_BAD_VALUE);access_set(f,child);}
    else if (kind==139) access_get(f,parent);
    else if (kind==144) access_set(f,child);
    else {
        if (kind==141) {access_end(f,held);held=UINT32_MAX;}
        else access_bad(f,XR_XIR_BAD_VALUE);
        access_set(f,parent);
    }
    if (held!=UINT32_MAX) access_end(f,held);
    if (kind==145) access_end(f,child);
    access_op(f,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={1,3}});
    uint32_t end=f->functions[0].instruction_count;access_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});
    f->functions[0].block_count=4;f->blocks[0]=(XrXirBlock){.count=first};
    f->blocks[1]=(XrXirBlock){.first=first,.count=second-first};f->blocks[2]=(XrXirBlock){.first=second,.count=end-second};
    f->blocks[3]=(XrXirBlock){.first=end,.count=1};
}
static void access_scope_panic(AccessFixture *f,uint32_t kind) {
    uint32_t parent=access_view(f,f->root,false,true);
    access_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}});uint32_t first=f->functions[0].instruction_count;
    uint32_t held=access_view(f,parent,true,kind==146);
    if (kind==147) {
        uint32_t copy=access_op(f,(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)A_SHARED,.args={held}});
        access_end(f,held);held=copy;
    }
    access_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={2}});uint32_t second=f->functions[0].instruction_count;
    access_get(f,held);access_end(f,held);
    access_op(f,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={1,4}});
    uint32_t handler=f->functions[0].instruction_count;
    access_op(f,(XrXirInstruction){.op=XR_XIR_PANIC_CATCH,.type=XR_XIR_PANIC_INFO});
    access_bad(f,XR_XIR_BAD_VALUE);
    if (kind==146) access_get(f,parent);else access_set(f,parent);
    access_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});
    uint32_t end=f->functions[0].instruction_count;access_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});
    f->functions[0].block_count=5;f->blocks[0]=(XrXirBlock){.count=first};
    f->blocks[1]=(XrXirBlock){.first=first,.count=second-first};
    f->blocks[2]=(XrXirBlock){.first=second,.count=handler-second,.panic=3};
    f->blocks[3]=(XrXirBlock){.first=handler,.count=end-handler};f->blocks[4]=(XrXirBlock){.first=end,.count=1};
}
static void access_root_phi(AccessFixture *f,uint32_t kind) {
    uint32_t initial=0,old=UINT32_MAX,old_at=0;
    if (kind==150 || kind==151) {
        uint32_t array=access_op(f,(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)A_ARRAY});
        initial=access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)A_CELL,.args={array}});
    }
    access_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}});uint32_t first=f->functions[0].instruction_count;
    if (kind==151) {
        old_at=access_group(f,(uint32_t[]){0,initial,4,0},4);
        old=access_op(f,(XrXirInstruction){.op=XR_XIR_PHI,.type=(XrXirType)A_CELL,.args={old_at,4}});
    }
    uint32_t array=access_op(f,(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)A_ARRAY});
    uint32_t root=access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)A_CELL,.args={array}});
    f->root=kind==149?1:root;uint32_t child=access_view(f,f->root,false,true);
    access_op(f,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={2,3}});
    uint32_t left=f->functions[0].instruction_count;
    uint32_t a;
    if (kind==155) {
        uint32_t nested=access_group(f,(uint32_t[]){1,root},2);
        a=access_op(f,(XrXirInstruction){.op=XR_XIR_PHI,.type=(XrXirType)A_CELL,.args={nested,2}});
    } else a=access_op(f,(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)A_CELL,.args={root}});
    access_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={4}});uint32_t right=f->functions[0].instruction_count;
    uint32_t b=access_op(f,(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)A_CELL,
        .args={kind==150?initial:kind==151?old:root}});
    access_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={4}});uint32_t merge=f->functions[0].instruction_count;
    uint32_t at=access_group(f,(uint32_t[]){2,a,3,b},4);
    uint32_t selected=access_op(f,(XrXirInstruction){.op=XR_XIR_PHI,.type=(XrXirType)A_CELL,.args={at,4}});
    if (kind==151) f->operands[old_at+3u]=root;
    if (kind==152) selected=access_op(f,(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)A_CELL,.args={selected}});
    if (kind==153 || kind==154) {
        uint32_t local=access_op(f,(XrXirInstruction){.op=XR_XIR_LOCAL_NEW,.type=(XrXirType)A_CELL,.args={selected}});
        if (kind==154) access_op(f,(XrXirInstruction){.op=XR_XIR_LOCAL_WRITE,.args={local,1}});
        selected=access_op(f,(XrXirInstruction){.op=XR_XIR_LOCAL_READ,.type=(XrXirType)A_CELL,.args={local}});
    }
    if (kind!=149 && kind!=154) access_bad(f,kind==150 || kind==151?XR_XIR_UNRESOLVED:XR_XIR_BAD_VALUE);
    access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=(XrXirType)A_ARRAY,.args={selected}});
    access_end(f,child);access_op(f,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={1,5}});
    uint32_t end=f->functions[0].instruction_count;access_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});
    f->functions[0].block_count=6;f->blocks[0]=(XrXirBlock){.count=first};
    f->blocks[1]=(XrXirBlock){.first=first,.count=left-first};f->blocks[2]=(XrXirBlock){.first=left,.count=right-left};
    f->blocks[3]=(XrXirBlock){.first=right,.count=merge-right};f->blocks[4]=(XrXirBlock){.first=merge,.count=end-merge};
    f->blocks[5]=(XrXirBlock){.first=end,.count=1};
}
static void access_deep_root_phi(AccessFixture *f,uint32_t kind) {
    access_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}});uint32_t first=f->functions[0].instruction_count;
    uint32_t array=access_op(f,(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)A_ARRAY});
    uint32_t root=access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)A_CELL,.args={array}});
    f->root=kind==157?1:root;uint32_t child=access_view(f,f->root,false,true);
    access_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={2}});
    f->blocks[0]=(XrXirBlock){.count=first};
    f->blocks[1]=(XrXirBlock){.first=first,.count=f->functions[0].instruction_count-first};
    uint32_t selected=root;
    for (uint32_t block=2;block<72;++block) {
        uint32_t start=f->functions[0].instruction_count;
        uint32_t at=access_group(f,(uint32_t[]){block-1u,selected},2);
        selected=access_op(f,(XrXirInstruction){.op=XR_XIR_PHI,.type=(XrXirType)A_CELL,.args={at,2}});
        access_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={block+1u}});
        f->blocks[block]=(XrXirBlock){.first=start,.count=2};
    }
    uint32_t last=f->functions[0].instruction_count;
    if (kind==156) access_bad(f,XR_XIR_BAD_VALUE);
    access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=(XrXirType)A_ARRAY,.args={selected}});
    access_end(f,child);access_op(f,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={1,73}});
    uint32_t end=f->functions[0].instruction_count;access_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});
    f->blocks[72]=(XrXirBlock){.first=last,.count=end-last};f->blocks[73]=(XrXirBlock){.first=end,.count=1};
    f->functions[0].block_count=74;
}
static void access_cyclic_root_phi(AccessFixture *f,uint32_t kind) {
    access_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}});uint32_t first=f->functions[0].instruction_count;
    uint32_t array=access_op(f,(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)A_ARRAY});
    uint32_t root=access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)A_CELL,.args={array}});
    f->root=kind==160?1:root;uint32_t child=UINT32_MAX;
    if (kind!=161) child=access_view(f,f->root,false,true);
    access_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={2}});uint32_t head=f->functions[0].instruction_count;
    uint32_t at=access_group(f,(uint32_t[]){1,root,3,0},4);
    uint32_t phi=access_op(f,(XrXirInstruction){.op=XR_XIR_PHI,.type=(XrXirType)A_CELL,.args={at,4}});
    uint32_t other=UINT32_MAX,other_at=0;
    if (kind>=164) {
        other_at=access_group(f,(uint32_t[]){1,kind==165?1:root,3,0},4);
        other=access_op(f,(XrXirInstruction){.op=XR_XIR_PHI,.type=(XrXirType)A_CELL,.args={other_at,4}});
    }
    if (kind==161) {
        array=access_op(f,(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)A_ARRAY});
        f->root=access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)A_CELL,.args={array}});
        child=access_view(f,f->root,false,true);
    }
    if (kind!=160 && kind!=161) access_bad(f,kind==163 || kind==165?XR_XIR_UNRESOLVED:XR_XIR_BAD_VALUE);
    access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=(XrXirType)A_ARRAY,.args={phi}});
    if (kind==161) access_end(f,child);
    access_op(f,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={3,4}});
    uint32_t back=f->functions[0].instruction_count,value=kind==163?1:kind==161?f->root:phi;
    if (kind==162) {
        uint32_t local=access_op(f,(XrXirInstruction){.op=XR_XIR_LOCAL_NEW,.type=(XrXirType)A_CELL,.args={value}});
        value=access_op(f,(XrXirInstruction){.op=XR_XIR_LOCAL_READ,.type=(XrXirType)A_CELL,.args={local}});
    } else value=access_op(f,(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)A_CELL,.args={value}});
    f->operands[at+3u]=kind>=164?other:value;
    if (kind>=164) f->operands[other_at+3u]=value;
    access_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={2}});
    uint32_t leave=f->functions[0].instruction_count;
    if (kind!=161) access_end(f,child);
    access_op(f,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={1,5}});
    uint32_t end=f->functions[0].instruction_count;access_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});
    f->functions[0].block_count=6;f->blocks[0]=(XrXirBlock){.count=first};
    f->blocks[1]=(XrXirBlock){.first=first,.count=head-first};f->blocks[2]=(XrXirBlock){.first=head,.count=back-head};
    f->blocks[3]=(XrXirBlock){.first=back,.count=leave-back};f->blocks[4]=(XrXirBlock){.first=leave,.count=end-leave};
    f->blocks[5]=(XrXirBlock){.first=end,.count=1};
}
static void access_older_root(AccessFixture *f,uint32_t kind) {
    uint32_t array=access_op(f,(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)A_ARRAY});
    uint32_t initial=access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)A_CELL,.args={array}});
    access_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}});uint32_t head=f->functions[0].instruction_count;
    uint32_t at=access_group(f,(uint32_t[]){0,initial,2,0},4);
    uint32_t phi=access_op(f,(XrXirInstruction){.op=XR_XIR_PHI,.type=(XrXirType)A_CELL,.args={at,4}}),older=phi;
    uint32_t other=UINT32_MAX,other_at=0;
    if (kind==171) {
        other_at=access_group(f,(uint32_t[]){0,1,2,0},4);
        other=access_op(f,(XrXirInstruction){.op=XR_XIR_PHI,.type=(XrXirType)A_CELL,.args={other_at,4}});
    }
    if (kind==166 || kind==168) older=access_op(f,(XrXirInstruction){
        .op=XR_XIR_COPY,.type=(XrXirType)A_CELL,.args={older}});
    uint32_t child=kind==170?access_view(f,older,false,true):UINT32_MAX;
    array=access_op(f,(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)A_ARRAY});
    uint32_t fresh=access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)A_CELL,.args={array}});
    if (kind==167 || kind==168 || kind==169) older=access_op(f,(XrXirInstruction){
        .op=XR_XIR_COPY,.type=(XrXirType)A_CELL,.args={older}});
    if (kind==169) older=access_op(f,(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)A_CELL,.args={older}});
    if (kind!=170) child=access_view(f,fresh,false,true);
    access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=(XrXirType)A_ARRAY,.args={kind==170?fresh:older}});
    access_end(f,child);access_op(f,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={2,3}});
    uint32_t back=f->functions[0].instruction_count;
    f->operands[at+3u]=kind==171?other:fresh;if (kind==171) f->operands[other_at+3u]=fresh;
    access_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}});
    uint32_t end=f->functions[0].instruction_count;access_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});
    f->functions[0].block_count=4;f->blocks[0]=(XrXirBlock){.count=head};
    f->blocks[1]=(XrXirBlock){.first=head,.count=back-head};f->blocks[2]=(XrXirBlock){.first=back,.count=end-back};
    f->blocks[3]=(XrXirBlock){.first=end,.count=1};
}
static void access_reaching_local(AccessFixture *f,uint32_t kind) {
    uint32_t local=UINT32_MAX;
    if (kind==178) local=access_op(f,(XrXirInstruction){.op=XR_XIR_LOCAL_NEW,.type=(XrXirType)A_CELL,.args={1}});
    access_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}});uint32_t head=f->functions[0].instruction_count;
    uint32_t array=access_op(f,(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)A_ARRAY});
    uint32_t root=access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)A_CELL,.args={array}});
    uint32_t child=access_view(f,root,false,true);
    if (local==UINT32_MAX) local=access_op(f,(XrXirInstruction){.op=kind==176?XR_XIR_LOCAL_UNINIT:XR_XIR_LOCAL_NEW,
        .type=(XrXirType)A_CELL,.args={kind==176?0:kind==173 || kind==175?1:root}});
    uint32_t read=UINT32_MAX;
    if (kind==174 || kind==175) read=access_op(f,(XrXirInstruction){.op=XR_XIR_LOCAL_READ,.type=(XrXirType)A_CELL,.args={local}});
    uint32_t left=0,right=0,merge=0;
    if (kind>=179) {
        access_op(f,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={2,3}});left=f->functions[0].instruction_count;
        access_op(f,(XrXirInstruction){.op=XR_XIR_LOCAL_WRITE,.args={local,1}});
        access_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={4}});right=f->functions[0].instruction_count;
        uint32_t value=root;
        if (kind==182) value=access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)A_CELL,.args={array}});
        if (kind!=181) access_op(f,(XrXirInstruction){.op=XR_XIR_LOCAL_WRITE,.args={local,kind==179 || kind==183?1:value}});
        access_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={4}});merge=f->functions[0].instruction_count;
    } else access_op(f,(XrXirInstruction){.op=kind==177?XR_XIR_PLACE_WRITE:XR_XIR_LOCAL_WRITE,
        .args={local,kind==173 || kind==175?root:1}});
    if (read==UINT32_MAX) read=access_op(f,(XrXirInstruction){.op=kind==177?XR_XIR_PLACE_READ:XR_XIR_LOCAL_READ,
        .type=(XrXirType)A_CELL,.args={local}});
    if (kind==178 || kind==183) access_op(f,(XrXirInstruction){.op=XR_XIR_LOCAL_WRITE,.args={local,root}});
    if (kind==173 || kind==174 || kind==180 || kind==181) access_bad(f,XR_XIR_UNRESOLVED);
    access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=(XrXirType)A_ARRAY,.args={read}});access_end(f,child);
    uint32_t exit=kind>=179?5u:2u;
    access_op(f,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={1,exit}});
    uint32_t end=f->functions[0].instruction_count;access_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});
    f->blocks[0]=(XrXirBlock){.count=head};
    if (kind>=179) {
        f->functions[0].block_count=6;f->blocks[1]=(XrXirBlock){.first=head,.count=left-head};
        f->blocks[2]=(XrXirBlock){.first=left,.count=right-left};f->blocks[3]=(XrXirBlock){.first=right,.count=merge-right};
        f->blocks[4]=(XrXirBlock){.first=merge,.count=end-merge};
    } else {f->functions[0].block_count=3;f->blocks[1]=(XrXirBlock){.first=head,.count=end-head};}
    f->blocks[exit]=(XrXirBlock){.first=end,.count=1};
}
static void access_local_panic(AccessFixture *f,uint32_t kind) {
    uint32_t array=access_op(f,(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)A_ARRAY});
    uint32_t root=access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)A_CELL,.args={array}});
    uint32_t child=access_view(f,root,false,true);
    uint32_t local=access_op(f,(XrXirInstruction){.op=XR_XIR_LOCAL_NEW,.type=(XrXirType)A_CELL,.args={kind==185?1:root}});
    access_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}});uint32_t protected=f->functions[0].instruction_count;
    access_op(f,(XrXirInstruction){.op=XR_XIR_LOCAL_WRITE,.args={local,1}});
    access_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={2}});uint32_t normal=f->functions[0].instruction_count;
    access_end(f,child);access_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});uint32_t handler=f->functions[0].instruction_count;
    access_op(f,(XrXirInstruction){.op=XR_XIR_PANIC_CATCH,.type=XR_XIR_PANIC_INFO});
    uint32_t read=access_op(f,(XrXirInstruction){.op=XR_XIR_LOCAL_READ,.type=(XrXirType)A_CELL,.args={local}});
    if (kind==184) access_bad(f,XR_XIR_UNRESOLVED);
    access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=(XrXirType)A_ARRAY,.args={read}});access_end(f,child);
    access_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});uint32_t end=f->functions[0].instruction_count;
    f->functions[0].block_count=4;f->blocks[0]=(XrXirBlock){.count=protected};
    f->blocks[1]=(XrXirBlock){.first=protected,.count=normal-protected,.panic=3};
    f->blocks[2]=(XrXirBlock){.first=normal,.count=handler-normal};f->blocks[3]=(XrXirBlock){.first=handler,.count=end-handler};
}
static void access_local_place_payload(AccessFixture *f,uint32_t kind) {
    access_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}});uint32_t head=f->functions[0].instruction_count;
    uint32_t array=access_op(f,(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)A_ARRAY});
    uint32_t root=access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)A_CELL,.args={array}});
    uint32_t local=access_op(f,(XrXirInstruction){.op=XR_XIR_LOCAL_NEW,.type=(XrXirType)A_CELL,.args={kind==186?1:root}});
    access_op(f,(XrXirInstruction){.op=XR_XIR_PLACE_WRITE,.args={local,kind==186?root:1}});
    uint32_t child=access_view(f,root,false,true);
    if (kind==186) access_bad(f,XR_XIR_UNRESOLVED);
    access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_LOCAL_WRITE,.args={local,array}});
    access_end(f,child);access_op(f,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={1,2}});
    uint32_t end=f->functions[0].instruction_count;access_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});
    f->functions[0].block_count=3;f->blocks[0]=(XrXirBlock){.count=head};
    f->blocks[1]=(XrXirBlock){.first=head,.count=end-head};f->blocks[2]=(XrXirBlock){.first=end,.count=1};
}
static void access_local_binding(AccessFixture *f,uint32_t kind) {
    uint32_t array=access_op(f,(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)A_ARRAY});
    uint32_t view=access_view(f,f->root,false,kind!=99 && kind!=101);
    if (kind==101) {
        access_bad(f,XR_XIR_BAD_VALUE);f->prefix_expected=f->expected;
        access_op(f,(XrXirInstruction){.op=XR_XIR_LOCAL_NEW,.type=(XrXirType)A_SHARED,.args={view}});return;
    }
    XrXirType type=(XrXirType)(kind==98?A_ARRAY:A_CELL);uint32_t value=kind==98?array:f->root;
    uint32_t local=access_op(f,(XrXirInstruction){.op=kind==96?XR_XIR_LOCAL_UNINIT:XR_XIR_LOCAL_NEW,.type=type,.args={kind==96?0:value}});
    access_op(f,(XrXirInstruction){.op=XR_XIR_LOCAL_WRITE,.args={local,value}});
    uint32_t read=access_op(f,(XrXirInstruction){.op=XR_XIR_LOCAL_READ,.type=type,.args={local}});
    if (kind==97 || kind==99 || kind==100) access_bad(f,XR_XIR_BAD_VALUE);
    if (kind==97) access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=(XrXirType)A_ARRAY,.args={read}});
    if (kind==99 || kind==100) access_op(f,(XrXirInstruction){.op=kind==99?XR_XIR_CELL_WRITE:XR_XIR_CELL_LOCAL_WRITE,
        .args={kind==99?read:local,array}});
    access_end(f,view);
}
static void access_entry_cells(AccessFixture *f,uint32_t kind) {
    if (kind==115) {access_bad(f,XR_XIR_UNRESOLVED);access_cell_read(f);return;}
    uint32_t first=access_view(f,0,false,kind!=113);
    if (kind==114) {access_bad(f,XR_XIR_BAD_VALUE);access_cell_read(f);access_end(f,first);return;}
    if (kind==112) {
        access_op(f,(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=(XrXirType)A_ARRAY,.args={1}});
        uint32_t second=access_view(f,1,false,false);access_set(f,first);access_get(f,second);access_end(f,second);
    } else {access_set(f,1);access_get(f,first);}
    access_end(f,first);
}
static void access_entry_inputs(AccessFixture *f,uint32_t kind) {
    if (kind>=106 && kind<=108) {access_aggregate_call(f,0,kind);return;}
    if (kind==109) {
        f->callees[0]=f->parameters[0];f->callees[1]=f->parameters[1];
        f->functions[2].parameters=f->callees;f->functions[2].parameter_count=2;
        uint32_t at=access_group(f,(uint32_t[]){0,1},2);access_op(f,(XrXirInstruction){.op=XR_XIR_CALL,.args={at,2},.immediate=2});return;
    }
    if (kind==110) {access_bad(f,XR_XIR_UNRESOLVED);access_cell_read(f);return;}
    if (kind==105 || kind==111) {
        uint32_t child=access_view(f,0,true,kind==105);
        if (kind==111) {
            f->callee=(XrXirType)A_MUT;uint32_t parent=0;access_bad(f,XR_XIR_BAD_VALUE);
            access_op(f,(XrXirInstruction){.op=XR_XIR_CALL,.args={access_group(f,&parent,1),1},.immediate=2});
        } else access_set(f,child);
        access_end(f,child);
    } else if (kind==103) access_get(f,0);else access_set(f,0);
    if (kind==104) access_get(f,1);else access_set(f,1);
}
static void access_case(AccessFixture *f,uint32_t kind) {
    access_base(f,kind);
    if (kind>=186) access_local_place_payload(f,kind);
    else if (kind>=184) access_local_panic(f,kind);
    else if (kind>=172) access_reaching_local(f,kind);
    else if (kind>=166) access_older_root(f,kind);
    else if (kind>=159) access_cyclic_root_phi(f,kind);
    else if (kind==158) {
        access_root_phi(f,155);f->expected=XR_XIR_UNRESOLVED;
        f->operands[f->ops[f->blocks[2].first].args[0]+1u]=1;
    }
    else if (kind>=156) access_deep_root_phi(f,kind);
    else if (kind>=148) access_root_phi(f,kind);
    else if (kind>=146) access_scope_panic(f,kind);
    else if (kind>=138) access_scope_blocks(f,kind);
    else if (kind>=136) access_root_panic(f,kind);
    else if (kind>=126) access_root_blocks(f,kind);
    else if (kind>=116) access_root_epoch(f,kind);
    else if (kind>=112) access_entry_cells(f,kind);
    else if (kind>=102) access_entry_inputs(f,kind);
    else if (kind>=96) access_local_binding(f,kind);
    else if (kind>=88) access_wrapped_epoch(f,kind);
    else if (kind>=74) access_prefix_epoch(f,kind);
    else if (kind>=47) access_aggregate(f,kind);
    else if (kind>=45) access_presence(f,kind);
    else if (kind>=35 && kind<44) access_wrappers(f,kind);
    else if (kind==44) {uint32_t view=access_view(f,f->root,false,false);access_end(f,view);access_cell_write(f);}
    else if (kind<=9) access_simple(f,kind);
    else if ((kind>=10 && kind<=17) || (kind>=32 && kind<=34)) access_children(f,kind);
    else if (kind==18 || kind==28) access_fresh(f,kind);
    else if (kind>=19 && kind<=24) access_places(f,kind);
    else if (kind==25 || kind==26) {
        uint32_t parent=access_view(f,f->root,false,true),child=access_view(f,parent,true,false);
        uint32_t at=access_group(f,(uint32_t[]){kind==25?parent:child},1);
        if (kind==25) access_bad(f,XR_XIR_BAD_VALUE);access_op(f,(XrXirInstruction){.op=XR_XIR_CALL,.args={at,1},.immediate=2});
    } else if (kind==27) {
        access_view(f,0,false,true);access_bad(f,XR_XIR_UNRESOLVED);access_view(f,1,false,true);
    } else if (kind==29) {access_bad(f,XR_XIR_UNRESOLVED);access_view(f,f->root,false,true);}
    else {access_phi(f,kind==31);}
    if (f->functions[0].block_count==1) {access_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});f->blocks[0].count=f->functions[0].instruction_count;}
    if (f->functions[0].operand_count) f->functions[0].operands=f->operands;
}
typedef struct AccessScoped {
    const XrXirCompileContext *parent;
    const XrXirTypes *types;
    size_t baseline_bytes,baseline_count;
    uint32_t events;
    bool stop;
} AccessScoped;
static XrXirStatus access_scoped_visit(void *user,const XrXirCompileContext *context,const XrXirViewOwnerEvent *event) {
    AccessScoped *s=user;CHECK(event->function==0 && context->resources==s->parent->resources);
    CHECK(context->limits.frame_bytes<s->parent->limits.frame_bytes && live>=s->baseline_count && live_bytes>=s->baseline_bytes);
    uint64_t payload=live_bytes-s->baseline_bytes-(live-s->baseline_count)*sizeof(CompileAllocation);
    CHECK(s->parent->limits.frame_bytes-context->limits.frame_bytes>=payload);++s->events;
    uint8_t *flags=NULL;XrXirStatus status=xir_compile_view_flags_new(context,s->types,&flags);
    xr_compile_resources_free(flags);return status==XR_XIR_OK && s->stop?XR_XIR_UNSUPPORTED:status;
}
static XrXirStatus access_scoped_check(const XrXirCompileContext *context,const AccessFixture *f) {
    AccessScoped s={context,&f->types,live_bytes,live,0,true};size_t first=attempts;
    XrXirCompileContext tiny=*context;tiny.limits.frame_bytes=1;
    CHECK(xir_compile_view_may_owned_function_scoped(&tiny,&f->module,0,access_scoped_visit,&s,NULL)==XR_XIR_BUDGET);
    CHECK(!s.events && attempts==first);
    XrXirStatus status=xir_compile_view_may_owned_function_scoped(context,&f->module,0,access_scoped_visit,&s,NULL);
    if (status==XR_XIR_BUDGET || status==XR_XIR_MEMORY_LIMIT || status==XR_XIR_OUT_OF_MEMORY) return status;
    CHECK(status==XR_XIR_UNSUPPORTED && s.events==1 && live_bytes==s.baseline_bytes && live==s.baseline_count);
    s.stop=false;s.events=0;
    status=xir_compile_view_may_owned_function_scoped(context,&f->module,0,access_scoped_visit,&s,NULL);
    if (status==XR_XIR_OK) CHECK(s.events && live_bytes==s.baseline_bytes && live==s.baseline_count);
    return status;
}
static XrXirStatus access_sequence(const XrXirCompileContext *context,uint32_t kind) {
    AccessFixture f;access_case(&f,kind);unsigned char before[sizeof(f)];memcpy(before,&f,sizeof(f));
    XrXirConstruction *construction=NULL;XrXirDiagnostic diagnostic={0};const char *stage="construction";
    XrXirStatus status=xir_fixture_construction(context,&f.types,&construction);if (status!=XR_XIR_OK) goto done;
    stage="ordinary";
    status=xir_compile_view_prepare_ordinary(context,&f.module,construction,&diagnostic);
    if (status==XR_XIR_OK) {stage="return origins";status=xir_compile_view_return_origins_verified(context,&f.module,&diagnostic);}
    if (f.prefix_expected && status!=XR_XIR_BUDGET && status!=XR_XIR_MEMORY_LIMIT && status!=XR_XIR_OUT_OF_MEMORY) CHECK(status==XR_XIR_OK);
    if (status==XR_XIR_OK) {stage="affine";status=xir_compile_view_affine_verified(context,&f.module,&diagnostic);}
    if (status==XR_XIR_BUDGET || status==XR_XIR_MEMORY_LIMIT || status==XR_XIR_OUT_OF_MEMORY) goto done;
    if (f.prefix_expected!=XR_XIR_OK) {
        CHECK(status==f.prefix_expected && diagnostic.function==0 && diagnostic.instruction==f.bad);goto done;
    }
    if (status!=XR_XIR_OK) fprintf(stderr,"access prefix kind=%u status=%u at %u/%u/%u\n",kind,status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_OK);
    if (kind==44) {status=access_scoped_check(context,&f);if (status!=XR_XIR_OK) goto done;}
    XrXirCompileContext tiny=*context;tiny.limits.frame_bytes=1;size_t count=attempts;
    CHECK(xir_compile_view_access_verified(&tiny,&f.module,NULL)==XR_XIR_BUDGET && attempts==count);
    if (kind>=112 && kind<116) CHECK(xir_compile_view_cell_access_verified(&tiny,&f.module,NULL)==XR_XIR_BUDGET && attempts==count);
    stage="access";
    status=kind>=112 && kind<116?xir_compile_view_cell_access_verified(context,&f.module,&diagnostic):
        xir_compile_view_access_verified(context,&f.module,&diagnostic);
    if (status!=XR_XIR_BUDGET && status!=XR_XIR_MEMORY_LIMIT && status!=XR_XIR_OUT_OF_MEMORY) {
        if (status!=f.expected || (status!=XR_XIR_OK && diagnostic.instruction!=f.bad))
            fprintf(stderr,"access kind=%u actual=%u at %u/%u/%u expected=%u instruction=%u\n",kind,status,diagnostic.function,diagnostic.block,diagnostic.instruction,f.expected,f.bad);
        CHECK(status==f.expected);if (status!=XR_XIR_OK) CHECK(diagnostic.function==0 && diagnostic.instruction==f.bad);
    }
done:
    if (kind==71 && fail_at==SIZE_MAX && status!=f.expected) {
        XrCompileResourceStats s=stats(context);
        fprintf(stderr,"access DAG stage=%s status=%u at %u/%u/%u held=%llu peak=%llu frame=%llu\n",
            stage,status,diagnostic.function,diagnostic.block,diagnostic.instruction,(unsigned long long)s.held_bytes,
            (unsigned long long)s.peak_bytes,(unsigned long long)context->limits.frame_bytes);
    }
    CHECK(!memcmp(before,&f,sizeof(f)));xr_xir_compile_construction_free(construction);return status;
}
static void access_resources(uint32_t kind,bool normal) {
    AccessFixture fixture;access_case(&fixture,kind);XrXirStatus expected=fixture.expected;
    XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).held_bytes;size_t first=attempts;
    XrXirStatus initial=access_sequence(&c,kind);
    if (initial!=expected) fprintf(stderr,"access initial kind=%u status=%u expected=%u\n",kind,initial,expected);
    CHECK(initial==expected);size_t points=attempts-first;XrCompileResourceStats measured=stats(&c);
    CHECK(points && points<4096);CHECK(access_sequence(&c,kind)==expected);owner_free(&c,baseline);
    if (!normal) for (size_t p=0;p<points;++p) {
        c=owner_new(caps());baseline=stats(&c).held_bytes;fail_at=attempts+p;injected=false;
        XrXirStatus status=access_sequence(&c,kind);fail_at=SIZE_MAX;
        if (status!=XR_XIR_OUT_OF_MEMORY || !injected) fprintf(stderr,"access kind=%u FI=%zu/%zu status=%u\n",kind,p,points,status);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && injected);owner_free(&c,baseline);
    }
    if (!normal) for (uint32_t delta=0;delta<3;++delta) {
        XrCompileResourceLimits limits=caps();limits.memory_mode=XR_COMPILE_MEMORY_FINITE;
        limits.memory_limit=measured.peak_bytes+delta-1u;
        c=owner_new(limits);baseline=stats(&c).held_bytes;
        CHECK(access_sequence(&c,kind)==(delta?expected:XR_XIR_MEMORY_LIMIT));
        if (delta) CHECK(access_sequence(&c,kind)==expected);
        owner_free(&c,baseline);
    }
    printf("View access kind=%u: %zu allocation points owner2/physical0%s PASS\n",kind,points,
        normal?"":"/all physical failures/held exact+minus1+plus1/reuse");
}
static bool access_frame_accepts(uint32_t kind,uint64_t bytes,XrXirStatus expected) {
    XrXirCompileContext context=owner_new(caps());uint64_t baseline=stats(&context).held_bytes;
    context.limits.frame_bytes=bytes;XrXirStatus status=access_sequence(&context,kind);
    CHECK(status==expected || status==XR_XIR_BUDGET);owner_free(&context,baseline);return status==expected;
}
static void access_frame_boundary(uint32_t kind) {
    AccessFixture fixture;access_case(&fixture,kind);
    uint64_t low=1,high=xr_xir_compile_default_limits().frame_bytes;
    CHECK(access_frame_accepts(kind,high,fixture.expected));
    while (low<high) {
        uint64_t middle=low+(high-low)/2u;
        if (access_frame_accepts(kind,middle,fixture.expected)) high=middle;else low=middle+1u;
    }
    CHECK(access_frame_accepts(kind,low,fixture.expected));
    CHECK(!access_frame_accepts(kind,low-1u,fixture.expected));
    CHECK(access_frame_accepts(kind,low+1u,fixture.expected));
    printf("Root PHI kind=%u minimum_pipeline_frame=%llu exact/minus1/plus1/physical0 PASS\n",
        kind,(unsigned long long)low);
}
static void access_root_model_dump(void) {
    static const char *const names[XR_XIR_OP_COUNT]={"INVALID",
#define XR_XIR_OP(name,stages,result,operands,edges,terminal) #name,
#include "xir/xxir_ops.def"
#undef XR_XIR_OP
    };
    printf("{\"cell_type\":%u,\"mut_type\":%u,\"cases\":[",A_CELL,A_MUT);
    for (uint32_t index=0;index<A_CASES-148u+2u;++index) {
        uint32_t kind=index==0?120:index==1?134:index+146u;
        AccessFixture fixture;access_case(&fixture,kind);
        XrXirCompileContext context=owner_new(caps());uint64_t baseline=stats(&context).held_bytes;
        XrXirStatus status=access_sequence(&context,kind);owner_free(&context,baseline);
        const XrXirFunction *f=fixture.functions;
        printf("%s{\"kind\":%u,\"status\":%u,\"bad\":%u,\"parameters\":[",index?",":"",kind,status,fixture.bad);
        for (uint32_t p=0;p<f->parameter_count;++p) printf("%s%u",p?",":"",(uint32_t)f->parameters[p]);
        printf("],\"blocks\":[");
        for (uint32_t b=0;b<f->block_count;++b) printf("%s[%u,%u,%u]",b?",":"",f->blocks[b].first,f->blocks[b].count,f->blocks[b].panic);
        printf("],\"operands\":[");
        for (uint32_t p=0;p<f->operand_count;++p) printf("%s%u",p?",":"",f->operands[p]);
        printf("],\"instructions\":[");
        for (uint32_t i=0;i<f->instruction_count;++i) {
            const XrXirInstruction *op=f->instructions+i;
            printf("%s[\"%s\",%u,%u,%u,%u,%u,%lld]",i?",":"",names[op->op],(uint32_t)op->type,
                op->args[0],op->args[1],op->targets[0],op->targets[1],(long long)op->immediate);
        }
        printf("]}");
    }
    puts("]}");
}
typedef struct SlotPublication {
    XrXirTypeNode nodes[8];XrXirTypes types;
    XrXirInstruction ops[16],end[1];uint32_t operands[4];XrXirBlock blocks[3];
    XrXirFunction functions[3];XrXirFunctionIdentity identities[3];
    XrXirSourceModule modules[2];XrXirSlot slot;XrXirLiteral literal;XrXirDeclarations declarations;XrXirModule module;
} SlotPublication;
static void slot_publication_fixture(SlotPublication *f,uint32_t kind) {
    memset(f,0,sizeof(*f));
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_U8};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_BOOL};
    f->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)257};
    f->nodes[3]=(XrXirTypeNode){.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_BOOL};
    XrXirType element=kind==1?XR_XIR_U8:kind==5?(XrXirType)256:
        kind==6?(XrXirType)257:kind==7?XR_XIR_STRING:XR_XIR_I64;
    f->nodes[4]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=element};
    f->nodes[5]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=kind==10?XR_XIR_STRING:(XrXirType)260};
    if (kind==8) {
        f->nodes[6]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)260};
        f->nodes[7]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)262};
    }
    f->types=(XrXirTypes){f->nodes,kind==8?8u:6u,NULL,NULL};
    f->ops[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64};
    f->ops[1]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=1};
    f->ops[2]=kind==10?(XrXirInstruction){.op=XR_XIR_CONST_STRING,.type=XR_XIR_STRING}:
        (XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)260};
    f->ops[3]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)261,.args={2}};
    uint32_t at=4,payload=3;
    if (kind==8) {
        f->ops[3]=(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)262,.args={0,1}};
        f->ops[at++]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)263,.args={3}};
        f->ops[at++]=(XrXirInstruction){.op=XR_XIR_CELL_PLACE,.type=(XrXirType)262,.args={4}};
        f->ops[at++]=(XrXirInstruction){.op=XR_XIR_INDEX_PLACE,.type=(XrXirType)260,.args={5,0}};
        f->ops[at++]=(XrXirInstruction){.op=XR_XIR_CELL_PROJECT,.type=(XrXirType)261,.args={6}};
        payload=7;
    }
    f->ops[at++]=(XrXirInstruction){.op=XR_XIR_SLOT_INIT,.args={kind==9?UINT32_MAX:payload}};
    uint32_t array=at;
    f->ops[at++]=(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)257};
    f->ops[at++]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)258,.args={array}};
    uint32_t view=at;
    f->ops[at++]=(XrXirInstruction){.op=XR_XIR_VIEW_NEW,.type=(XrXirType)259,.args={kind==8?1u:0u,3}};
    f->ops[at++]=(XrXirInstruction){.op=XR_XIR_VIEW_END,.args={view}};
    f->ops[at++]=(XrXirInstruction){.op=XR_XIR_RETURN};
    uint32_t table=kind==8?1u:0u;f->operands[0]=2;
    f->operands[table]=array+1;f->operands[table+1]=0;f->operands[table+2]=1;
    f->blocks[0]=(XrXirBlock){.count=at};f->blocks[1]=f->blocks[2]=(XrXirBlock){.count=1};
    f->end[0]=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->functions[0]=(XrXirFunction){.name="initialize",.name_length=10,.blocks=f->blocks,.block_count=1,
        .instructions=f->ops,.instruction_count=at,.operands=f->operands,.operand_count=kind==8?4u:3u,.error_type=XR_XIR_NEVER};
    f->functions[1]=(XrXirFunction){.name="other",.name_length=5,.blocks=f->blocks+1,.block_count=1,
        .instructions=f->end,.instruction_count=1,.error_type=XR_XIR_NEVER};
    f->functions[2]=(XrXirFunction){.name="ordinary",.name_length=8,.blocks=f->blocks+2,.block_count=1,
        .instructions=f->end,.instruction_count=1,.error_type=XR_XIR_NEVER};
    f->identities[1].module=1;
    f->modules[0]=(XrXirSourceModule){.name="root",.name_length=4,.initializer=kind==4?2:0};
    f->modules[1]=(XrXirSourceModule){.name="other",.name_length=5,.initializer=1};
    f->slot=(XrXirSlot){kind==3?1u:0u,(XrXirType)261,1};
    f->declarations=(XrXirDeclarations){.modules=f->modules,.module_count=2,.functions=f->identities,
        .slots=&f->slot,.slot_count=1,.root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    if (kind==10) {f->literal=(XrXirLiteral){"array-owner",11};f->declarations.literals=&f->literal;f->declarations.literal_count=1;}
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=3,
        .declarations=&f->declarations,.types=&f->types,.linkage_kind=XR_XIR_LIBRARY};
}
static XrXirStatus slot_publication_attempt(const XrXirCompileContext *context,uint32_t kind) {
    SlotPublication f;slot_publication_fixture(&f,kind);unsigned char before[sizeof(f)];memcpy(before,&f,sizeof(f));
    XrXirConstruction *construction=NULL;XrXirDiagnostic diagnostic={0};
    const char *gate="types";
    XrXirStatus status=xr_xir_compile_types_structure_verify(context,&f.types);
    XrXirSlotFact fact={0,"fixture",7,0};const XrXirSlotFacts slots={&fact,1};
    if (status==XR_XIR_OK) {gate="construction";status=xir_construction_project(context,&f.types,&slots,&construction);}
    if (status==XR_XIR_OK) {gate="ordinary";status=xir_compile_view_prepare_ordinary(context,&f.module,construction,&diagnostic);}
    if (status==XR_XIR_OK) {gate="affine";status=xir_compile_view_affine_verified(context,&f.module,&diagnostic);}
    if (status==XR_XIR_OK) {gate="access";status=kind==2?xir_compile_view_access_verified(context,&f.module,&diagnostic):
        xir_compile_view_cell_access_verified(context,&f.module,&diagnostic);}
    if (status!=XR_XIR_OUT_OF_MEMORY && status!=XR_XIR_MEMORY_LIMIT && status!=XR_XIR_BUDGET)
        fprintf(stderr,"Slot publication kind=%u %s status=%u at %u/%u/%u\n",kind,gate,status,
            diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(!memcmp(before,&f,sizeof(f)));xr_xir_compile_construction_free(construction);return status;
}
static void slot_publication_cases(void) {
    static const XrXirStatus expected[]={XR_XIR_OK,XR_XIR_OK,XR_XIR_UNRESOLVED,
        XR_XIR_BAD_STRUCTURE,XR_XIR_BAD_STRUCTURE,XR_XIR_BAD_TYPE,XR_XIR_UNRESOLVED,
        XR_XIR_UNRESOLVED,XR_XIR_BAD_VALUE,XR_XIR_BAD_VALUE,XR_XIR_OK};
    for (uint32_t kind=0;kind<sizeof(expected)/sizeof(expected[0]);++kind) {
        XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).held_bytes;size_t first=attempts;
        CHECK(slot_publication_attempt(&c,kind)==expected[kind]);size_t points=attempts-first;
        CHECK(slot_publication_attempt(&c,kind)==expected[kind]);owner_free(&c,baseline);
        if (kind>1 && kind!=10) continue;
        CHECK(points>1);
        for (uint32_t edge=0;edge<2;++edge) {
            c=owner_new(caps());baseline=stats(&c).held_bytes;
            fail_at=attempts+(edge?points-1:0);injected=false;
            CHECK(slot_publication_attempt(&c,kind)==XR_XIR_OUT_OF_MEMORY && injected);
            fail_at=SIZE_MAX;CHECK(slot_publication_attempt(&c,kind)==XR_XIR_OK);owner_free(&c,baseline);
        }
    }
    puts("Scalar Array slot publication: authentic owner, module, borrowed refusal and physical zero passed");
}

#include "xir/xxir_program_internal.h"
#include "xir/xxir_program_packet_internal.h"
#include "xir/xxir_call_view_operation_internal.h"
#include "xir/xxir_call_internal.h"
#include "xir/xxir_instance_view_internal.h"
#include "xir/xxir_array.h"
#include "xir_native_metadata_fixture.h"
enum { MP_ARRAY=256,MP_CELL,MP_SLICE,MP_PROJECT };
typedef struct ModuleViewFrame { XrXirValue root,value,slice;uint32_t phase; } ModuleViewFrame;
typedef struct ModuleViewWitness {
    XrXirValue saved,foreign,late;XrXirViewAccess access;XrXirInstance *other;
    uint32_t mode,checks,releases;
} ModuleViewWitness;
static void module_view_cleanup(XrXirCallView *view,XrXirCallStatus status) {
    (void)status;ModuleViewFrame *f=view->state;
    xr_xir_value_drop(&f->slice);xr_xir_value_drop(&f->value);xr_xir_value_drop(&f->root);
}
static XrXirAction module_view_done(void) {
    return (XrXirAction){.kind=XR_XIR_ACTION_RETURN};
}
static XrXirAction module_view_init(XrXirCallView *view) {
    if (xr_xir_call_current_entry(view->activation)==1) return module_view_done();
    ModuleViewFrame *f=view->state;ModuleViewWitness *w=(ModuleViewWitness *)view->environment;
    const XrXirValue values[]={{XR_XIR_U8,0,7},{XR_XIR_U8,0,9}};
    CHECK(xr_xir_array_new((XrXirType)MP_ARRAY,values,2,xr_xir_call_admission(view),&f->value)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_instance_cell(view,(XrXirType)MP_CELL,&f->value,&f->root)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_slot_write(view,0,&f->root,true)==XR_XIR_CALL_READY);
    if (!w->saved.type) CHECK(xr_xir_value_copy(&f->root,&w->saved)==XR_XIR_VALUE_OK);
    return module_view_done();
}
static XrXirAction module_view_return(XrXirCallView *view) {
    return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value={
        xr_xir_call_current_entry(view->activation)==2?XR_XIR_I64:XR_XIR_UNIT,0,0}};
}
static XrXirAction module_view_borrowed(XrXirCallView *view) {
    ModuleViewFrame *f=view->state;ModuleViewWitness *w=(ModuleViewWitness *)view->environment;
    CHECK(xr_xir_value_copy(&view->arguments[0],&f->root)==XR_XIR_VALUE_OK);
    XrXirCallViewOperation request={.operation=XR_XIR_VIEW_NEW,.type=(XrXirType)MP_SLICE,
        .root=&f->root,.to_end=true};XrXirFaultDetail fault={0};
    CHECK(xr_xir_call_view_operation(view,&request,&f->slice,&fault)==XR_XIR_VALUE_OK);
    ++w->checks;return module_view_done();
}
static XrXirAction module_view_entry(XrXirCallView *view) {
    ModuleViewFrame *f=view->state;ModuleViewWitness *w=(ModuleViewWitness *)view->environment;
    if (f->phase) return module_view_return(view);
    f->phase=1;
    const XrXirValue *root=w->mode==2?&w->foreign:&w->saved;
    CHECK(xr_xir_value_copy(root,&f->root)==XR_XIR_VALUE_OK);
    fprintf(stderr,"Module View callback mode%u entry%u\n",w->mode,xr_xir_call_current_entry(view->activation));
    if (w->mode==4) return (XrXirAction){.kind=XR_XIR_ACTION_CALL,.callee=4,.arguments=&f->root,.argument_count=1};
    XrXirCallView copied=*view;copied.instance=NULL;
    CHECK(xir_instance_view_module_root(&copied,&f->root)!=XR_XIR_CALL_READY);
    copied=*view;copied.instance=w->other;
    CHECK(xir_instance_view_module_root(&copied,&f->root)!=XR_XIR_CALL_READY);
    XrXirCallViewOperation request={.operation=XR_XIR_VIEW_NEW,.type=(XrXirType)MP_SLICE,
        .root=&f->root,.to_end=true};XrXirFaultDetail fault={0};
    if (!w->mode || w->mode==3) {
        const XrXirValuePathStep step={XR_XIR_PATH_INDEX,(XrXirType)MP_ARRAY,0};
        const XrXirValuePath path={&step,1};
        CHECK(xr_xir_call_cell_project(view,(XrXirType)MP_PROJECT,&f->root,&path,&f->value)==XR_XIR_VALUE_OK);
        request.root=&f->value;
        CHECK(xr_xir_call_view_operation(view,&request,&f->slice,&fault)==XR_XIR_VALUE_BAD_ARGUMENT && !f->slice.type);
        xr_xir_value_drop(&f->value);request.root=&f->root;
    }
    XrXirValueStatus status=xr_xir_call_view_operation(view,&request,&f->slice,&fault);
    if (w->mode==1 || w->mode==2) {
        CHECK(status==XR_XIR_VALUE_BAD_ARGUMENT && !f->slice.type && !f->slice.payload && !fault.code);
        ++w->checks;return module_view_return(view);
    }
    CHECK(status==XR_XIR_VALUE_OK);
    CHECK(xr_xir_call_view_access(view,&w->access)==XR_XIR_VALUE_OK);
    int64_t count=-1;CHECK(xr_xir_view_len(&f->slice,&w->access,&count)==XR_XIR_VALUE_OK && count==2);
    CHECK(xr_xir_value_copy(&f->slice,&w->late)==XR_XIR_VALUE_OK);
    XrXirValue outside={0};request.root=&w->saved;
    CHECK(xr_xir_call_view_operation(view,&request,&outside,&fault)==XR_XIR_VALUE_BAD_ARGUMENT && !outside.type);
    request.root=&f->root;request.type=(XrXirType)UINT16_MAX;
    CHECK(xr_xir_call_view_operation(view,&request,&f->value,&fault)==XR_XIR_VALUE_BAD_ARGUMENT && !f->value.type);
    CHECK(xr_xir_call_view_local(view,(XrXirType)MP_SLICE,&f->root,0,2,&f->value)==XR_XIR_VALUE_BAD_ARGUMENT);
    /* A genuine Cell carrying a publication outside the actual slot table is not a module root. */
    const XrXirValue value={XR_XIR_U8,0,11};XrXirValue array={0};
    CHECK(xr_xir_array_new((XrXirType)MP_ARRAY,&value,1,xr_xir_call_admission(view),&array)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_instance_cell(view,(XrXirType)MP_CELL,&array,&f->value)==XR_XIR_CALL_READY);
    XrXirCellPublication publication={view->instance,xr_xir_call_admission(view)->domain,99};
    CHECK(xr_xir_cell_publication_prepare(&f->value,&publication)==XR_XIR_VALUE_OK);
    xr_xir_cell_publication_commit(&f->value,&publication);
    request.type=(XrXirType)MP_SLICE;request.root=&f->value;
    XrXirValue absent={0};CHECK(xr_xir_call_view_operation(view,&request,&absent,&fault)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(!absent.type && !absent.reserved && !absent.payload);xr_xir_value_drop(&array);
    ++w->checks;
    if (w->mode==3) return (XrXirAction){.kind=XR_XIR_ACTION_SUSPEND};
    return module_view_return(view);
}
static void module_view_code_release(void *owner) {++((ModuleViewWitness *)owner)->releases;}
static void module_view_run(XrXirInstance *instance,uint32_t entry) {
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult polled=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
    if (polled.outcome.status!=XR_XIR_CALL_RETURNED) fprintf(stderr,"Module View entry%u status%u value%lld\n",entry,polled.outcome.status,(long long)polled.outcome.value.payload);
    CHECK(polled.outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(result.type==(uint32_t)(entry==2?XR_XIR_I64:XR_XIR_UNIT) && !result.reserved && !result.payload);
}
static XrXirStatus module_view_metadata(const XrXirCompileContext *context,
    const XrXirProgramSpec *spec,XrXirArtifact **output) {
    XrXirArtifact *base=NULL;XrXirStatus status=native_metadata_fixture(context,spec,&base);
    if (status!=XR_XIR_OK) return status;
    XrXirModule module=*xr_xir_compile_artifact_module(base);XrXirFunction functions[5];
    memcpy(functions,module.functions,sizeof(functions));module.stage=XR_XIR_BUILT;module.functions=functions;
    const XrXirBlock block={.count=3};
    const XrXirInstruction ops[]={
        {.op=XR_XIR_CELL_READ,.type=(XrXirType)MP_ARRAY,.args={0}},
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64},
        {.op=XR_XIR_MATCH_FAIL}};
    functions[4].blocks=&block;functions[4].block_count=1;functions[4].instructions=ops;functions[4].instruction_count=3;
    const XrXirBlock caller_block={.count=4};const uint32_t argument=0;
    const XrXirInstruction caller_ops[]={
        {.op=XR_XIR_SLOT_LOAD,.type=(XrXirType)MP_CELL},
        {.op=XR_XIR_CALL,.args={0,1},.immediate=4},
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64},
        {.op=XR_XIR_RETURN,.args={2}}};
    functions[2].blocks=&caller_block;functions[2].block_count=1;functions[2].instructions=caller_ops;
    functions[2].instruction_count=4;functions[2].operands=&argument;functions[2].operand_count=1;
    XrXirArtifact *checked=NULL,*closed=NULL;
    status=xir_fixture_check(context,&module,&checked,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_specialize(checked,&closed,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_lower(closed,&spec->target,output,NULL);
    xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(checked);xr_xir_compile_artifact_free(base);
    return status;
}
static void module_view_cases(void) {
    CHECK(!live && !live_bytes);ModuleViewWitness w={0};NativeFixtureOwner owner={0};
    CHECK(native_fixture_owner_new(&owner)==XR_XIR_OK);
    const XrXirTypeNode nodes[]={
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_U8},
        {.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)MP_ARRAY},
        {.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_U8},
        {.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_U8}};
    XrXirTypes types={nodes,4,NULL,NULL};uint32_t dependency=1;
    XrXirSourceModule modules[]={{.name="root",.name_length=4,.dependencies=&dependency,.dependency_count=1,.initializer=0},
        {.name="other",.name_length=5,.initializer=1}};
    XrXirFunctionIdentity identities[5]={{0},{.module=1},{.exported=1},{.module=1,.exported=1},{.module=1,.exported=1}};
    XrXirSlot slot={0,(XrXirType)MP_CELL,1};
    XrXirDeclarations declarations={.modules=modules,.module_count=2,.functions=identities,.slots=&slot,
        .slot_count=1,.root_module=0,.entry_function=2};
    XrXirCallEntry entries[5]={0};
    for (uint32_t i=0;i<5;++i) entries[i]=(XrXirCallEntry){.abi_version=XR_XIR_CALL_ABI_VERSION,
        .result=i==2?XR_XIR_I64:XR_XIR_UNIT,.state_bytes=sizeof(ModuleViewFrame),
        .resume=i<2?module_view_init:module_view_entry,.release=module_view_cleanup,.environment=&w};
    const XrXirType parameter=(XrXirType)MP_CELL;
    entries[4].parameters=&parameter;entries[4].parameter_count=1;entries[4].resume=module_view_borrowed;
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        entries,5,&declarations,{&w,module_view_code_release},&types,{0}};
    XrXirArtifact *artifact=NULL;CHECK(module_view_metadata(&owner.context,&spec,&artifact)==XR_XIR_OK);
    spec.types=xr_xir_compile_artifact_module(artifact)->types;spec.packet=xir_program_packet_borrow(artifact);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(xr_xir_compile_artifact_context(artifact),&spec,&program)==XR_XIR_OK);
    xr_xir_compile_artifact_free(artifact);native_fixture_owner_free(&owner);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *first=NULL,*second=NULL;
    CHECK(xr_xir_instance_new(program,&config,&first)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program,&config,&second)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);w.other=second;
    module_view_run(first,2);
    int64_t length=77;CHECK(xr_xir_view_len(&w.late,&w.access,&length)==XR_XIR_VALUE_BAD_ARGUMENT && length==77);
    xr_xir_value_drop(&w.late);w.access=(XrXirViewAccess){0};
    w.mode=1;module_view_run(first,3);
    CHECK(xr_xir_value_copy(&w.saved,&w.foreign)==XR_XIR_VALUE_OK);
    w.mode=2;module_view_run(second,2);xr_xir_value_drop(&w.foreign);
    w.mode=4;module_view_run(first,2);
    w.mode=3;CHECK(xr_xir_instance_start(first,2,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(first,UINT64_MAX).outcome.status==XR_XIR_CALL_SUSPENDED);
    CHECK(xr_xir_instance_cancel_current(first)==XR_XIR_CALL_CANCEL_REQUESTED);
    CHECK(xr_xir_instance_poll_bounded(first,UINT64_MAX).outcome.status==XR_XIR_CALL_CANCELLED);
    length=77;CHECK(xr_xir_view_len(&w.late,&w.access,&length)==XR_XIR_VALUE_BAD_ARGUMENT && length==77);
    CHECK(w.checks==5);
    CHECK(xr_xir_instance_free(first)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_free(second)==XR_XIR_CALL_READY);
    xr_xir_value_drop(&w.saved);xr_xir_value_drop(&w.foreign);xr_xir_value_drop(&w.late);
    CHECK(w.releases==1 && !live && !live_bytes);
}
#include "xir_view_access_default_access_cases.h"
int main(int argc,char **argv) {
    if (argc==2 && !strcmp(argv[1],"--root-model")) {access_root_model_dump();return 0;}
    if (argc==2 && !strcmp(argv[1],"--slot-init")) {slot_publication_cases();module_view_cases();return 0;}
    bool normal=argc==2 && !strcmp(argv[1],"--normal");CHECK(argc==1 || normal);setvbuf(stdout,NULL,_IONBF,0);
    slot_publication_cases();module_view_cases();
    default_access_cases(normal);
    for (uint32_t kind=0;kind<A_CASES;++kind) access_resources(kind,normal);
    for (uint32_t kind=148;kind<A_CASES;++kind) access_frame_boundary(kind);
    puts("Proven conflicts reject, unresolved epochs stay explicit, and Source execution remains closed");return 0;
}
