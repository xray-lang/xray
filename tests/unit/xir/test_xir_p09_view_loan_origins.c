/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_view_loan_origins.c - Scope producers retain PHI correlations
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_view_prepare_internal.h"
#include "xir/xxir_view_loan_origins_internal.h"
#include "tuple_owner_observer.h"
enum { L_ARRAY=256,L_CELL,L_SHARED,L_MUT,L_OPTIONAL,L_TUPLE,L_CALLABLE,L_CASES=24 };
typedef struct LoanFixture {
    XrXirTypeNode nodes[80];XrXirCallableParameter fields[80][3],parameter;XrXirTypes types;
    XrXirType parameters[4],callee;XrXirInstruction ops[160],other[2];uint32_t operands[512];
    XrXirBlock blocks[4],single[2];XrXirFunction functions[3];
    XrXirSourceModule source;XrXirFunctionIdentity identities[3];XrXirDeclarations declarations;XrXirModule module;
    XirViewRootRequest request,second;uint32_t first,child;
} LoanFixture;
typedef struct LoanObserved {
    XirViewRootResolver *resolver;const XirViewRootRequest *request;uint32_t count;
    XrXirViewLoanOrigin events[8];uint32_t selections[8][8];XrXirViewLoanChoice choices[8][8];
} LoanObserved;
static uint32_t loan_op(LoanFixture *f,XrXirInstruction op) {
    CHECK(f->functions[0].instruction_count<160);uint32_t i=f->functions[0].instruction_count++;
    f->ops[i]=op;return 4u+i;
}
static uint32_t loan_group(LoanFixture *f,const uint32_t *values,uint32_t count) {
    uint32_t at=f->functions[0].operand_count;CHECK(at+count<=512);
    if (count) memcpy(f->operands+at,values,(size_t)count*sizeof(*values));f->functions[0].operand_count+=count;return at;
}
static uint32_t loan_view(LoanFixture *f,uint32_t source,bool child) {
    uint32_t at=loan_group(f,(uint32_t[]){source,4,5},3);
    return loan_op(f,(XrXirInstruction){.op=child?XR_XIR_VIEW_REBORROW:XR_XIR_VIEW_NEW,
        .type=(XrXirType)L_SHARED,.args={at,3}});
}
static void loan_base(LoanFixture *f) {
    memset(f,0,sizeof(*f));f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)L_ARRAY};
    f->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_I64};
    f->nodes[3]=(XrXirTypeNode){.kind=XR_XIR_TYPE_MUT_SLICE,.element=XR_XIR_I64};
    f->nodes[4]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)L_SHARED};
    f->fields[5][0]=(XrXirCallableParameter){XR_XIR_UNIT,XR_PARAM_READ};
    f->fields[5][1]=(XrXirCallableParameter){(XrXirType)L_SHARED,XR_PARAM_READ};
    f->fields[5][2]=(XrXirCallableParameter){(XrXirType)L_OPTIONAL,XR_PARAM_READ};
    f->nodes[5]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=f->fields[5],.parameter_count=3};
    f->parameter=f->fields[5][1];f->nodes[6]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.parameters=&f->parameter,
        .parameter_count=1,.result=(XrXirType)L_SHARED,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    f->types=(XrXirTypes){f->nodes,7,NULL,NULL};f->parameters[0]=XR_XIR_BOOL;
    f->parameters[1]=(XrXirType)L_CELL;f->parameters[2]=(XrXirType)L_SHARED;f->parameters[3]=(XrXirType)L_CALLABLE;
    f->functions[0]=(XrXirFunction){.name="loans",.name_length=5,.parameters=f->parameters,.parameter_count=4,
        .blocks=f->blocks,.block_count=1,.instructions=f->ops};
    for (uint32_t i=0;i<2;++i) {
        f->other[i]=(XrXirInstruction){.op=XR_XIR_RETURN};f->single[i]=(XrXirBlock){.count=1};
        f->functions[i+1]=(XrXirFunction){.name="body",.name_length=4,.blocks=f->single+i,.block_count=1,
            .instructions=f->other+i,.instruction_count=1};
    }
    f->callee=(XrXirType)L_SHARED;f->functions[1].parameters=&f->callee;f->functions[1].parameter_count=1;
    f->functions[1].result=(XrXirType)L_SHARED;
    f->source=(XrXirSourceModule){.name="root",.name_length=4,.initializer=2};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,
        .root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=3,
        .declarations=&f->declarations,.types=&f->types,.linkage_kind=XR_XIR_LIBRARY};
    loan_op(f,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64});
    loan_op(f,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=1});
}
static uint32_t loan_phi(LoanFixture *f,bool loop) {
    if (loop) {
        loan_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}});
        uint32_t at=loan_group(f,(uint32_t[]){0,2,1,7},4);
        uint32_t value=loan_op(f,(XrXirInstruction){.op=XR_XIR_PHI,.type=(XrXirType)L_SHARED,.args={at,4}});
        loan_op(f,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={1,2}});
        loan_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});f->functions[0].block_count=3;
        f->blocks[0]=(XrXirBlock){.count=3};f->blocks[1]=(XrXirBlock){.first=3,.count=2};f->blocks[2]=(XrXirBlock){.first=5,.count=1};
        return value;
    }
    uint32_t a=loan_view(f,1,false),b=loan_view(f,1,false);
    loan_op(f,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={1,2}});
    loan_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3}});loan_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3}});
    uint32_t at=loan_group(f,(uint32_t[]){1,a,2,b},4);
    uint32_t value=loan_op(f,(XrXirInstruction){.op=XR_XIR_PHI,.type=(XrXirType)L_SHARED,.args={at,4}});
    at=loan_group(f,(uint32_t[]){1,b,2,a},4);
    uint32_t second=loan_op(f,(XrXirInstruction){.op=XR_XIR_PHI,.type=(XrXirType)L_SHARED,.args={at,4}});
    f->second=(XirViewRootRequest){0,second,UINT32_MAX,NULL,0};loan_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});
    f->functions[0].block_count=4;f->blocks[0]=(XrXirBlock){.count=5};f->blocks[1]=(XrXirBlock){.first=5,.count=1};
    f->blocks[2]=(XrXirBlock){.first=6,.count=1};f->blocks[3]=(XrXirBlock){.first=7,.count=3};return value;
}
static uint32_t loan_call(LoanFixture *f,uint32_t kind) {
    if (kind==23) {
        f->callee=(XrXirType)L_CELL;f->functions[1].instructions=f->ops+100;f->functions[1].instruction_count=4;
        f->functions[1].operands=f->operands+500;f->functions[1].operand_count=3;f->single[0].count=4;
        f->operands[500]=0;f->operands[501]=1;f->operands[502]=2;
        f->ops[100]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64};
        f->ops[101]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=1};
        f->ops[102]=(XrXirInstruction){.op=XR_XIR_VIEW_NEW,.type=(XrXirType)L_SHARED,.args={0,3}};
        f->ops[103]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={3}};
    }
    uint32_t at=loan_group(f,(uint32_t[]){kind==23?1u:2u},1);
    uint32_t value=loan_op(f,(XrXirInstruction){.op=kind==13?XR_XIR_CALL_INDIRECT:kind==14?XR_XIR_INVOKE:XR_XIR_CALL,
        .type=(XrXirType)L_SHARED,.args={at,1},.targets={kind==14?1u:0u,kind==14?2u:0u},.immediate=kind==13?3:1});
    if (kind!=14) return value;
    value=loan_op(f,(XrXirInstruction){.op=XR_XIR_INVOKE_RESULT,.type=(XrXirType)L_SHARED,.immediate=2});
    loan_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});loan_op(f,(XrXirInstruction){.op=XR_XIR_INVOKE_ERROR,.type=XR_XIR_ERROR,.immediate=2});
    loan_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});f->functions[0].block_count=3;
    f->blocks[0]=(XrXirBlock){.count=3};f->blocks[1]=(XrXirBlock){.first=3,.count=2};f->blocks[2]=(XrXirBlock){.first=5,.count=2};return value;
}
static void loan_case(LoanFixture *f,uint32_t kind) {
    loan_base(f);uint32_t value;
    if (kind==9 || kind==10) value=loan_phi(f,kind==10);
    else if ((kind>=12 && kind<=14) || kind==23) value=loan_call(f,kind);
    else {
        value=loan_view(f,1,false);f->first=value;
        if (kind==1 || kind>=18) {
            if (kind>=18) f->ops[value-4u].type=(XrXirType)L_MUT;
            if (kind==1 || (kind>=19 && kind<=21)) {
                value=loan_view(f,value,true);f->child=value;
                if (kind==19) f->ops[value-4u].type=(XrXirType)L_MUT;
            }
            if (kind==21 || kind==22) loan_op(f,(XrXirInstruction){.op=XR_XIR_VIEW_END,.args={f->first}});
        }
        else if (kind==2) value=loan_op(f,(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)L_SHARED,.args={value}});
        else if (kind>=3 && kind<=6) {
            uint32_t some=loan_op(f,(XrXirInstruction){.op=kind==4?XR_XIR_NULLABLE_NONE:XR_XIR_NULLABLE_SOME,
                .type=(XrXirType)L_OPTIONAL,.args={kind==4?0u:value}});value=kind==3 || kind==4?some:value;
            if (kind==5 || kind==6) {
                uint32_t at=loan_group(f,(uint32_t[]){value,some},2);
                value=loan_op(f,(XrXirInstruction){.op=XR_XIR_TUPLE_NEW,.type=(XrXirType)L_TUPLE,.args={at,2}});
                if (kind==6) {
                    value=loan_op(f,(XrXirInstruction){.op=XR_XIR_TUPLE_FIELD,.type=(XrXirType)L_OPTIONAL,.args={value},.immediate=2});
                    value=loan_op(f,(XrXirInstruction){.op=XR_XIR_NULLABLE_UNWRAP,.type=(XrXirType)L_SHARED,.args={value}});
                }
            }
        } else if (kind==7 || kind==8 || kind==17) {f->parameters[2]=(XrXirType)L_TUPLE;value=2;}
        else if (kind==11) {
            XrXirType previous=(XrXirType)L_SHARED;
            for (uint32_t i=7;i<77;++i) {
                f->fields[i][0]=(XrXirCallableParameter){XR_XIR_UNIT,XR_PARAM_READ};
                f->fields[i][1]=f->fields[i][2]=(XrXirCallableParameter){previous,XR_PARAM_READ};
                f->nodes[i]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=f->fields[i],.parameter_count=3};
                uint32_t at=loan_group(f,(uint32_t[]){value,value},2);previous=(XrXirType)(256u+i);
                value=loan_op(f,(XrXirInstruction){.op=XR_XIR_TUPLE_NEW,.type=previous,.args={at,2}});
            }
            f->types.count=77;
        }
    }
    f->request=(XirViewRootRequest){0,kind==15?UINT32_MAX:kind==16?1u:value,UINT32_MAX,NULL,0};
    if (f->functions[0].block_count==1) {loan_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});f->blocks[0].count=f->functions[0].instruction_count;}
    if (f->functions[0].operand_count) f->functions[0].operands=f->operands;
}
static XrXirStatus loan_observe(void *user,const XrXirCompileContext *context,const XrXirViewLoanOrigin *e) {
    LoanObserved *o=user;CHECK(o->count<8 && e->selection_count<=8 && e->choice_count<=8);
    CHECK(xir_compile_view_loan_origins(o->resolver,o->request,NULL,NULL,NULL)==XR_XIR_BAD_STAGE);
    CHECK(xir_compile_view_root_resolver_query(o->resolver,o->request,NULL,NULL,NULL)==XR_XIR_BAD_STAGE);
    if (!xir_compile_work(context,(uint64_t)e->selection_count+e->choice_count)) return XR_XIR_BUDGET;
    uint32_t i=o->count++;o->events[i]=*e;
    if (e->selection_count) memcpy(o->selections[i],e->selection,e->selection_count*sizeof(uint32_t));
    if (e->choice_count) memcpy(o->choices[i],e->choices,e->choice_count*sizeof(*e->choices));
    o->events[i].selection=o->selections[i];o->events[i].choices=o->choices[i];return XR_XIR_OK;
}
static void loan_assert(const LoanFixture *f,uint32_t kind,const LoanObserved *o) {
    if (kind==9 || kind==10) {CHECK(o->count==2);return;}CHECK(o->count==1);
    const XrXirViewLoanOrigin *e=o->events;CHECK(!e->repeated_phi && !e->choice_count);
    if (kind==1 || (kind>=19 && kind<=21)) {
        CHECK(e->kind==XR_XIR_VIEW_LOAN_REBORROW && e->value==f->child && e->source==f->first);
        CHECK(e->type==(XrXirType)(kind==19?L_MUT:L_SHARED));
    }
    else if (kind==4) CHECK(e->kind==XR_XIR_VIEW_LOAN_ABSENT && e->source==UINT32_MAX);
    else if (kind==7 || kind==8) {
        CHECK(e->kind==XR_XIR_VIEW_LOAN_INPUT && e->value==2 && e->source==UINT32_MAX);
        if (kind==7) CHECK(e->type==(XrXirType)L_TUPLE && !e->selection_count);
        else CHECK(e->type==(XrXirType)L_SHARED && e->selection_count==2 && e->selection[0]==2 && e->selection[1]==UINT32_MAX);
    } else if ((kind>=12 && kind<=14) || kind==23) CHECK(e->kind==XR_XIR_VIEW_LOAN_CALL && e->source==(kind==23?1u:2u) && e->value==f->request.value);
    else {
        CHECK(e->kind==XR_XIR_VIEW_LOAN_NEW && e->source==1 && e->value==f->first);
        CHECK(e->type==(XrXirType)(kind==18 || kind==22?L_MUT:L_SHARED));
    }
}
static XrXirStatus loan_stop(void *user,const XrXirCompileContext *context,const XrXirViewLoanOrigin *event) {
    (void)context;(void)event;++*(uint32_t *)user;return XR_XIR_BAD_VALUE;
}
static XrXirStatus loan_sequence(const XrXirCompileContext *context,uint32_t kind) {
    LoanFixture f;loan_case(&f,kind);uint32_t selection[]={2,UINT32_MAX},invalid=9;
    if (kind==8) {f.request.selection=selection;f.request.selection_count=2;}
    if (kind==17) {f.request.selection=&invalid;f.request.selection_count=1;}
    unsigned char before[sizeof(f)];memcpy(before,&f,sizeof(f));XrXirConstruction *construction=NULL;
    XirViewRootResolver *resolver=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_construction(context,&f.types,&construction);if (status!=XR_XIR_OK) goto done;
    status=xir_compile_view_prepare_ordinary(context,&f.module,construction,&diagnostic);
    if (status==XR_XIR_BUDGET || status==XR_XIR_OUT_OF_MEMORY) goto done;
    if (status!=XR_XIR_OK) fprintf(stderr,"loan ordinary kind=%u status=%u at %u/%u/%u\n",kind,status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_OK);status=xir_compile_view_root_resolver_new(context,&f.module,&resolver,&diagnostic);
    if (status!=XR_XIR_OK) goto done;LoanObserved observed={.resolver=resolver,.request=&f.request};
    status=xir_compile_view_loan_origins(resolver,&f.request,loan_observe,&observed,&diagnostic);
    if (status!=XR_XIR_OK) goto done;loan_assert(&f,kind,&observed);
    if (kind==9) {
        LoanObserved other={.resolver=resolver,.request=&f.second};
        status=xir_compile_view_loan_origins(resolver,&f.second,loan_observe,&other,&diagnostic);if (status!=XR_XIR_OK) goto done;
        CHECK(other.count==2);
        for (uint32_t i=0;i<2;++i) for (uint32_t j=0;j<2;++j) {
            CHECK(observed.events[i].choice_count==1 && other.events[j].choice_count==1);
            CHECK(observed.events[i].choices[0].block==3 && other.events[j].choices[0].block==3);
            bool same=observed.events[i].choices[0].predecessor==other.events[j].choices[0].predecessor;
            CHECK(same==(observed.events[i].value!=other.events[j].value));
        }
    } else if (kind==10) {
        uint32_t input=0,repeated=0;
        for (uint32_t i=0;i<observed.count;++i) {input+=observed.events[i].kind==XR_XIR_VIEW_LOAN_INPUT;repeated+=observed.events[i].repeated_phi;}
        CHECK(input==1 && repeated==1);
    }
    uint32_t calls=0;status=xir_compile_view_loan_origins(resolver,&f.request,loan_stop,&calls,&diagnostic);
    if (status==XR_XIR_BUDGET || status==XR_XIR_OUT_OF_MEMORY) goto done;CHECK(status==XR_XIR_BAD_VALUE && calls==1);
    status=xir_compile_view_loan_origins(resolver,&f.request,NULL,NULL,&diagnostic);
done:CHECK(!memcmp(before,&f,sizeof(f)));xir_compile_view_root_resolver_free(resolver);xr_xir_compile_construction_free(construction);return status;
}
static void loan_resources(uint32_t kind,bool normal) {
    XrXirStatus expected=kind==15?XR_XIR_BAD_STRUCTURE:kind==16 || kind==17?XR_XIR_BAD_TYPE:XR_XIR_OK;
    XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;size_t first=attempts;
    XrXirStatus status=loan_sequence(&c,kind);if (status!=expected) fprintf(stderr,"loan kind=%u status=%u expected=%u\n",kind,status,expected);
    CHECK(status==expected);size_t points=attempts-first;XrCompileResourceStats measured=stats(&c);CHECK(points && points<4096);
    CHECK(loan_sequence(&c,kind)==expected);owner_free(&c,baseline);
    if (!normal) for (size_t p=0;p<points;++p) {
        c=owner_new(caps());baseline=stats(&c).live_bytes;fail_at=attempts+p;injected=false;
        status=loan_sequence(&c,kind);fail_at=SIZE_MAX;
        if (status!=XR_XIR_OUT_OF_MEMORY || !injected) fprintf(stderr,"loan kind=%u FI=%zu/%zu status=%u\n",kind,p,points,status);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && injected);owner_free(&c,baseline);
    }
    if (!normal) for (uint32_t axis=0;axis<3;++axis) for (uint32_t delta=0;delta<3;++delta) {
        XrCompileResourceLimits limits=caps();uint64_t *bound=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        *bound=(axis==0?measured.allocated_bytes:axis==1?measured.peak_bytes:measured.work)+delta-1u;
        c=owner_new(limits);baseline=stats(&c).live_bytes;CHECK(loan_sequence(&c,kind)==(delta?expected:XR_XIR_BUDGET));owner_free(&c,baseline);
    }
    printf("Loan origin kind=%u: %zu allocation points owner2/physical0%s PASS\n",kind,points,normal?"":"/all actual failures/three axes exact+minus1+plus1");
}
int main(int argc,char **argv) {
    bool normal=argc==2 && !strcmp(argv[1],"--normal");CHECK(argc==1 || normal);setvbuf(stdout,NULL,_IONBF,0);
    for (uint32_t kind=0;kind<L_CASES;++kind) loan_resources(kind,normal);
    puts("Scope producer sites preserve PHI choices and do not grant ancestor or access permissions");return 0;
}
