/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_view_owner_path.c - Required edges and conditional owner state
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_view_prepare_internal.h"
#include "xir/xxir_view_owner_path_internal.h"
#include "xir/xxir_view_types_internal.h"
#include "tuple_owner_observer.h"

enum { P_SHARED=256,P_MUT,P_OPTIONAL,P_CASES=37 };
typedef struct PathFixture {
    XrXirTypeNode nodes[3];XrXirTypes types;XrXirType parameters[4];
    XrXirInstruction ops[128],empty;XrXirBlock blocks[8],single;uint32_t operands[128];
    XrXirFunction functions[2];XrXirSourceModule source;XrXirFunctionIdentity identities[2];
    XrXirDeclarations declarations;XrXirModule module;XrXirViewLoanChoice left[4],right[4];
    XirViewOwnerPathRequest request;XrXirStatus expected;bool possible;
} PathFixture;
static uint32_t path_op(PathFixture *f,XrXirInstruction op) {
    CHECK(f->functions[0].instruction_count<128);uint32_t i=f->functions[0].instruction_count++;
    f->ops[i]=op;return 4u+i;
}
static uint32_t path_group(PathFixture *f,const uint32_t *values,uint32_t count) {
    uint32_t at=f->functions[0].operand_count;CHECK(at+count<=128);
    if (count) memcpy(f->operands+at,values,(size_t)count*sizeof(*values));f->functions[0].operand_count+=count;return at;
}
static void path_end(PathFixture *f,uint32_t value) {path_op(f,(XrXirInstruction){.op=XR_XIR_VIEW_END,.args={value}});}
static uint32_t path_phi(PathFixture *f,const uint32_t *values,uint32_t count) {
    uint32_t at=path_group(f,values,count);return path_op(f,(XrXirInstruction){.op=XR_XIR_PHI,.type=(XrXirType)P_MUT,.args={at,count}});
}
static void path_base(PathFixture *f) {
    memset(f,0,sizeof(*f));f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_I64};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_MUT_SLICE,.element=XR_XIR_I64};
    f->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)P_MUT};
    f->types=(XrXirTypes){f->nodes,3,NULL,NULL};f->parameters[0]=XR_XIR_BOOL;f->parameters[1]=(XrXirType)P_SHARED;
    f->parameters[2]=f->parameters[3]=(XrXirType)P_MUT;
    f->functions[0]=(XrXirFunction){.name="path",.name_length=4,.parameters=f->parameters,.parameter_count=4,
        .blocks=f->blocks,.block_count=1,.instructions=f->ops};
    f->empty=(XrXirInstruction){.op=XR_XIR_RETURN};f->single=(XrXirBlock){.count=1};
    f->functions[1]=(XrXirFunction){.name="init",.name_length=4,.blocks=&f->single,.block_count=1,.instructions=&f->empty,.instruction_count=1};
    f->source=(XrXirSourceModule){.name="root",.name_length=4,.initializer=1};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,
        .root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=2,
        .declarations=&f->declarations,.types=&f->types,.linkage_kind=XR_XIR_LIBRARY};
    f->request=(XirViewOwnerPathRequest){.owner=1,.left=f->left,.right=f->right};f->possible=true;
    path_op(f,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64});
}
static void path_linear(PathFixture *f,uint32_t kind) {
    if (kind==1) {path_end(f,1);f->possible=false;}
    if (kind==2 || kind==3 || kind==6) {
        uint32_t value=path_op(f,(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)P_SHARED,.args={1}});
        if (kind==6) {f->request.instruction=1;f->request.owner=value;f->possible=false;}
        else {path_end(f,1);f->request.owner=kind==2?value:1;f->possible=kind==2;}
    } else if (kind==4 || kind==5) {
        uint32_t value=path_op(f,(XrXirInstruction){.op=XR_XIR_NULLABLE_SOME,.type=(XrXirType)P_OPTIONAL,.args={2}});
        f->request.owner=kind==4?2:value;f->possible=kind==5;
    } else if (kind==7) for (uint32_t i=0;i<72;++i)
        f->request.owner=path_op(f,(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)P_SHARED,.args={1}});
    if (kind!=6) f->request.instruction=f->functions[0].instruction_count;
    path_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});f->blocks[0].count=f->functions[0].instruction_count;
}
static void path_diamond(PathFixture *f,uint32_t kind) {
    path_op(f,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={1,2}});uint32_t first=f->functions[0].instruction_count;
    path_end(f,1);path_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3}});uint32_t second=f->functions[0].instruction_count;
    path_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3}});uint32_t join=f->functions[0].instruction_count;
    uint32_t phi=path_phi(f,(uint32_t[]){1,2,2,3},4);f->request.instruction=f->functions[0].instruction_count;
    path_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});f->functions[0].block_count=4;
    f->blocks[0]=(XrXirBlock){.count=first};f->blocks[1]=(XrXirBlock){.first=first,.count=second-first};
    f->blocks[2]=(XrXirBlock){.first=second,.count=join-second};f->blocks[3]=(XrXirBlock){.first=join,.count=2};
    f->left[0]=(XrXirViewLoanChoice){3,kind==9 || kind==12?2:1};f->request.left_count=kind==10?0:1;
    f->possible=kind==9 || kind==10 || kind==12 || kind==17;
    if (kind==11 || kind==12) f->request.owner=2;
    if (kind==13 || kind==15) {f->request.right_count=1;f->right[0]=(XrXirViewLoanChoice){3,kind==13?2:1};}
    if (kind==14) {f->left[0].predecessor=7;f->expected=XR_XIR_BAD_STRUCTURE;}
    if (kind==16 || kind==17) {f->request.owner=phi;if (kind==16) f->request.instruction=join;}
}
static void path_mandatory(PathFixture *f,uint32_t kind) {
    path_op(f,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={1,2}});uint32_t first=f->functions[0].instruction_count;
    path_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3}});path_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={4}});
    uint32_t chosen=f->functions[0].instruction_count;path_phi(f,(uint32_t[]){1,2},2);
    path_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});uint32_t bypass=f->functions[0].instruction_count;
    path_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});f->functions[0].block_count=5;
    f->blocks[0]=(XrXirBlock){.count=first};f->blocks[1]=(XrXirBlock){.first=first,.count=1};
    f->blocks[2]=(XrXirBlock){.first=first+1u,.count=1};f->blocks[3]=(XrXirBlock){.first=chosen,.count=2};
    f->blocks[4]=(XrXirBlock){.first=bypass,.count=1};f->left[0]=(XrXirViewLoanChoice){3,1};f->request.left_count=1;
    f->request.instruction=kind==18?bypass:chosen+1u;f->possible=kind==19;
}
static void path_two_joins(PathFixture *f,uint32_t kind) {
    path_op(f,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={1,2}});uint32_t first=f->functions[0].instruction_count;
    path_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3}});path_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3}});
    uint32_t join=f->functions[0].instruction_count,phi=path_phi(f,(uint32_t[]){1,2,2,3},4);
    path_op(f,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={4,5}});uint32_t left=f->functions[0].instruction_count;
    path_end(f,1);path_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={6}});uint32_t right=f->functions[0].instruction_count;
    path_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={6}});uint32_t final=f->functions[0].instruction_count;
    path_phi(f,(uint32_t[]){4,phi,5,phi},4);f->request.instruction=f->functions[0].instruction_count;
    path_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});f->functions[0].block_count=7;
    f->blocks[0]=(XrXirBlock){.count=first};f->blocks[1]=(XrXirBlock){.first=first,.count=1};
    f->blocks[2]=(XrXirBlock){.first=first+1u,.count=1};f->blocks[3]=(XrXirBlock){.first=join,.count=2};
    f->blocks[4]=(XrXirBlock){.first=left,.count=right-left};f->blocks[5]=(XrXirBlock){.first=right,.count=1};
    f->blocks[6]=(XrXirBlock){.first=final,.count=2};f->possible=kind==21;
    if (kind==20) {f->left[0]=(XrXirViewLoanChoice){6,4};f->left[1]=(XrXirViewLoanChoice){3,1};f->request.left_count=2;}
    else {f->left[0]=(XrXirViewLoanChoice){3,2};f->right[0]=(XrXirViewLoanChoice){kind==21?6:3,kind==21?5:1};f->request.left_count=f->request.right_count=1;}
}
static void path_loop(PathFixture *f,uint32_t kind) {
    path_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}});uint32_t first=f->functions[0].instruction_count;
    if (kind<25) {
        uint32_t phi=4u+first;path_phi(f,(uint32_t[]){0,2,1,phi},4);
        f->left[0]=(XrXirViewLoanChoice){1,0};f->request.left_count=1;f->expected=XR_XIR_UNRESOLVED;
        if (kind==24) {f->right[0]=(XrXirViewLoanChoice){1,1};f->request.right_count=1;}
    } else {
        f->request.instruction=f->functions[0].instruction_count;path_op(f,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64});
        f->request.owner=path_op(f,(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)P_SHARED,.args={1}});
        if (kind==26) path_end(f,f->request.owner);f->possible=kind==25;
    }
    path_op(f,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0},.targets={1,2}});uint32_t end=f->functions[0].instruction_count;
    if (kind<25) f->request.instruction=end;path_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});f->functions[0].block_count=3;
    f->blocks[0]=(XrXirBlock){.count=first};f->blocks[1]=(XrXirBlock){.first=first,.count=end-first};
    f->blocks[2]=(XrXirBlock){.first=end,.count=1};
}
static void path_panic(PathFixture *f,uint32_t kind) {
    path_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}});uint32_t first=f->functions[0].instruction_count;
    uint32_t local=path_op(f,(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)P_SHARED,.args={1}});
    path_end(f,local);path_end(f,1);path_op(f,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3}});uint32_t handler=f->functions[0].instruction_count;
    path_op(f,(XrXirInstruction){.op=XR_XIR_PANIC_CATCH,.type=XR_XIR_PANIC_INFO});
    f->request.instruction=handler+1u;f->request.owner=kind==27?local:1;
    path_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});path_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});f->functions[0].block_count=4;
    f->blocks[0]=(XrXirBlock){.count=first};f->blocks[1]=(XrXirBlock){.first=first,.count=handler-first,.panic=2};
    f->blocks[2]=(XrXirBlock){.first=handler,.count=2};f->blocks[3]=(XrXirBlock){.first=handler+2u,.count=1};
    if (kind==29) {f->request.left_count=1;f->left[0]=(XrXirViewLoanChoice){2,1};f->expected=XR_XIR_BAD_STRUCTURE;}
}
static void path_case(PathFixture *f,uint32_t kind) {
    path_base(f);
    if (kind<8 || kind>=30) path_linear(f,kind);
    else if (kind<=17) path_diamond(f,kind);else if (kind<=19) path_mandatory(f,kind);
    else if (kind<=22) path_two_joins(f,kind);else if (kind<=26) path_loop(f,kind);else path_panic(f,kind);
    if (kind>=30) {
        f->expected=kind==31?XR_XIR_BAD_TYPE:XR_XIR_BAD_STRUCTURE;
        if (kind==30) f->request.owner=UINT32_MAX;else if (kind==31) f->request.owner=4;
        else if (kind==32) f->request.instruction=UINT32_MAX;
        else if (kind==33) f->request.left_count=2;
        else {f->request.left_count=1;if (kind==34) f->request.left=NULL;
            else f->left[0]=(XrXirViewLoanChoice){kind==35?0:1,0};}
    }
    if (f->functions[0].operand_count) f->functions[0].operands=f->operands;
}
static XrXirStatus path_sequence(const XrXirCompileContext *context,uint32_t kind) {
    PathFixture f;path_case(&f,kind);unsigned char before[sizeof(f)];memcpy(before,&f,sizeof(f));
    XrXirConstruction *construction=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_construction(context,&f.types,&construction);if (status!=XR_XIR_OK) goto done;
    status=xir_compile_view_prepare_ordinary(context,&f.module,construction,&diagnostic);
    if (status==XR_XIR_OK) status=xir_compile_view_return_origins_verified(context,&f.module,&diagnostic);
    if (status==XR_XIR_OK) status=xir_compile_view_affine_verified(context,&f.module,&diagnostic);
    if (status==XR_XIR_BUDGET || status==XR_XIR_OUT_OF_MEMORY) goto done;
    if (status!=XR_XIR_OK) fprintf(stderr,"owner path prefix kind=%u status=%u at %u/%u/%u\n",kind,status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_OK);bool possible=!f.possible;
    if (kind<30) {
        XrXirCompileContext tiny=*context;tiny.limits.frame_bytes=1;size_t count=attempts;
        CHECK(xir_compile_view_owner_path_possible(&tiny,&f.module,&f.request,&possible,NULL)==XR_XIR_BUDGET && attempts==count && possible!=f.possible);
    }
    status=xir_compile_view_owner_path_possible(context,&f.module,&f.request,&possible,&diagnostic);
    if (status!=XR_XIR_BUDGET && status!=XR_XIR_OUT_OF_MEMORY) {
        if (status!=f.expected || (status==XR_XIR_OK && possible!=f.possible))
            fprintf(stderr,"owner path kind=%u status=%u expected=%u possible=%u expected=%u\n",kind,status,f.expected,possible,f.possible);
        CHECK(status==f.expected);if (status==XR_XIR_OK) CHECK(possible==f.possible);
    }
    if (status!=XR_XIR_OK) CHECK(possible!=f.possible);
done:CHECK(!memcmp(before,&f,sizeof(f)));xr_xir_compile_construction_free(construction);return status;
}
static void path_resources(uint32_t kind,bool normal) {
    PathFixture fixture;path_case(&fixture,kind);XrXirStatus expected=fixture.expected;
    XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;size_t first=attempts;
    CHECK(path_sequence(&c,kind)==expected);size_t points=attempts-first;XrCompileResourceStats measured=stats(&c);
    CHECK(points && points<4096);CHECK(path_sequence(&c,kind)==expected);owner_free(&c,baseline);
    if (!normal) for (size_t p=0;p<points;++p) {
        c=owner_new(caps());baseline=stats(&c).live_bytes;fail_at=attempts+p;injected=false;
        XrXirStatus status=path_sequence(&c,kind);fail_at=SIZE_MAX;
        if (status!=XR_XIR_OUT_OF_MEMORY || !injected) fprintf(stderr,"owner path kind=%u FI=%zu/%zu status=%u\n",kind,p,points,status);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && injected);owner_free(&c,baseline);
    }
    if (!normal) for (uint32_t axis=0;axis<3;++axis) for (uint32_t delta=0;delta<3;++delta) {
        XrCompileResourceLimits limits=caps();uint64_t *bound=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        *bound=(axis==0?measured.allocated_bytes:axis==1?measured.peak_bytes:measured.work)+delta-1u;
        c=owner_new(limits);baseline=stats(&c).live_bytes;CHECK(path_sequence(&c,kind)==(delta?expected:XR_XIR_BUDGET));owner_free(&c,baseline);
    }
    printf("Owner path kind=%u: %zu allocation points owner2/physical0%s PASS\n",kind,points,normal?"":"/all actual failures/three axes exact+minus1+plus1");
}
int main(int argc,char **argv) {
    bool normal=argc==2 && !strcmp(argv[1],"--normal");CHECK(argc==1 || normal);setvbuf(stdout,NULL,_IONBF,0);
    for (uint32_t kind=0;kind<P_CASES;++kind) path_resources(kind,normal);
    puts("Required acyclic edges preserve conditional owners; cyclic choices issue no absence proof");return 0;
}
