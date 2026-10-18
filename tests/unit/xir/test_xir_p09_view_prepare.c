/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_view_prepare.c - Real ordinary module admission before lifetime work
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_view_prepare_internal.h"
#include "tuple_owner_observer.h"
enum {PRE_SHARED,PRE_MUT,PRE_OPTIONAL,PRE_ENDED,PRE_BAD_OP,PRE_BAD_TYPE,PRE_BAD_PARAMETERS,
    PRE_BAD_ENTRY,PRE_BAD_INIT,PRE_BAD_DECLARATION,PRE_BAD_SSA,PRE_BAD_RETURN,PRE_UNREACHABLE,
    PRE_DOMINANCE,PRE_BAD_PANIC,PRE_BAD_FRONTIER,PRE_BAD_SELECTOR,PRE_KINDS};
typedef struct PrepareFixture {
    XrXirTypeNode nodes[3];XrXirTypes types;XrXirType parameter;
    XrXirInstruction ops[3][6];XrXirBlock blocks[3][3];XrXirFunction functions[3];
    XrXirSourceModule source;XrXirFunctionIdentity identities[3];
    XrXirDeclarations declarations;XrXirModule module;
} PrepareFixture;
static void prepare_fixture(PrepareFixture *f) {
    memset(f,0,sizeof(*f));
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_I64};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_MUT_SLICE,.element=XR_XIR_I64};
    f->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)256};
    f->types=(XrXirTypes){f->nodes,3,NULL,NULL};f->parameter=(XrXirType)256;
    for (uint32_t i=0;i<3;++i) {
        f->ops[i][0]=(XrXirInstruction){.op=XR_XIR_RETURN};f->blocks[i][0]=(XrXirBlock){0,1,0,0};
        f->functions[i]=(XrXirFunction){"body",4,NULL,0,XR_XIR_UNIT,f->blocks[i],1,f->ops[i],1,NULL,0};
    }
    f->ops[1][0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64};
    f->ops[1][1]=(XrXirInstruction){.op=XR_XIR_RETURN};f->functions[1].result=XR_XIR_I64;
    f->functions[1].instruction_count=f->blocks[1][0].count=2;
    f->functions[2].parameters=&f->parameter;f->functions[2].parameter_count=1;f->functions[2].result=f->parameter;
    f->source=(XrXirSourceModule){"root",4,NULL,0,0};
    f->declarations=(XrXirDeclarations){&f->source,1,f->identities,NULL,0,NULL,0,0,1,NULL};
    f->module=(XrXirModule){XR_XIR_BUILT,f->functions,3,&f->declarations,NULL,&f->types,NULL,XR_XIR_PROGRAM,NULL};
}
static XrXirStatus prepare_case(PrepareFixture *f,uint32_t kind) {
    XrXirFunction *body=f->functions+2;XrXirInstruction *ops=f->ops[2];
    switch(kind) {
    case PRE_SHARED:return XR_XIR_OK;
    case PRE_MUT:case PRE_OPTIONAL:f->parameter=body->result=(XrXirType)(kind==PRE_MUT?257:258);return XR_XIR_OK;
    case PRE_ENDED:
        ops[0]=(XrXirInstruction){.op=XR_XIR_VIEW_END};
        ops[1]=(XrXirInstruction){.op=XR_XIR_VIEW_LEN,.type=XR_XIR_I64};
        ops[2]=(XrXirInstruction){.op=XR_XIR_RETURN};body->instruction_count=f->blocks[2][0].count=3;return XR_XIR_OK;
    case PRE_BAD_OP:ops[0].op=XR_XIR_OP_COUNT;return XR_XIR_BAD_STRUCTURE;
    case PRE_BAD_TYPE:f->nodes[0].element=XR_XIR_UNIT;return XR_XIR_BAD_TYPE;
    case PRE_BAD_PARAMETERS:body->parameters=NULL;return XR_XIR_BAD_STRUCTURE;
    case PRE_BAD_ENTRY:f->functions[1].result=XR_XIR_UNIT;return XR_XIR_BAD_TYPE;
    case PRE_BAD_INIT:f->functions[0].result=XR_XIR_I64;return XR_XIR_BAD_TYPE;
    case PRE_BAD_DECLARATION:f->declarations.root_module=1;return XR_XIR_BAD_STRUCTURE;
    case PRE_BAD_SSA:ops[0].args[0]=99;return XR_XIR_BAD_VALUE;
    case PRE_BAD_RETURN:body->result=XR_XIR_I64;return XR_XIR_BAD_TYPE;
    case PRE_UNREACHABLE:
        ops[1]=ops[0];body->instruction_count=2;body->block_count=2;f->blocks[2][1]=(XrXirBlock){1,1,0,0};return XR_XIR_BAD_STRUCTURE;
    case PRE_DOMINANCE:
        body->result=XR_XIR_I64;body->instruction_count=6;body->block_count=3;
        f->blocks[2][0]=(XrXirBlock){0,2,0,0};f->blocks[2][1]=(XrXirBlock){2,2,0,0};f->blocks[2][2]=(XrXirBlock){4,2,0,0};
        ops[0]=(XrXirInstruction){.op=XR_XIR_CONST_BOOL,.type=XR_XIR_BOOL,.immediate=1};
        ops[1]=(XrXirInstruction){.op=XR_XIR_BRANCH,.args={1},.targets={1,2}};
        ops[2]=(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)256};
        ops[3]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={2}};
        ops[4]=(XrXirInstruction){.op=XR_XIR_VIEW_LEN,.type=XR_XIR_I64,.args={3}};
        ops[5]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={5}};return XR_XIR_BAD_DOMINANCE;
    case PRE_BAD_PANIC:f->blocks[2][0].panic=2;return XR_XIR_BAD_STRUCTURE;
    case PRE_BAD_FRONTIER:f->blocks[2][0].frontier=1;return XR_XIR_BAD_STRUCTURE;
    case PRE_BAD_SELECTOR:ops[0]=(XrXirInstruction){.op=XR_XIR_INVOKE_RESULT,.type=(XrXirType)256};return XR_XIR_BAD_STRUCTURE;
    default:CHECK(false);return XR_XIR_BAD_STRUCTURE;
    }
}
static XrXirStatus prepare_sequence(XrXirCompileContext *context,uint32_t kind) {
    PrepareFixture f;prepare_fixture(&f);XrXirConstruction *construction=NULL;
    XrXirStatus status=xir_fixture_construction(context,&f.types,&construction);
    if (status!=XR_XIR_OK) return status;
    XrXirStatus expected=prepare_case(&f,kind);unsigned char before[sizeof(f)];memcpy(before,&f,sizeof(f));
    XrXirDiagnostic diagnostic={0};status=xir_compile_view_prepare_ordinary(context,&f.module,construction,&diagnostic);
    CHECK(!memcmp(before,&f,sizeof(f)));
    if (status==XR_XIR_BUDGET || status==XR_XIR_OUT_OF_MEMORY) goto done;
    if (status!=expected) fprintf(stderr,"ordinary prepare kind=%u expected=%u actual=%u function=%u block=%u instruction=%u\n",
        kind,(unsigned)expected,(unsigned)status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==expected && diagnostic.status==status);
    if (status==XR_XIR_OK) {
        status=xr_xir_compile_structure_verify_v2(context,&f.module,construction,&diagnostic);
        if (status==XR_XIR_BUDGET || status==XR_XIR_OUT_OF_MEMORY) goto done;
        CHECK(status==(kind==PRE_ENDED?XR_XIR_BAD_VALUE:XR_XIR_BAD_TYPE));
    }
    status=XR_XIR_OK;
done:xr_xir_compile_construction_free(construction);return status;
}
static void prepare_resources(uint32_t kind,bool normal) {
    XrXirCompileContext context=owner_new(caps());uint64_t baseline=stats(&context).live_bytes;size_t start=attempts;
    CHECK(prepare_sequence(&context,kind)==XR_XIR_OK);size_t points=attempts-start;XrCompileResourceStats measured=stats(&context);
    CHECK(prepare_sequence(&context,kind)==XR_XIR_OK);owner_free(&context,baseline);CHECK(points>1 && points<256);
    if (!normal) for (size_t point=0;point<points;++point) {
        context=owner_new(caps());baseline=stats(&context).live_bytes;fail_at=attempts+point;injected=false;
        CHECK(prepare_sequence(&context,kind)==XR_XIR_OUT_OF_MEMORY && injected);fail_at=SIZE_MAX;owner_free(&context,baseline);
    }
    if (!normal) for (uint32_t axis=0;axis<3;++axis) for (uint32_t adjustment=0;adjustment<3;++adjustment) {
        XrCompileResourceLimits bound=caps();uint64_t *limit=axis==0?&bound.allocated_bytes:axis==1?&bound.live_bytes:&bound.work;
        *limit=(axis==0?measured.allocated_bytes:axis==1?measured.peak_bytes:measured.work)+adjustment-1u;
        context=owner_new(bound);baseline=stats(&context).live_bytes;
        CHECK(prepare_sequence(&context,kind)==(adjustment?XR_XIR_OK:XR_XIR_BUDGET));owner_free(&context,baseline);
    }
    printf("ordinary prepare case=%u: %zu allocation points, same owner2/physical0%s PASS\n",kind,points,
        normal?"":"/all actual failures/three axes exact+minus1+plus1");
}
int main(int argc,char **argv) {
    bool normal=argc==2 && !strcmp(argv[1],"--normal");CHECK(argc==1 || normal);setvbuf(stdout,NULL,_IONBF,0);
    for (uint32_t kind=0;kind<PRE_KINDS;++kind) prepare_resources(kind,normal);
    puts("Real ordinary module/type/declaration/SSA prefix, immutable input and complete borrowed gate retained PASS");return 0;
}
