/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_view_may_owned.c - Possible owners and every panic prefix
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_view_prepare_internal.h"
#include "xir/xxir_view_may_owned_internal.h"
#include "xir/xxir_view_types_internal.h"
#include "tuple_owner_observer.h"

enum { M_SHARED=256,M_MUT,M_OPTIONAL,M_OPTIONAL_MUT,M_TUPLE,M_CALLABLE,M_CASES=21 };
typedef struct MayFixture {
    XrXirTypeNode nodes[6];XrXirCallableParameter fields[2],ref;XrXirTypes types;
    XrXirType parameters[4],callee[2];XrXirInstruction ops[160],returns[2];
    XrXirBlock blocks[8],empty[2];uint32_t operands[128];XrXirFunction functions[3];
    XrXirSourceModule source;XrXirFunctionIdentity identities[3];XrXirDeclarations declarations;XrXirModule module;
} MayFixture;
typedef struct MayObserved {
    uint64_t before[160][3],after[160][3],entries[8][3],edges[8][8][3];
    bool instructions[160],blocks[8],panic[8][8];uint32_t events,owners,words,last[8];
} MayObserved;
static uint32_t may_op(MayFixture *f,XrXirInstruction op) {
    CHECK(f->functions[0].instruction_count<160);uint32_t i=f->functions[0].instruction_count++;
    f->ops[i]=op;return f->functions[0].parameter_count+i;
}
static uint32_t may_group(MayFixture *f,const uint32_t *values,uint32_t count) {
    uint32_t at=f->functions[0].operand_count;CHECK(at+count<=128);
    if (count) memcpy(f->operands+at,values,(size_t)count*sizeof(*values));
    f->functions[0].operand_count+=count;return at;
}
static void may_base(MayFixture *f) {
    memset(f,0,sizeof(*f));f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_I64};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_MUT_SLICE,.element=XR_XIR_I64};
    f->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)M_SHARED};
    f->nodes[3]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)M_MUT};
    f->fields[0]=(XrXirCallableParameter){(XrXirType)M_MUT,XR_PARAM_READ};
    f->fields[1]=(XrXirCallableParameter){(XrXirType)M_OPTIONAL_MUT,XR_PARAM_READ};
    f->nodes[4]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=f->fields,.parameter_count=2};
    f->ref=(XrXirCallableParameter){(XrXirType)M_MUT,XR_PARAM_READ};
    f->nodes[5]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.parameters=&f->ref,.parameter_count=1,
        .flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};f->types=(XrXirTypes){f->nodes,6,NULL,NULL};
    f->parameters[0]=XR_XIR_BOOL;f->parameters[1]=(XrXirType)M_SHARED;
    f->parameters[2]=f->parameters[3]=(XrXirType)M_MUT;f->callee[0]=f->callee[1]=(XrXirType)M_MUT;
    f->functions[0]=(XrXirFunction){.name="owners",.name_length=6,.parameters=f->parameters,.parameter_count=3,
        .blocks=f->blocks,.block_count=1,.instructions=f->ops,.operands=f->operands};
    for (uint32_t i=0;i<2;++i) {
        f->returns[i]=(XrXirInstruction){.op=XR_XIR_RETURN};f->empty[i]=(XrXirBlock){.count=1};
        f->functions[i+1]=(XrXirFunction){.name="body",.name_length=4,.blocks=f->empty+i,.block_count=1,
            .instructions=f->returns+i,.instruction_count=1};
    }
    f->functions[2].parameters=f->callee;f->functions[2].parameter_count=1;
    f->source=(XrXirSourceModule){.name="root",.name_length=4,.initializer=1};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,
        .root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=3,
        .declarations=&f->declarations,.types=&f->types,.linkage_kind=XR_XIR_LIBRARY};
}
static void may_end(MayFixture *f,uint32_t value) {may_op(f,(XrXirInstruction){.op=XR_XIR_VIEW_END,.args={value}});}
static void may_cleanup(MayFixture *f,const uint32_t *values,uint32_t count) {
    uint32_t at=may_group(f,values,count);may_op(f,(XrXirInstruction){.op=XR_XIR_VIEW_CLEANUP,.args={at,count}});
}
static void may_diamond(MayFixture *f,uint32_t kind) {
    may_op(f,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={1,2}});
    uint32_t first=f->functions[0].instruction_count;
    if (kind==2 || kind==3) may_end(f,1);
    may_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3}});uint32_t second=f->functions[0].instruction_count;
    if (kind==3) may_end(f,1);
    may_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3}});uint32_t join=f->functions[0].instruction_count;
    if (kind==4 || kind==5) {
        uint32_t value=kind==4?2u:1u,at=may_group(f,(uint32_t[]){1,value,2,value},4);
        uint32_t phi=may_op(f,(XrXirInstruction){.op=XR_XIR_PHI,
            .type=(XrXirType)(kind==4?M_MUT:M_SHARED),.args={at,4}});may_end(f,phi);
    } else may_cleanup(f,(uint32_t[]){1},1);
    may_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});
    f->functions[0].block_count=4;f->blocks[0]=(XrXirBlock){.count=first};
    f->blocks[1]=(XrXirBlock){.first=first,.count=second-first};
    f->blocks[2]=(XrXirBlock){.first=second,.count=join-second};
    f->blocks[3]=(XrXirBlock){.first=join,.count=f->functions[0].instruction_count-join};
}
static void may_loop(MayFixture *f) {
    may_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}});
    uint32_t copy=may_op(f,(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)M_SHARED,.args={1}});may_end(f,copy);
    may_op(f,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={1,2}});may_end(f,1);
    may_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});f->functions[0].block_count=3;
    f->blocks[0]=(XrXirBlock){.count=1};f->blocks[1]=(XrXirBlock){.first=1,.count=3};f->blocks[2]=(XrXirBlock){.first=4,.count=2};
}
static void may_panic(MayFixture *f,bool mutable) {
    may_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}});
    uint32_t copy=may_op(f,(XrXirInstruction){.op=mutable?XR_XIR_NULLABLE_SOME:XR_XIR_COPY,
        .type=(XrXirType)(mutable?M_OPTIONAL_MUT:M_SHARED),.args={mutable?2u:1u}});
    if (!mutable) may_end(f,1);may_end(f,copy);may_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});
    uint32_t handler=f->functions[0].instruction_count;
    may_op(f,(XrXirInstruction){.op=XR_XIR_PANIC_CATCH,.type=XR_XIR_PANIC_INFO});
    may_cleanup(f,(uint32_t[]){mutable?2u:1u,copy},2);may_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});
    f->functions[0].block_count=3;f->blocks[0]=(XrXirBlock){.count=1};
    f->blocks[1]=(XrXirBlock){.first=1,.count=handler-1,.panic=2};f->blocks[2]=(XrXirBlock){.first=handler,.count=3};
}
static void may_call(MayFixture *f,uint32_t kind) {
    uint32_t callee=0;
    if (kind==15) callee=may_op(f,(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)M_CALLABLE,.immediate=2});
    if (kind==20) f->functions[2].parameter_count=2;
    uint32_t at=may_group(f,(uint32_t[]){2,2},kind==20?2u:1u);
    may_op(f,(XrXirInstruction){.op=kind==15?XR_XIR_CALL_INDIRECT:XR_XIR_CALL,.args={at,kind==20?2u:1u},
        .immediate=kind==15?callee:2});may_end(f,2);
}
static void may_invoke(MayFixture *f) {
    f->functions[2].result=(XrXirType)M_SHARED;f->callee[0]=(XrXirType)M_SHARED;
    uint32_t at=may_group(f,(uint32_t[]){1},1);
    may_op(f,(XrXirInstruction){.op=XR_XIR_INVOKE,.type=(XrXirType)M_SHARED,.args={at,1},.targets={1,2},.immediate=2});
    uint32_t result=may_op(f,(XrXirInstruction){.op=XR_XIR_INVOKE_RESULT,.type=(XrXirType)M_SHARED});may_end(f,result);
    may_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});may_op(f,(XrXirInstruction){.op=XR_XIR_INVOKE_ERROR,.type=XR_XIR_ERROR});
    may_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});f->functions[0].block_count=3;
    f->blocks[0]=(XrXirBlock){.count=1};f->blocks[1]=(XrXirBlock){.first=1,.count=3};f->blocks[2]=(XrXirBlock){.first=4,.count=2};
}
static void may_case(MayFixture *f,uint32_t kind) {
    may_base(f);
    if (kind>=2 && kind<=5) {may_diamond(f,kind);return;}
    if (kind==6) {may_loop(f);return;}if (kind==7 || kind==8) {may_panic(f,kind==8);return;}
    if (kind==16) {may_invoke(f);return;}
    if (kind==0) {
        uint32_t copy=may_op(f,(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)M_SHARED,.args={1}});
        may_end(f,1);may_op(f,(XrXirInstruction){.op=XR_XIR_VIEW_LEN,.type=XR_XIR_I64,.args={copy}});may_end(f,copy);
    } else if (kind==1 || kind==10) {
        uint32_t value=may_op(f,(XrXirInstruction){.op=kind==1?XR_XIR_NULLABLE_SOME:XR_XIR_NULLABLE_NONE,
            .type=(XrXirType)(kind==1?M_OPTIONAL_MUT:M_OPTIONAL),.args={kind==1?2u:0u}});may_end(f,value);
    } else if (kind==9 || kind==15 || kind==20) may_call(f,kind);
    else if (kind==11) {
        may_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});uint32_t value=may_op(f,
            (XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)M_SHARED,.args={1}});may_end(f,value);
        f->functions[0].block_count=2;f->blocks[0]=(XrXirBlock){.count=1};f->blocks[1]=(XrXirBlock){.first=1,.count=3};
    } else if (kind==12) {
        for (uint32_t i=0;i<70;++i) may_op(f,(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)M_SHARED,.args={1}});
    } else if (kind==13) {f->functions[0].parameter_count=0;f->functions[0].parameters=NULL;}
    else if (kind==14) {
        f->functions[0].parameter_count=4;
        uint32_t some=may_op(f,(XrXirInstruction){.op=XR_XIR_NULLABLE_SOME,.type=(XrXirType)M_OPTIONAL_MUT,.args={3}});
        uint32_t at=may_group(f,(uint32_t[]){2,some},2);
        uint32_t tuple=may_op(f,(XrXirInstruction){.op=XR_XIR_TUPLE_NEW,.type=(XrXirType)M_TUPLE,.args={at,2}});may_end(f,tuple);
    } else if (kind==17) {
        uint32_t zero=may_op(f,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64});
        uint32_t one=may_op(f,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=1});
        uint32_t at=may_group(f,(uint32_t[]){2,zero,one},3);
        uint32_t child=may_op(f,(XrXirInstruction){.op=XR_XIR_VIEW_REBORROW,.type=(XrXirType)M_SHARED,.args={at,3}});
        may_end(f,2);may_end(f,child);
    } else if (kind==18) {
        f->functions[0].result=(XrXirType)M_SHARED;may_op(f,(XrXirInstruction){.op=XR_XIR_RETURN,.args={1}});
    } else if (kind==19) {may_end(f,1);may_op(f,(XrXirInstruction){.op=XR_XIR_VIEW_LEN,.type=XR_XIR_I64,.args={1}});}
    else CHECK(false);
    if (kind!=18) may_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});
    if (f->functions[0].block_count==1) f->blocks[0].count=f->functions[0].instruction_count;
}
static void may_pack(const XrXirViewOwnerSet *set,uint64_t *mask) {
    memset(mask,0,3*sizeof(*mask));
    for (uint32_t i=0;i<set->count;++i) if (set->bits[i/64]&(UINT64_C(1)<<(i%64))) {
        uint32_t v=set->values[i];CHECK(v<192);mask[v/64]|=UINT64_C(1)<<(v%64);
    }
}
static XrXirStatus may_observe(void *user,const XrXirCompileContext *context,const XrXirViewOwnerEvent *e) {
    if (e->function) return XR_XIR_OK;
    if (!xir_compile_work(context,(uint64_t)e->before.count*2u+1u)) return XR_XIR_BUDGET;
    MayObserved *o=user;CHECK(e->block<8);++o->events;o->owners=e->before.count;o->words=e->before.words;
    if (e->kind==XR_XIR_VIEW_OWNER_ENTRY) {
        CHECK(!o->blocks[e->block]);o->blocks[e->block]=true;may_pack(&e->before,o->entries[e->block]);
    } else if (e->kind==XR_XIR_VIEW_OWNER_INSTRUCTION) {
        CHECK(e->instruction<160 && !o->instructions[e->instruction]);o->instructions[e->instruction]=true;
        CHECK(o->last[e->block]<=e->instruction);o->last[e->block]=e->instruction+1u;
        may_pack(&e->before,o->before[e->instruction]);may_pack(&e->after,o->after[e->instruction]);
    } else {
        CHECK(e->kind==XR_XIR_VIEW_OWNER_EDGE && e->target<8);may_pack(&e->after,o->edges[e->block][e->target]);
        o->panic[e->block][e->target]=e->panic;
    }
    return XR_XIR_OK;
}
static void may_mask_check(const uint64_t *actual,const uint32_t *values,size_t count) {
    uint64_t expected[3]={0};for (size_t i=0;i<count;++i) expected[values[i]/64]|=UINT64_C(1)<<(values[i]%64);
    CHECK(!memcmp(actual,expected,sizeof(expected)));
}
#define MASK(actual,...) may_mask_check(actual,(uint32_t[]){__VA_ARGS__},sizeof((uint32_t[]){__VA_ARGS__})/sizeof(uint32_t))
static void may_assert(uint32_t kind,const MayObserved *o) {
    if (kind==13) {CHECK(!o->events);return;}CHECK(o->events && o->blocks[0]);
    if (kind==0) {MASK(o->after[0],1,2,3);MASK(o->after[1],2,3);MASK(o->after[3],2);}
    else if (kind==1) {MASK(o->after[0],1,3);MASK(o->after[1],1);}
    else if (kind==2 || kind==3) {
        if (kind==2) {MASK(o->entries[3],1,2);MASK(o->after[4],2);}else {MASK(o->entries[3],2);MASK(o->after[5],2);}
    } else if (kind==4 || kind==5) {
        if (kind==4) {MASK(o->edges[1][3],1);MASK(o->entries[3],1);MASK(o->after[3],1,6);MASK(o->after[4],1);}
        else {MASK(o->edges[1][3],1,2);MASK(o->entries[3],1,2);MASK(o->after[3],1,2,6);MASK(o->after[4],1,2);}
    } else if (kind==6) {MASK(o->entries[1],1,2);MASK(o->after[1],1,2,4);MASK(o->after[2],1,2);MASK(o->after[4],2);}
    else if (kind==7 || kind==8) {
        CHECK(o->panic[1][2]);MASK(o->entries[2],1,2,4);MASK(o->edges[1][2],1,2,4);
        if (kind==7) {MASK(o->after[4],2);MASK(o->after[6],2);}else {MASK(o->after[3],1);MASK(o->after[5],1);}
    } else if (kind==9 || kind==15) {MASK(o->after[kind==9?0:1],1,2);MASK(o->after[kind==9?1:2],1);}
    else if (kind==10) {MASK(o->after[0],1,2,3);MASK(o->after[1],1,2);}
    else if (kind==12) {
        CHECK(o->owners==72 && o->words==2);
        uint64_t expected[3]={UINT64_MAX-1u,UINT64_C(0x1ff),0};CHECK(!memcmp(expected,o->after[69],sizeof(expected)));
    } else if (kind==14) {MASK(o->after[0],1,2,4);MASK(o->after[1],1,5);MASK(o->after[2],1);}
    else if (kind==16) {CHECK(o->owners==3);MASK(o->after[0],1,2);MASK(o->after[1],1,2,4);MASK(o->after[2],1,2);MASK(o->entries[2],1,2);}
    else if (kind==17) {MASK(o->after[2],1,2,5);MASK(o->after[3],1,5);MASK(o->after[4],1);}
    else if (kind==18) {MASK(o->after[0],2);}else CHECK(false);
}
static XrXirStatus may_stop(void *user,const XrXirCompileContext *context,const XrXirViewOwnerEvent *event) {
    (void)context;(void)event;++*(uint32_t *)user;return XR_XIR_BAD_VALUE;
}
static XrXirStatus may_sequence(const XrXirCompileContext *context,uint32_t kind) {
    MayFixture f;may_case(&f,kind);if (!f.functions[0].operand_count) f.functions[0].operands=NULL;
    unsigned char before[sizeof(f)];memcpy(before,&f,sizeof(f));
    XrXirConstruction *construction=NULL;XrXirDiagnostic diagnostic={0};MayObserved observed={0};
    XrXirStatus status=xir_fixture_construction(context,&f.types,&construction);if (status!=XR_XIR_OK) goto done;
    status=xir_compile_view_prepare_ordinary(context,&f.module,construction,&diagnostic);
    if (status==XR_XIR_BUDGET || status==XR_XIR_OUT_OF_MEMORY) goto done;
    if (kind==11) {CHECK(status==XR_XIR_BAD_STRUCTURE);goto done;}
    if (status!=XR_XIR_OK) fprintf(stderr,"may ordinary kind=%u status=%u f=%u b=%u i=%u\n",kind,status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_OK);status=xir_compile_view_affine_verified(context,&f.module,&diagnostic);
    if (status!=XR_XIR_OK && status!=XR_XIR_BUDGET && status!=XR_XIR_OUT_OF_MEMORY && kind<19)
        fprintf(stderr,"may affine kind=%u status=%u f=%u b=%u i=%u\n",kind,status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    if (status!=XR_XIR_OK) goto done;
    XrXirCompileContext bounded=*context;bounded.limits.frame_bytes=1;
    CHECK(xir_compile_view_may_owned_verified(&bounded,&f.module,may_observe,&observed,NULL)==XR_XIR_BUDGET && !observed.events);
    status=xir_compile_view_may_owned_verified(context,&f.module,may_observe,&observed,&diagnostic);
    if (status!=XR_XIR_OK) goto done;may_assert(kind,&observed);
    if (kind!=13) {
        uint32_t calls=0;status=xir_compile_view_may_owned_verified(context,&f.module,may_stop,&calls,&diagnostic);
        if (status==XR_XIR_BUDGET || status==XR_XIR_OUT_OF_MEMORY) goto done;
        CHECK(status==XR_XIR_BAD_VALUE && calls==1);
    }
    status=xir_compile_view_may_owned_verified(context,&f.module,NULL,NULL,&diagnostic);
done:CHECK(!memcmp(before,&f,sizeof(f)));xr_xir_compile_construction_free(construction);return status;
}
static void may_resources(uint32_t kind,bool normal) {
    XrXirStatus expected=kind==11?XR_XIR_BAD_STRUCTURE:kind>=19?XR_XIR_BAD_VALUE:XR_XIR_OK;
    XrXirCompileContext c=owner_new(caps());
    uint64_t baseline=stats(&c).live_bytes;size_t first=attempts;XrXirStatus status=may_sequence(&c,kind);
    if (status!=expected) fprintf(stderr,"may kind=%u status=%u expected=%u\n",kind,status,expected);CHECK(status==expected);
    size_t points=attempts-first;XrCompileResourceStats measured=stats(&c);CHECK(points && points<4096);
    CHECK(may_sequence(&c,kind)==expected);owner_free(&c,baseline);
    if (!normal) for (size_t p=0;p<points;++p) {
        c=owner_new(caps());baseline=stats(&c).live_bytes;fail_at=attempts+p;injected=false;
        status=may_sequence(&c,kind);fail_at=SIZE_MAX;
        if (status!=XR_XIR_OUT_OF_MEMORY || !injected) fprintf(stderr,"may kind=%u FI=%zu/%zu status=%u\n",kind,p,points,status);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && injected);owner_free(&c,baseline);
    }
    if (!normal) for (uint32_t axis=0;axis<3;++axis) for (uint32_t delta=0;delta<3;++delta) {
        XrCompileResourceLimits limits=caps();uint64_t *bound=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        *bound=(axis==0?measured.allocated_bytes:axis==1?measured.peak_bytes:measured.work)+delta-1u;
        c=owner_new(limits);baseline=stats(&c).live_bytes;CHECK(may_sequence(&c,kind)==(delta?expected:XR_XIR_BUDGET));owner_free(&c,baseline);
    }
    printf("May-owned kind=%u: %zu allocation points owner2/physical0%s PASS\n",kind,points,
        normal?"":"/all actual failures/three axes exact+minus1+plus1");
}
int main(int argc,char **argv) {
    bool normal=argc==2 && !strcmp(argv[1],"--normal");CHECK(argc==1 || normal);setvbuf(stdout,NULL,_IONBF,0);
    for (uint32_t kind=0;kind<M_CASES;++kind) may_resources(kind,normal);
    puts("Possible owned slots include every panic prefix and grant no logical or access permission");return 0;
}
