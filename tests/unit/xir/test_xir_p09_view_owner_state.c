/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_view_owner_state.c - Shared copies and explicit borrowed END
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_view_types_internal.h"
#include "tuple_owner_observer.h"
#include "xir_view_root_fixture.h"
static void state_end(RootFixture *f,uint32_t value) {
    root_op(f,0,(XrXirInstruction){.op=XR_XIR_VIEW_END,.args={value}});
}
static void state_len(RootFixture *f,uint32_t value) {
    root_op(f,0,(XrXirInstruction){.op=XR_XIR_VIEW_LEN,.type=XR_XIR_I64,.args={value}});
}
static uint32_t state_view(RootFixture *f) {root_bounds(f,0);return root_view(f,0,1,false);}
static void state_finish(RootFixture *f) {
    for (uint32_t i=0;i<4;++i) {
        XrXirFunction *body=&f->functions[i];uint32_t count=body->instruction_count;
        if (!count || f->ops[i][count-1u].op!=XR_XIR_RETURN) {
            root_op(f,i,(XrXirInstruction){.op=XR_XIR_RETURN});
            if (body->block_count>1) {
                f->blocks[i][0].count=f->blocks[i][1].first;++f->blocks[i][body->block_count-1u].count;
            }
        }
        if (!body->parameter_count) body->parameters=NULL;
        if (!body->operand_count) body->operands=NULL;
    }
}
static void state_straight(RootFixture *f,uint32_t kind) {
    uint32_t value=state_view(f);
    if (kind==0 || kind==1 || kind==3 || kind==18) {
        if (kind==18) f->ops[0][2].type=ROOT_MUT;
        state_end(f,value);
        if (kind==1) state_end(f,value);
        else if (kind==3) root_op(f,0,(XrXirInstruction){.op=XR_XIR_COPY,.type=ROOT_SHARED,.args={value}});
        else state_len(f,value);
    } else if (kind==2) {
        uint32_t copy=root_op(f,0,(XrXirInstruction){.op=XR_XIR_COPY,.type=ROOT_SHARED,.args={value}});
        state_end(f,value);state_len(f,copy);state_end(f,copy);
    } else if (kind==4 || kind==5) {
        uint32_t some=root_op(f,0,(XrXirInstruction){.op=XR_XIR_NULLABLE_SOME,.type=ROOT_OPTIONAL,.args={value}});
        if (kind==5) {
            state_end(f,some);root_op(f,0,(XrXirInstruction){.op=XR_XIR_NULLABLE_IS_SOME,.type=XR_XIR_BOOL,.args={some}});
        } else {
            state_end(f,value);uint32_t copy=root_op(f,0,(XrXirInstruction){.op=XR_XIR_NULLABLE_UNWRAP,.type=ROOT_SHARED,.args={some}});
            state_end(f,some);state_len(f,copy);state_end(f,copy);
        }
    } else if (kind==6 || kind==7) {
        uint32_t some=root_op(f,0,(XrXirInstruction){.op=XR_XIR_NULLABLE_SOME,.type=ROOT_OPTIONAL,.args={value}});
        uint32_t start=root_group(f,0,(uint32_t[]){value,some},2);
        uint32_t tuple=root_op(f,0,(XrXirInstruction){.op=XR_XIR_TUPLE_NEW,.type=ROOT_TUPLE,.args={start,2}});
        state_end(f,tuple);
        if (kind==6) state_len(f,value);
        else root_op(f,0,(XrXirInstruction){.op=XR_XIR_TUPLE_FIELD,.type=ROOT_OPTIONAL,.args={tuple},.immediate=2});
    } else if (kind==19) {
        f->ops[0][2].type=ROOT_MUT;uint32_t child=root_view(f,0,value,true);state_end(f,child);state_len(f,value);state_end(f,value);
    } else CHECK(false);
}
static void state_branch(RootFixture *f,uint32_t kind) {
    uint32_t value=state_view(f);root_op(f,0,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={1,2}});
    uint32_t first=f->functions[0].instruction_count;
    if (kind==9 || kind==10 || kind==11) state_end(f,value);
    if (kind==11) root_op(f,0,(XrXirInstruction){.op=XR_XIR_RETURN});
    else root_op(f,0,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3}});
    uint32_t second=f->functions[0].instruction_count;
    if (kind==10) state_end(f,value);
    if (kind==11) {state_len(f,value);root_op(f,0,(XrXirInstruction){.op=XR_XIR_RETURN});}
    else root_op(f,0,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3}});
    uint32_t join=f->functions[0].instruction_count;
    if (kind==8 || kind==9) {
        uint32_t start=root_group(f,0,(uint32_t[]){1,value,2,value},4);
        uint32_t phi=root_op(f,0,(XrXirInstruction){.op=XR_XIR_PHI,.type=ROOT_SHARED,.args={start,4}});state_end(f,phi);
    }
    if (kind!=11) state_len(f,value);
    f->functions[0].block_count=kind==11?3u:4u;f->blocks[0][0]=(XrXirBlock){.count=first};
    f->blocks[0][1]=(XrXirBlock){.first=first,.count=second-first};f->blocks[0][2]=(XrXirBlock){.first=second,.count=join-second};
    if (kind!=11) f->blocks[0][3]=(XrXirBlock){.first=join,.count=f->functions[0].instruction_count-join};
}
static void state_loop(RootFixture *f,bool recreate) {
    root_bounds(f,0);uint32_t value=UINT32_MAX;
    if (!recreate) {value=root_view(f,0,1,false);state_end(f,value);}
    root_op(f,0,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}});uint32_t loop=f->functions[0].instruction_count;
    if (recreate) {value=root_view(f,0,1,false);state_end(f,value);}else state_len(f,value);
    root_op(f,0,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={1,2}});
    uint32_t exit=f->functions[0].instruction_count;f->functions[0].block_count=3;
    f->blocks[0][0]=(XrXirBlock){.count=loop};f->blocks[0][1]=(XrXirBlock){.first=loop,.count=exit-loop};
    f->blocks[0][2]=(XrXirBlock){.first=exit};
}
static void state_panic(RootFixture *f,bool copy) {
    uint32_t value=state_view(f);root_op(f,0,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}});
    uint32_t first=f->functions[0].instruction_count;
    uint32_t ended=copy?root_op(f,0,(XrXirInstruction){.op=XR_XIR_COPY,.type=ROOT_SHARED,.args={value}}):value;
    state_end(f,ended);root_op(f,0,(XrXirInstruction){.op=XR_XIR_RETURN});uint32_t handler=f->functions[0].instruction_count;
    root_op(f,0,(XrXirInstruction){.op=XR_XIR_PANIC_CATCH,.type=XR_XIR_PANIC_INFO});state_len(f,value);
    f->functions[0].block_count=3;f->blocks[0][0]=(XrXirBlock){.count=first};
    f->blocks[0][1]=(XrXirBlock){.first=first,.count=handler-first,.panic=2};f->blocks[0][2]=(XrXirBlock){.first=handler,.count=2};
}
static void state_call(RootFixture *f,uint32_t kind) {
    uint32_t value=state_view(f);if (kind==20) f->ops[0][2].type=ROOT_MUT;
    f->parameters[1][0]=kind==20?ROOT_MUT:ROOT_SHARED;
    if (kind!=16) {f->functions[1].parameter_count=2;f->parameters[1][1]=f->parameters[1][0];}
    else state_end(f,value);
    uint32_t start=root_group(f,0,(uint32_t[]){value,value},kind==16?1u:2u);
    root_op(f,0,(XrXirInstruction){.op=XR_XIR_CALL,.args={start,kind==16?1u:2u},.immediate=1});
    if (kind==17) state_len(f,value);
}
static uint32_t state_prepare(RootFixture *f,uint32_t kind) {
    root_fixture(f);uint32_t rejected=UINT32_MAX;
    if (kind<=7 || kind==18 || kind==19) state_straight(f,kind);
    else if (kind<=11) state_branch(f,kind);
    else if (kind<=13) state_loop(f,kind==12);
    else if (kind<=15) state_panic(f,kind==15);
    else if (kind<=20) state_call(f,kind);
    else if (kind==21 || kind==22) {
        f->functions[0].parameter_count=1;f->parameters[0][0]=ROOT_CELL;f->functions[0].result=ROOT_SHARED;
        root_bounds(f,0);uint32_t value=root_view(f,0,0,false);if (kind==21) state_end(f,value);
        root_op(f,0,(XrXirInstruction){.op=XR_XIR_RETURN,.args={value}});
    } else if (kind==23) {
        f->functions[0].parameter_count=1;f->parameters[0][0]=ROOT_SHARED;state_end(f,0);state_len(f,0);
    } else if (kind==24) {
        uint32_t none=root_op(f,0,(XrXirInstruction){.op=XR_XIR_NULLABLE_NONE,.type=ROOT_OPTIONAL});state_end(f,none);
        root_op(f,0,(XrXirInstruction){.op=XR_XIR_NULLABLE_IS_SOME,.type=XR_XIR_BOOL,.args={none}});
    } else CHECK(false);
    if (kind==0 || kind==1 || kind==3 || kind==18 || kind==16 || kind==21) rejected=4;
    else if (kind==5) rejected=5;else if (kind==7 || kind==9) rejected=6+(kind==9);
    else if (kind==10) rejected=8;else if (kind==13) rejected=5;else if (kind==14) rejected=7;
    else if (kind==23) rejected=1;else if (kind==24) rejected=2;
    else if (kind==20) rejected=3;
    state_finish(f);return rejected;
}
static XrXirStatus state_prove(const XrXirCompileContext *c,const RootFixture *f,XrXirDiagnostic *d) {
    return xir_compile_view_affine_verified(c,&f->module,d);
}
typedef struct StateDense {RootFixture fixture;XrXirType parameters[63];} StateDense;
static void state_dense(StateDense *dense) {
    RootFixture *f=&dense->fixture;root_fixture(f);
    for (uint32_t i=0;i<63;++i) dense->parameters[i]=ROOT_MUT;
    f->functions[0].parameters=dense->parameters;f->functions[0].parameter_count=63;
    f->parameters[1][0]=ROOT_MUT;f->functions[1].result=ROOT_MUT;
    root_op(f,1,(XrXirInstruction){.op=XR_XIR_RETURN,.args={0}});
    root_group(f,0,(uint32_t[]){0},1);
    root_op(f,0,(XrXirInstruction){.op=XR_XIR_INVOKE,.type=ROOT_MUT,.args={0,1},.targets={1,2},.immediate=1});
    root_op(f,0,(XrXirInstruction){.op=XR_XIR_INVOKE_RESULT,.type=ROOT_MUT});
    state_end(f,64);root_op(f,0,(XrXirInstruction){.op=XR_XIR_RETURN});
    root_op(f,0,(XrXirInstruction){.op=XR_XIR_INVOKE_ERROR,.type=XR_XIR_ERROR});
    f->functions[0].block_count=3;f->blocks[0][0]=(XrXirBlock){.count=1};
    f->blocks[0][1]=(XrXirBlock){.first=1,.count=3};f->blocks[0][2]=(XrXirBlock){.first=4,.count=1};state_finish(f);
}
static void state_inventory(void) {
    StateDense dense;state_dense(&dense);XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;
    CHECK(xr_xir_compile_types_structure_verify(&c,&dense.fixture.types)==XR_XIR_OK);
    XrXirArtifact *checked=NULL;
    CHECK(xir_fixture_check(&c,&dense.fixture.module,&checked,NULL)==XR_XIR_BAD_TYPE && !checked);
    c.limits.frame_bytes=347;XrXirStatus status=state_prove(&c,&dense.fixture,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"owner inventory status=%u actual64 function scratch347\n",status);
    CHECK(status==XR_XIR_OK);c.limits.frame_bytes=346;
    CHECK(state_prove(&c,&dense.fixture,NULL)==XR_XIR_BUDGET);owner_free(&c,baseline);
    puts("borrowed owners: actual64 slots, INVOKE contract is not an owner; scratch347 exact and346 reject PASS");
}
static void state_normal(void) {
    XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;
    for (uint32_t kind=0;kind<25;++kind) {
        RootFixture f;uint32_t rejected=state_prepare(&f,kind);XrXirStatus expected=rejected==UINT32_MAX?XR_XIR_OK:XR_XIR_BAD_VALUE;
        CHECK(xr_xir_compile_types_structure_verify(&c,&f.types)==XR_XIR_OK);XrXirDiagnostic d={0};
        XrXirStatus status=state_prove(&c,&f,&d);
        if (status!=expected || (status==XR_XIR_BAD_VALUE && d.instruction!=rejected))
            fprintf(stderr,"owner state case=%u status=%u at %u/%u/%u expected=%u/%u\n",kind,status,d.function,d.block,d.instruction,expected,rejected);
        CHECK(status==expected && (status!=XR_XIR_BAD_VALUE || (d.function==0 && d.instruction==rejected)));
        CHECK(stats(&c).live_bytes==baseline);XrXirArtifact *checked=NULL;
        status=xir_fixture_check(&c,&f.module,&checked,&d);expected=expected==XR_XIR_OK?XR_XIR_BAD_TYPE:expected;
        if (status!=expected) fprintf(stderr,"owner common case=%u status=%u at %u/%u/%u\n",kind,status,d.function,d.block,d.instruction);
        CHECK(status==expected && !checked && stats(&c).live_bytes==baseline);
    }
    owner_free(&c,baseline);puts("borrowed owners: Shared independent copies/wrappers/PHIs/calls and END/branch/loop/panic/return/None25 PASS");
}
static void state_resources(uint32_t kind) {
    RootFixture storage,*f=&storage;StateDense dense;uint32_t rejected=UINT32_MAX;
    if (kind==25) {state_dense(&dense);f=&dense.fixture;}else rejected=state_prepare(f,kind);
    XrXirStatus expected=rejected==UINT32_MAX?XR_XIR_OK:XR_XIR_BAD_VALUE;
    XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;size_t start=attempts;
    CHECK(state_prove(&c,f,NULL)==expected);size_t count=attempts-start;CHECK(count>=5 && count<32);
    XrCompileResourceStats measured=stats(&c);CHECK(state_prove(&c,f,NULL)==expected);owner_free(&c,baseline);
    for (size_t point=0;point<count;++point) {
        c=owner_new(caps());baseline=stats(&c).live_bytes;injected=false;fail_at=attempts+point;
        CHECK(state_prove(&c,f,NULL)==XR_XIR_OUT_OF_MEMORY && injected);fail_at=SIZE_MAX;owner_free(&c,baseline);
    }
    for (uint32_t axis=0;axis<3;++axis) for (uint32_t adjust=0;adjust<3;++adjust) {
        XrCompileResourceLimits limits=caps();uint64_t *limit=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        *limit=(axis==0?measured.allocated_bytes:axis==1?measured.peak_bytes:measured.work)+adjust-1u;
        c=owner_new(limits);baseline=stats(&c).live_bytes;
        CHECK(state_prove(&c,f,NULL)==(adjust?expected:XR_XIR_BUDGET));owner_free(&c,baseline);
    }
    c=owner_new(caps());baseline=stats(&c).live_bytes;c.limits.frame_bytes=1;
    CHECK(state_prove(&c,f,NULL)==XR_XIR_BUDGET);owner_free(&c,baseline);
    printf("borrowed owner case=%u: %zu complete allocation failures/same owner2/three axes exact-minus1-plus1/frame/physical0 PASS\n",kind,count);
}
int main(int argc,char **argv) {
    bool normal=argc==2 && !strcmp(argv[1],"--normal"),inventory=argc==2 && !strcmp(argv[1],"--inventory");
    CHECK(argc==1 || normal || inventory);if (inventory) {state_inventory();return 0;}
    state_normal();state_inventory();
    if (!normal) for (uint32_t kind=0;kind<26;++kind) state_resources(kind);
    puts("Owner availability after physical END is not root/alias permission or Source NLL completion PASS");return 0;
}
