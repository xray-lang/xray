/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_view_normalize.c - Real ordinary CFGs and conditional owner cleanup
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_view_prepare_internal.h"
#include "xir/xxir_view_normalize_internal.h"
#include "xir/xxir_view_types_internal.h"
#include "tuple_owner_observer.h"
enum {N_SHARED=256,N_MUT,N_OPTIONAL,N_KINDS=12};
typedef struct NormalizeFixture {
    XrXirTypeNode nodes[3];XrXirTypes types;XrXirType parameters[4][3];
    XrXirInstruction ops[4][16];XrXirBlock blocks[4][4];uint32_t operands[4][32];
    XrXirFunction functions[4];XrXirSourceModule source;XrXirFunctionIdentity identities[4];
    XrXirDeclarations declarations;XrXirModule module;
} NormalizeFixture;
static uint32_t norm_op(NormalizeFixture *f,uint32_t function,XrXirInstruction op) {
    XrXirFunction *body=f->functions+function;CHECK(body->instruction_count<16);
    uint32_t i=body->instruction_count++;f->ops[function][i]=op;return body->parameter_count+i;
}
static uint32_t norm_group(NormalizeFixture *f,uint32_t function,const uint32_t *values,uint32_t count) {
    XrXirFunction *body=f->functions+function;uint32_t at=body->operand_count;CHECK(at+count<=32);
    memcpy(f->operands[function]+at,values,count*sizeof(*values));body->operand_count+=count;return at;
}
static void norm_fixture(NormalizeFixture *f) {
    memset(f,0,sizeof(*f));f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_I64};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_MUT_SLICE,.element=XR_XIR_I64};
    f->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)N_MUT};
    f->types=(XrXirTypes){f->nodes,3,NULL,NULL};
    for (uint32_t i=0;i<4;++i) f->functions[i]=(XrXirFunction){
        "body",4,NULL,0,XR_XIR_UNIT,f->blocks[i],1,f->ops[i],0,NULL,0,XR_XIR_NEVER};
    norm_op(f,0,(XrXirInstruction){.op=XR_XIR_RETURN});
    uint32_t value=norm_op(f,1,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64});
    norm_op(f,1,(XrXirInstruction){.op=XR_XIR_RETURN,.args={value}});f->functions[1].result=XR_XIR_I64;
    norm_op(f,3,(XrXirInstruction){.op=XR_XIR_RETURN});
    f->source=(XrXirSourceModule){"root",4,NULL,0,0,0,XR_XIR_MODULE_FRESH_STORAGE};
    f->declarations=(XrXirDeclarations){&f->source,1,f->identities,NULL,0,NULL,0,0,1,NULL};
    f->module=(XrXirModule){XR_XIR_BUILT,f->functions,4,&f->declarations,NULL,&f->types,NULL,XR_XIR_PROGRAM,NULL};
    f->parameters[2][0]=(XrXirType)N_SHARED;f->functions[2].parameters=f->parameters[2];f->functions[2].parameter_count=1;
}
static void norm_branch(NormalizeFixture *f,uint32_t kind) {
    XrXirFunction *body=f->functions+2;body->parameter_count=2;f->parameters[2][1]=XR_XIR_BOOL;
    norm_op(f,2,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={1},.targets={1,2}});
    if (kind==3) {
        norm_op(f,2,(XrXirInstruction){.op=XR_XIR_VIEW_LEN,.type=XR_XIR_I64});
        norm_op(f,2,(XrXirInstruction){.op=XR_XIR_RETURN});
        norm_op(f,2,(XrXirInstruction){.op=XR_XIR_RETURN});
        f->blocks[2][0]=(XrXirBlock){0,1,0,0};f->blocks[2][1]=(XrXirBlock){1,2,0,0};
        f->blocks[2][2]=(XrXirBlock){3,1,0,0};body->block_count=3;return;
    }
    uint32_t left=norm_op(f,2,(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)N_SHARED});
    norm_op(f,2,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3}});
    uint32_t right=norm_op(f,2,(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)N_SHARED});
    norm_op(f,2,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3}});
    uint32_t at=norm_group(f,2,(uint32_t[]){1,left,2,right},4);
    uint32_t phi=norm_op(f,2,(XrXirInstruction){.op=XR_XIR_PHI,.type=(XrXirType)N_SHARED,.args={at,4}});
    norm_op(f,2,(XrXirInstruction){.op=XR_XIR_VIEW_LEN,.type=XR_XIR_I64,.args={phi}});
    norm_op(f,2,(XrXirInstruction){.op=XR_XIR_RETURN});
    body->block_count=4;f->blocks[2][0]=(XrXirBlock){0,1,0,0};
    f->blocks[2][1]=(XrXirBlock){1,2,0,0};f->blocks[2][2]=(XrXirBlock){3,2,0,0};f->blocks[2][3]=(XrXirBlock){5,3,0,0};
}
static void norm_control(NormalizeFixture *f,uint32_t kind) {
    XrXirFunction *body=f->functions+2;
    if (kind==6) {
        body->parameter_count=2;f->parameters[2][1]=XR_XIR_BOOL;
        norm_op(f,2,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}});
        uint32_t at=norm_group(f,2,(uint32_t[]){0,0,1,3},4);
        norm_op(f,2,(XrXirInstruction){.op=XR_XIR_PHI,.type=(XrXirType)N_SHARED,.args={at,4}});
        norm_op(f,2,(XrXirInstruction){.op=XR_XIR_BRANCH,.args={1},.targets={1,2}});
        norm_op(f,2,(XrXirInstruction){.op=XR_XIR_VIEW_LEN,.type=XR_XIR_I64,.args={3}});
        norm_op(f,2,(XrXirInstruction){.op=XR_XIR_RETURN});
        f->blocks[2][0]=(XrXirBlock){0,1,0,0};f->blocks[2][1]=(XrXirBlock){1,2,0,0};
        f->blocks[2][2]=(XrXirBlock){3,2,0,0};body->block_count=3;
    } else if (kind==7) {
        norm_op(f,2,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}});
        uint32_t copy=norm_op(f,2,(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)N_SHARED});
        norm_op(f,2,(XrXirInstruction){.op=XR_XIR_VIEW_LEN,.type=XR_XIR_I64,.args={copy}});
        norm_op(f,2,(XrXirInstruction){.op=XR_XIR_RETURN});
        norm_op(f,2,(XrXirInstruction){.op=XR_XIR_PANIC_CATCH,.type=XR_XIR_PANIC_INFO});
        norm_op(f,2,(XrXirInstruction){.op=XR_XIR_RETURN});
        f->blocks[2][0]=(XrXirBlock){0,1,0,0};f->blocks[2][1]=(XrXirBlock){1,3,2,0};
        f->blocks[2][2]=(XrXirBlock){4,2,0,0};body->block_count=3;
    } else {
        f->identities[3].cleanup_owner=3;
        body->parameter_count=2;f->parameters[2][1]=(XrXirType)N_SHARED;
        norm_op(f,2,(XrXirInstruction){.op=XR_XIR_CLEANUP_REGISTER,.targets={1},.immediate=3});
        norm_op(f,2,(XrXirInstruction){.op=XR_XIR_VIEW_LEN,.type=XR_XIR_I64});
        norm_op(f,2,(XrXirInstruction){.op=XR_XIR_CLEANUP_LEAVE,.targets={2}});
        norm_op(f,2,(XrXirInstruction){.op=XR_XIR_RETURN});
        f->blocks[2][0]=(XrXirBlock){0,1,0,0};f->blocks[2][1]=(XrXirBlock){1,2,0,1};
        f->blocks[2][2]=(XrXirBlock){3,1,0,0};body->block_count=3;
    }
}
static void norm_case(NormalizeFixture *f,uint32_t kind) {
    norm_fixture(f);XrXirFunction *body=f->functions+2;
    if (kind==3 || kind==5) norm_branch(f,kind);
    else if (kind==6 || kind==7 || kind==10) norm_control(f,kind);
    else if (kind==8 || kind==9) {
        f->parameters[3][0]=(XrXirType)N_SHARED;f->functions[3].parameter_count=1;
        f->functions[3].parameters=f->parameters[3];f->functions[3].instruction_count=0;
        uint32_t value=norm_op(f,3,(XrXirInstruction){.op=XR_XIR_VIEW_LEN,.type=XR_XIR_I64});
        norm_op(f,3,(XrXirInstruction){.op=XR_XIR_RETURN,.args={value}});f->functions[3].result=XR_XIR_I64;
        if (kind==9) {body->parameter_count=2;f->parameters[2][1]=(XrXirType)N_SHARED;}
        uint32_t at=norm_group(f,2,(uint32_t[]){0},1);
        norm_op(f,2,(XrXirInstruction){.op=kind==8?XR_XIR_CALL:XR_XIR_INVOKE,.type=XR_XIR_I64,
            .args={at,1},.targets={kind==8?0u:1u,kind==8?0u:2u},.immediate=3});
        if (kind==9) {
            norm_op(f,2,(XrXirInstruction){.op=XR_XIR_INVOKE_RESULT,.type=XR_XIR_I64});
            norm_op(f,2,(XrXirInstruction){.op=XR_XIR_RETURN});
            norm_op(f,2,(XrXirInstruction){.op=XR_XIR_INVOKE_ERROR,.type=XR_XIR_ERROR});
            norm_op(f,2,(XrXirInstruction){.op=XR_XIR_RETURN});
            f->blocks[2][0]=(XrXirBlock){0,1,0,0};f->blocks[2][1]=(XrXirBlock){1,2,0,0};
            f->blocks[2][2]=(XrXirBlock){3,2,0,0};body->block_count=3;
        } else norm_op(f,2,(XrXirInstruction){.op=XR_XIR_RETURN});
    } else {
        if (kind==1 || kind==2) f->parameters[2][0]=(XrXirType)N_MUT;
        if (kind==11) {body->parameter_count=0;body->parameters=NULL;}
        if (kind==0) norm_op(f,2,(XrXirInstruction){.op=XR_XIR_VIEW_LEN,.type=XR_XIR_I64});
        if (kind==2) {
            uint32_t some=norm_op(f,2,(XrXirInstruction){.op=XR_XIR_NULLABLE_SOME,.type=(XrXirType)N_OPTIONAL});
            norm_op(f,2,(XrXirInstruction){.op=XR_XIR_NULLABLE_IS_SOME,.type=XR_XIR_BOOL,.args={some}});
        }
        if (kind==4) {
            uint32_t copy=norm_op(f,2,(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)N_SHARED});
            norm_op(f,2,(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}});
            uint32_t at=norm_group(f,2,(uint32_t[]){0,copy},2);
            uint32_t phi=norm_op(f,2,(XrXirInstruction){.op=XR_XIR_PHI,.type=(XrXirType)N_SHARED,.args={at,2}});
            norm_op(f,2,(XrXirInstruction){.op=XR_XIR_VIEW_LEN,.type=XR_XIR_I64,.args={phi}});
            body->block_count=2;f->blocks[2][0]=(XrXirBlock){0,2,0,0};f->blocks[2][1]=(XrXirBlock){2,3,0,0};
        }
        norm_op(f,2,(XrXirInstruction){.op=XR_XIR_RETURN});
    }
    for (uint32_t i=0;i<4;++i) {
        XrXirFunction *fn=f->functions+i;
        if (fn->block_count==1) f->blocks[i][0].count=fn->instruction_count;
        if (fn->operand_count) fn->operands=f->operands[i];
    }
}
static XrXirStatus norm_rejections(XrXirCompileContext *context,XirViewNormalization *n,
    XrXirConstruction *construction) {
    const XrXirFunction *function=n->bodies+2;uint32_t first=function->blocks[3].first;
    CHECK(function->instructions[first].op==XR_XIR_PHI);
    const XrXirInstruction *cleanup=function->instructions+first+1u;
    CHECK(cleanup->op==XR_XIR_VIEW_CLEANUP && cleanup->args[1]>=2);
    uint32_t *slots=n->functions[2].operands+cleanup->args[0];
    uint32_t last=cleanup->args[1]-1u,saved=slots[last];XrXirStatus status=XR_XIR_OK;
    for (uint32_t kind=0;kind<4;++kind) {
        slots[last]=kind==0?function->parameter_count+first:kind==1?UINT32_MAX:
            kind==2?slots[last-1u]:function->parameter_count+first+2u;
        XrXirDiagnostic diagnostic={0};
        status=xir_compile_view_prepare_ordinary(context,&n->module,construction,&diagnostic);
        if (status==XR_XIR_OK && !kind)
            status=xir_compile_view_affine_verified(context,&n->module,&diagnostic);
        slots[last]=saved;
        if (status==XR_XIR_BUDGET || status==XR_XIR_OUT_OF_MEMORY) return status;
        CHECK(status==(kind==0?XR_XIR_BAD_VALUE:kind==3?XR_XIR_BAD_TYPE:XR_XIR_BAD_STRUCTURE));
    }
    return XR_XIR_OK;
}
static XrXirStatus norm_sequence(XrXirCompileContext *context,uint32_t kind) {
    NormalizeFixture f;norm_case(&f,kind);unsigned char before[sizeof(f)];memcpy(before,&f,sizeof(f));
    XrXirConstruction *construction=NULL;XirViewNormalization *n=NULL;
    XrXirStatus status=xir_fixture_construction(context,&f.types,&construction);XrXirDiagnostic diagnostic={0};
    if (status!=XR_XIR_OK) goto done;
    status=xir_compile_view_prepare_ordinary(context,&f.module,construction,&diagnostic);
    if (status!=XR_XIR_OK && status!=XR_XIR_BUDGET && status!=XR_XIR_OUT_OF_MEMORY)
        fprintf(stderr,"original kind=%u status=%u f=%u b=%u i=%u\n",kind,status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    if (status==XR_XIR_BUDGET || status==XR_XIR_OUT_OF_MEMORY) goto done;CHECK(status==XR_XIR_OK);
    status=xir_compile_view_affine_verified(context,&f.module,&diagnostic);
    if (status==XR_XIR_BUDGET || status==XR_XIR_OUT_OF_MEMORY) goto done;CHECK(status==XR_XIR_OK);
    status=xir_compile_view_normalize_verified(context,&f.module,&n,&diagnostic);
    if (status!=XR_XIR_OK) goto done;CHECK(n && !memcmp(before,&f,sizeof(f)));
    status=xir_compile_view_prepare_ordinary(context,&n->module,construction,&diagnostic);
    if (status!=XR_XIR_OK && status!=XR_XIR_BUDGET && status!=XR_XIR_OUT_OF_MEMORY)
        fprintf(stderr,"normalized kind=%u status=%u f=%u b=%u i=%u\n",kind,status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    if (status==XR_XIR_BUDGET || status==XR_XIR_OUT_OF_MEMORY) goto done;CHECK(status==XR_XIR_OK);
    status=xir_compile_view_affine_verified(context,&n->module,&diagnostic);
    if (status!=XR_XIR_OK && status!=XR_XIR_BUDGET && status!=XR_XIR_OUT_OF_MEMORY)
        fprintf(stderr,"normalized owner kind=%u status=%u f=%u b=%u i=%u\n",kind,status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    if (status==XR_XIR_BUDGET || status==XR_XIR_OUT_OF_MEMORY) goto done;CHECK(status==XR_XIR_OK);
    CHECK((n->functions[2].instructions!=NULL)==(kind!=11));
    if (kind==5) status=norm_rejections(context,n,construction);
done:CHECK(!memcmp(before,&f,sizeof(f)));xir_compile_view_normalization_free(n);xr_xir_compile_construction_free(construction);return status;
}
static void norm_resources(uint32_t kind,bool normal) {
    XrXirCompileContext context=owner_new(caps());uint64_t baseline=stats(&context).live_bytes;size_t start=attempts;
    CHECK(norm_sequence(&context,kind)==XR_XIR_OK);size_t points=attempts-start;XrCompileResourceStats measured=stats(&context);
    CHECK(norm_sequence(&context,kind)==XR_XIR_OK);owner_free(&context,baseline);CHECK(points>0 && points<2048);
    if (!normal) for (size_t point=0;point<points;++point) {
        context=owner_new(caps());baseline=stats(&context).live_bytes;fail_at=attempts+point;injected=false;
        CHECK(norm_sequence(&context,kind)==XR_XIR_OUT_OF_MEMORY && injected);fail_at=SIZE_MAX;owner_free(&context,baseline);
    }
    if (!normal) for (uint32_t axis=0;axis<3;++axis) for (uint32_t delta=0;delta<3;++delta) {
        XrCompileResourceLimits bound=caps();uint64_t *limit=axis==0?&bound.allocated_bytes:axis==1?&bound.live_bytes:&bound.work;
        *limit=(axis==0?measured.allocated_bytes:axis==1?measured.peak_bytes:measured.work)+delta-1u;
        context=owner_new(bound);baseline=stats(&context).live_bytes;
        CHECK(norm_sequence(&context,kind)==(delta?XR_XIR_OK:XR_XIR_BUDGET));owner_free(&context,baseline);
    }
    printf("View normalization kind=%u: %zu allocation points, owner2/physical0%s PASS\n",kind,points,
        normal?"":"/all actual failures/three axes exact+minus1+plus1");
}
int main(int argc,char **argv) {
    bool normal=argc==2 && !strcmp(argv[1],"--normal");CHECK(argc==1 || normal);setvbuf(stdout,NULL,_IONBF,0);
    for (uint32_t kind=0;kind<N_KINDS;++kind) norm_resources(kind,normal);
    puts("Ordinary CFG lifetime rewrite, metadata slot cleanup, fresh SSA admission and owner checking PASS");return 0;
}
