/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_view_access_scalar_slot_cases.h - Scalar publication preserves later loans
 */
#ifndef XIR_VIEW_ACCESS_SCALAR_SLOT_CASES_H
#define XIR_VIEW_ACCESS_SCALAR_SLOT_CASES_H

enum { SS_ARRAY=256,SS_OWNER,SS_VIEW,SS_CELL,SS_CASES=14 };
typedef struct ScalarSlotFixture {
    XrXirTypeNode nodes[4];XrXirTypes types;
    XrXirInstruction ops[32],entry_ops[2],other;uint32_t operands[8];
    XrXirBlock block,entry_block,other_block;XrXirFunction functions[3];
    XrXirSourceModule source;XrXirFunctionIdentity identities[3];
    XrXirSlot slot;XrXirLiteral literal;XrXirDeclarations declarations;XrXirModule module;
    uint32_t init,bad;XrXirStatus expected;bool prefix_rejected;
} ScalarSlotFixture;
static uint32_t scalar_slot_op(ScalarSlotFixture *f,XrXirInstruction op) {
    uint32_t at=f->functions[0].instruction_count++;CHECK(at<32);f->ops[at]=op;return at;
}
static void scalar_slot_fixture(ScalarSlotFixture *f,uint32_t kind,bool lowered) {
    memset(f,0,sizeof(*f));
    XrXirType scalar=kind==1?XR_XIR_BOOL:kind==2?XR_XIR_RUNE:kind==3?XR_XIR_F64:
        kind==4?XR_XIR_U8:kind==10?(XrXirType)SS_ARRAY:kind==11?XR_XIR_STRING:kind==12?XR_XIR_UNIT:XR_XIR_I64;
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)SS_ARRAY};
    f->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_I64};
    f->nodes[3]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=kind==10?XR_XIR_I64:scalar};
    f->types=(XrXirTypes){f->nodes,4,NULL,NULL};
    f->functions[0]=(XrXirFunction){.name="$init",.name_length=5,.instructions=f->ops,
        .blocks=&f->block,.block_count=1,.operands=f->operands,.error_type=XR_XIR_NEVER};
    f->entry_ops[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64};
    f->entry_ops[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0}};
    f->entry_block=(XrXirBlock){.count=2};
    f->other=(XrXirInstruction){.op=XR_XIR_RETURN};f->other_block=(XrXirBlock){.count=1};
    f->functions[1]=(XrXirFunction){.name="other",.name_length=5,.instructions=&f->other,
        .instruction_count=1,.blocks=&f->other_block,.block_count=1,.error_type=XR_XIR_NEVER};
    f->functions[2]=(XrXirFunction){.name="entry",.name_length=5,.result=XR_XIR_I64,
        .instructions=f->entry_ops,.instruction_count=2,.blocks=&f->entry_block,.block_count=1,.error_type=XR_XIR_NEVER};
    f->source=(XrXirSourceModule){.name="scalar_slot",.name_length=11,.initializer=kind==7?1u:0u};
    XrXirType storage=(XrXirType)(kind==10?SS_OWNER:SS_CELL);
    f->slot=(XrXirSlot){0,kind==13?(XrXirType)SS_VIEW:storage,kind==13?0u:1u};
    f->literal=(XrXirLiteral){"x",1};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,
        .slots=&f->slot,.slot_count=1,.literals=&f->literal,.literal_count=1,
        .root_module=lowered?0u:UINT32_MAX,.entry_function=lowered?2u:UINT32_MAX};
    f->module=(XrXirModule){.stage=lowered?XR_XIR_LOWERED:XR_XIR_BUILT,
        .functions=f->functions,.function_count=3,.declarations=&f->declarations,.types=&f->types,
        .linkage_kind=lowered?XR_XIR_PROGRAM:XR_XIR_LIBRARY};
    uint32_t zero=scalar_slot_op(f,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64});
    uint32_t one=scalar_slot_op(f,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=1});
    XrXirOp literal=scalar==XR_XIR_BOOL?XR_XIR_CONST_BOOL:scalar==XR_XIR_RUNE?XR_XIR_CONST_RUNE:scalar==XR_XIR_F64?XR_XIR_CONST_FLOAT:
        scalar==XR_XIR_STRING?XR_XIR_CONST_STRING:kind==10?XR_XIR_ARRAY_NEW:XR_XIR_CONST_INT;
    uint32_t initial=scalar==XR_XIR_UNIT?0u:scalar_slot_op(f,(XrXirInstruction){.op=literal,.type=scalar});
    uint32_t cell=scalar_slot_op(f,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=storage,.args={initial}});
    uint32_t array=scalar_slot_op(f,(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)SS_ARRAY});
    uint32_t owner=scalar_slot_op(f,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)SS_OWNER,.args={array}});
    uint32_t view=UINT32_MAX;
    f->operands[0]=owner;f->operands[1]=zero;f->operands[2]=one;f->functions[0].operand_count=3;
    if (kind==5 || kind==6 || kind==13)
        view=scalar_slot_op(f,(XrXirInstruction){.op=XR_XIR_VIEW_NEW,.type=(XrXirType)SS_VIEW,.args={0,3}});
    f->init=scalar_slot_op(f,(XrXirInstruction){.op=kind==8?XR_XIR_SLOT_STORE:XR_XIR_SLOT_INIT,
        .args={kind==9?zero:kind==13?view:cell}});
    f->bad=f->init;f->expected=kind>=10?XR_XIR_UNRESOLVED:XR_XIR_OK;
    f->prefix_rejected=(kind>=7 && kind<=9) || kind==13;
    if (f->prefix_rejected) f->expected=kind==9 || kind==13?XR_XIR_BAD_TYPE:XR_XIR_BAD_STRUCTURE;
    if (view==UINT32_MAX)
        view=scalar_slot_op(f,(XrXirInstruction){.op=XR_XIR_VIEW_NEW,.type=(XrXirType)SS_VIEW,.args={0,3}});
    if (kind==6) {
        f->bad=scalar_slot_op(f,(XrXirInstruction){.op=XR_XIR_CELL_WRITE,.args={owner,array}});
        f->expected=XR_XIR_BAD_VALUE;
    }
    scalar_slot_op(f,(XrXirInstruction){.op=XR_XIR_VIEW_GET,.type=XR_XIR_I64,.args={view,zero}});
    scalar_slot_op(f,(XrXirInstruction){.op=XR_XIR_VIEW_END,.args={view}});
    scalar_slot_op(f,(XrXirInstruction){.op=XR_XIR_RETURN});
    f->block.count=f->functions[0].instruction_count;
}
static void scalar_slot_case(uint32_t kind,bool lowered) {
    ScalarSlotFixture f;scalar_slot_fixture(&f,kind,lowered);
    XrXirCompileContext context=owner_new(caps());uint64_t baseline=stats(&context).live_bytes;
    unsigned char before[sizeof(f)];memcpy(before,&f,sizeof(f));
    XrXirConstruction *construction=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_module_construction(&context,&f.module,&construction);
    if (status==XR_XIR_OK) status=xir_compile_view_prepare_ordinary(&context,&f.module,construction,&diagnostic);
    if (!f.prefix_rejected) {
        CHECK(status==XR_XIR_OK);
        CHECK(xir_compile_view_return_origins_verified(&context,&f.module,&diagnostic)==XR_XIR_OK);
        CHECK(xir_compile_view_affine_verified(&context,&f.module,&diagnostic)==XR_XIR_OK);
        XrXirCompileContext tiny=context;tiny.limits.frame_bytes=1;
        CHECK(xir_compile_view_access_verified(&tiny,&f.module,NULL)==XR_XIR_BUDGET);
        status=xir_compile_view_cell_access_verified(&context,&f.module,&diagnostic);
    }
    if (status!=f.expected)
        fprintf(stderr,"scalar slot case=%u lowered=%u status=%u expected=%u at %u/%u/%u\n",
            kind,(unsigned)lowered,status,f.expected,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==f.expected);
    if (status!=XR_XIR_OK && !f.prefix_rejected) CHECK(diagnostic.function==0 && diagnostic.instruction==f.bad);
    CHECK(!memcmp(before,&f,sizeof(f)));
    xr_xir_compile_construction_free(construction);owner_free(&context,baseline);
}
static void scalar_slot_cases(void) {
    for (uint32_t kind=0;kind<SS_CASES;++kind) {
        scalar_slot_case(kind,false);scalar_slot_case(kind,true);
    }
    puts("Scalar slot initialization preserves publication guards and later borrowed-owner conflicts");
}
#endif
