/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_view_access_default_access_cases.h - Default closure access under live loans
 *
 * KEY CONCEPT:
 *   A real default edge cannot hide module storage behind zero SSA inputs.
 */
#ifndef XIR_VIEW_ACCESS_DEFAULT_ACCESS_CASES_H
#define XIR_VIEW_ACCESS_DEFAULT_ACCESS_CASES_H
#include "xir/xxir_defaults_internal.h"
#include "xir/xxir_checked.h"
#include "shared/xnative_declaration.h"
enum { DA_ARRAY=256,DA_CELL,DA_VIEW,DA_CALLABLE,DA_CASES=19 };
typedef struct DefaultAccessFixture {
    XrXirTypeNode nodes[4];XrXirTypes types;XrXirType scalar;
    XrXirInstruction ops[8][40];XrXirBlock blocks[8][4];uint32_t operands[8][16];
    XrXirFunction functions[8];XrXirFunctionIdentity identities[8];
    XrXirSourceModule source;XrXirDeclarations declarations;XrXirModule module;
    XrXirSlot slot;XrXirLiteral literal;XrXirDefaultBinding bindings[2];XrXirDefaultTable defaults;
    uint32_t cell,view,site,bad;XrXirStatus expected;bool prefix_bad;
} DefaultAccessFixture;
static uint32_t da_op(DefaultAccessFixture *f,uint32_t fn,XrXirInstruction op) {
    XrXirFunction *body=&f->functions[fn];uint32_t i=body->instruction_count++;
    CHECK(i<40);f->ops[fn][i]=op;f->blocks[fn][0].count=body->instruction_count;
    return body->parameter_count+i;
}
static void da_scalar(DefaultAccessFixture *f,uint32_t fn) {
    uint32_t n=da_op(f,fn,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=41});
    da_op(f,fn,(XrXirInstruction){.op=XR_XIR_RETURN,.args={n}});
}
static uint32_t da_loan(DefaultAccessFixture *f) {
    uint32_t zero=da_op(f,0,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64});
    uint32_t one=da_op(f,0,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=1});
    uint32_t at=f->functions[0].operand_count;f->operands[0][at]=f->cell;
    f->operands[0][at+1]=zero;f->operands[0][at+2]=one;f->functions[0].operand_count+=3;
    return da_op(f,0,(XrXirInstruction){.op=XR_XIR_VIEW_NEW,.type=(XrXirType)DA_VIEW,.args={at,3}});
}
static void da_use(DefaultAccessFixture *f,uint32_t view) {
    uint32_t zero=da_op(f,0,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64});
    da_op(f,0,(XrXirInstruction){.op=XR_XIR_VIEW_GET,.type=XR_XIR_U8,.args={view,zero}});
}
static void da_write(DefaultAccessFixture *f) {
    uint32_t payload=da_op(f,0,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_U8,.immediate=7});
    uint32_t at=f->functions[0].operand_count++;f->operands[0][at]=payload;
    uint32_t array=da_op(f,0,(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)DA_ARRAY,.args={at,1}});
    f->bad=f->functions[0].instruction_count;
    da_op(f,0,(XrXirInstruction){.op=XR_XIR_CELL_WRITE,.args={f->cell,array}});
}
static void da_fixture(DefaultAccessFixture *f,uint32_t kind) {
    memset(f,0,sizeof(*f));f->scalar=XR_XIR_I64;f->expected=XR_XIR_OK;
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_U8};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)DA_ARRAY};
    f->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_U8};
    f->nodes[3]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,.result=XR_XIR_I64,
        .flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    f->types=(XrXirTypes){f->nodes,4,NULL,NULL};
    for (uint32_t n=0;n<8;++n) {
        f->functions[n]=(XrXirFunction){.name="body",.name_length=4,.instructions=f->ops[n],
            .blocks=f->blocks[n],.block_count=1,.operands=f->operands[n],.error_type=XR_XIR_NEVER};
    }
    f->functions[0].name="caller";f->functions[0].name_length=6;
    f->functions[2].parameters=&f->scalar;f->functions[2].parameter_count=1;f->functions[2].result=XR_XIR_I64;
    f->functions[3].result=f->functions[4].result=f->functions[6].result=f->functions[7].result=XR_XIR_I64;
    f->functions[6].parameters=&f->scalar;f->functions[6].parameter_count=1;
    f->source=(XrXirSourceModule){.name="default_access",.name_length=14,.initializer=1};
    f->literal=(XrXirLiteral){"local",5};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,
        .literals=&f->literal,.literal_count=1,.root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    f->bindings[0]=(XrXirDefaultBinding){XR_XIR_DEFAULT_PARAMETER,2,0,3};
    f->defaults=(XrXirDefaultTable){f->bindings,1};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=8,
        .declarations=&f->declarations,.types=&f->types,.defaults=&f->defaults,.linkage_kind=XR_XIR_LIBRARY};
    da_op(f,2,(XrXirInstruction){.op=XR_XIR_RETURN,.args={0}});
    da_op(f,6,(XrXirInstruction){.op=XR_XIR_RETURN,.args={0}});
    bool slot=kind==4 || kind==5 || kind==6 || kind==7 || kind==9 || kind==18;
    if (slot) {
        f->slot=(XrXirSlot){0,kind==5?(XrXirType)DA_CELL:XR_XIR_I64,kind==5};
        f->declarations.slots=&f->slot;f->declarations.slot_count=1;
        uint32_t initial;
        if (kind==5) {
            uint32_t array=da_op(f,1,(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)DA_ARRAY});
            initial=da_op(f,1,(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)DA_CELL,.args={array}});
        } else initial=da_op(f,1,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=41});
        da_op(f,1,(XrXirInstruction){.op=XR_XIR_SLOT_INIT,.args={initial}});
    }
    da_op(f,1,(XrXirInstruction){.op=XR_XIR_RETURN});
    uint32_t payload=da_op(f,0,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_U8,.immediate=7});
    uint32_t at=f->functions[0].operand_count++;f->operands[0][at]=payload;
    uint32_t array=da_op(f,0,(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)DA_ARRAY,.args={at,1}});
    f->cell=da_op(f,0,(XrXirInstruction){.op=kind==5?XR_XIR_SLOT_LOAD:XR_XIR_CELL_NEW,
        .type=(XrXirType)DA_CELL,.args={kind==5?0:array}});
    f->view=da_loan(f);f->site=f->functions[0].instruction_count;f->bad=f->site;
    bool invoke=kind>=14 && kind<=16;
    if (invoke) f->functions[3].error_type=XR_XIR_ERROR;
    da_op(f,0,(XrXirInstruction){.op=invoke?XR_XIR_INVOKE_DEFAULT:XR_XIR_CALL_DEFAULT,.type=XR_XIR_I64,
        .args={invoke?2u:0u,0},.targets={invoke?1u:2u,invoke?2u:0u}});
    if (kind==12) {da_write(f);f->expected=XR_XIR_BAD_VALUE;}
    if (kind==13) {
        da_op(f,0,(XrXirInstruction){.op=XR_XIR_VIEW_END,.args={f->view}});da_write(f);
        f->view=da_loan(f);
    }
    if (invoke) {
        uint32_t first=f->functions[0].instruction_count;
        da_op(f,0,(XrXirInstruction){.op=XR_XIR_INVOKE_RESULT,.type=XR_XIR_I64,.immediate=f->site});
        if (kind==15) {da_write(f);f->expected=XR_XIR_BAD_VALUE;}
        da_use(f,f->view);da_op(f,0,(XrXirInstruction){.op=XR_XIR_VIEW_END,.args={f->view}});
        da_op(f,0,(XrXirInstruction){.op=XR_XIR_RETURN});uint32_t second=f->functions[0].instruction_count;
        da_op(f,0,(XrXirInstruction){.op=XR_XIR_INVOKE_ERROR,.type=XR_XIR_ERROR,.immediate=f->site});
        if (kind==16) {da_write(f);f->expected=XR_XIR_BAD_VALUE;}
        da_use(f,f->view);da_op(f,0,(XrXirInstruction){.op=XR_XIR_VIEW_END,.args={f->view}});
        da_op(f,0,(XrXirInstruction){.op=XR_XIR_RETURN});
        f->functions[0].block_count=3;f->blocks[0][0]=(XrXirBlock){.count=first};
        f->blocks[0][1]=(XrXirBlock){.first=first,.count=second-first};
        f->blocks[0][2]=(XrXirBlock){.first=second,.count=f->functions[0].instruction_count-second};
    } else {
        da_use(f,f->view);da_op(f,0,(XrXirInstruction){.op=XR_XIR_VIEW_END,.args={f->view}});
        da_op(f,0,(XrXirInstruction){.op=XR_XIR_RETURN});
    }
    if (kind==1 || kind==2 || kind==6) {
        da_op(f,3,(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=4});
    }
    if (kind==2) da_op(f,4,(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=4});
    if (kind==3 || kind==7) {
        f->bindings[1]=(XrXirDefaultBinding){XR_XIR_DEFAULT_PARAMETER,6,0,7};f->defaults.count=2;
        da_op(f,3,(XrXirInstruction){.op=XR_XIR_CALL_DEFAULT,.type=XR_XIR_I64,.targets={6,0}});
    }
    if (kind==4 || kind==5 || kind==6 || kind==7 || kind==9) {
        uint32_t target=kind==6?4:kind==7?7:kind==9?5:3;
        uint32_t loaded=da_op(f,target,(XrXirInstruction){.op=XR_XIR_SLOT_LOAD,.type=f->slot.type});
        if (kind==5) {
            uint32_t fresh=da_op(f,3,(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)DA_ARRAY});
            da_op(f,3,(XrXirInstruction){.op=XR_XIR_CELL_WRITE,.args={loaded,fresh}});
        }
        f->expected=XR_XIR_UNRESOLVED;
    }
    if (kind==8 || kind==9) {
        /* The cleanup is physically present even when it has no SSA capture. */
        f->identities[5].cleanup_owner=4;
        uint32_t register_at=f->functions[3].instruction_count;
        CHECK(register_at==0);
        da_op(f,3,(XrXirInstruction){.op=XR_XIR_CLEANUP_REGISTER,.immediate=5,.targets={1}});
        uint32_t first=f->functions[3].instruction_count;
        da_op(f,3,(XrXirInstruction){.op=XR_XIR_CLEANUP_LEAVE,.targets={2}});
        uint32_t second=f->functions[3].instruction_count;da_scalar(f,3);
        f->functions[3].block_count=3;f->blocks[3][0]=(XrXirBlock){.count=first};
        f->blocks[3][1]=(XrXirBlock){.first=first,.count=second-first,.frontier=1};
        f->blocks[3][2]=(XrXirBlock){.first=second,.count=f->functions[3].instruction_count-second};
    } else {
        if (kind==10) {
            uint32_t ref=da_op(f,3,(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)DA_CALLABLE,.immediate=4});
            da_op(f,3,(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=ref});
            f->expected=XR_XIR_UNRESOLVED;
        }
        if (kind==11) {
            const XrNativeIntrinsicDeclaration *native=xr_native_intrinsic_by_id(1);CHECK(native);
            uint32_t text=da_op(f,3,(XrXirInstruction){.op=XR_XIR_CONST_STRING,.type=XR_XIR_STRING});
            f->operands[3][0]=f->operands[3][1]=text;f->functions[3].operand_count=2;
            da_op(f,3,(XrXirInstruction){.op=XR_XIR_CALL_INTRINSIC,.type=XR_XIR_BOOL,.args={0,2},.immediate=native->id});
            f->expected=XR_XIR_UNRESOLVED;
        }
        if (kind==18) {
            uint32_t value=da_op(f,3,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64});
            da_op(f,3,(XrXirInstruction){.op=XR_XIR_SLOT_STORE,.args={value}});
            f->prefix_bad=true;f->expected=XR_XIR_BAD_STRUCTURE;
        }
        da_scalar(f,3);
    }
    da_scalar(f,4);da_op(f,5,(XrXirInstruction){.op=XR_XIR_RETURN});da_scalar(f,7);
    for (uint32_t n=0;n<8;++n) if (!f->functions[n].operand_count) f->functions[n].operands=NULL;
    if (kind==17) {f->ops[0][f->site].targets[0]=3;f->prefix_bad=true;f->expected=XR_XIR_BAD_STRUCTURE;}
}
static XrXirStatus da_sequence(const XrXirCompileContext *context,uint32_t kind) {
    DefaultAccessFixture f;da_fixture(&f,kind);unsigned char before[sizeof(f)];memcpy(before,&f,sizeof(f));
    XrXirConstruction *construction=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_module_construction(context,&f.module,&construction);
    const char *stage="construction";
    if (status==XR_XIR_OK) {stage="ordinary";status=xir_compile_view_prepare_ordinary(context,&f.module,construction,&diagnostic);}
    if (status==XR_XIR_OK) {stage="origins";status=xir_compile_view_return_origins_verified(context,&f.module,&diagnostic);}
    if (status==XR_XIR_OK) {stage="affine";status=xir_compile_view_affine_verified(context,&f.module,&diagnostic);}
    if (status==XR_XIR_OK) {
        stage="access";status=xir_compile_view_cell_access_verified(context,&f.module,&diagnostic);
        if (status==f.expected && status!=XR_XIR_OK) CHECK(diagnostic.function==0 && diagnostic.instruction==f.bad);
    }
    if (status!=f.expected && status!=XR_XIR_OUT_OF_MEMORY && status!=XR_XIR_MEMORY_LIMIT && status!=XR_XIR_BUDGET)
        fprintf(stderr,"default access kind=%u stage=%s status=%u expected=%u at %u/%u/%u\n",kind,stage,status,f.expected,
            diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(!memcmp(before,&f,sizeof(f)));xr_xir_compile_construction_free(construction);return status;
}
static void da_resources(uint32_t kind,bool normal) {
    DefaultAccessFixture f;da_fixture(&f,kind);XrXirCompileContext context=owner_new(caps());
    uint64_t baseline=stats(&context).held_bytes;size_t first=attempts;
    XrXirStatus status=da_sequence(&context,kind);
    if (status!=f.expected) fprintf(stderr,"default access first kind=%u status=%u expected=%u\n",kind,status,f.expected);
    CHECK(status==f.expected);size_t points=attempts-first;XrCompileResourceStats measured=stats(&context);
    CHECK(points && points<4096);CHECK(da_sequence(&context,kind)==f.expected);owner_free(&context,baseline);
    if (!normal) for (size_t p=0;p<points;++p) {
        context=owner_new(caps());baseline=stats(&context).held_bytes;fail_at=attempts+p;injected=false;
        status=da_sequence(&context,kind);fail_at=SIZE_MAX;
        if (status!=XR_XIR_OUT_OF_MEMORY || !injected) fprintf(stderr,"default access FI kind=%u point=%zu status=%u\n",kind,p,status);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && injected);owner_free(&context,baseline);
    }
    if (!normal) for (uint32_t delta=0;delta<3;++delta) {
        XrCompileResourceLimits limits=caps();limits.memory_mode=XR_COMPILE_MEMORY_FINITE;limits.memory_limit=measured.peak_bytes+delta-1u;
        context=owner_new(limits);baseline=stats(&context).held_bytes;
        CHECK(da_sequence(&context,kind)==(delta?f.expected:XR_XIR_MEMORY_LIMIT));
        if (delta) CHECK(da_sequence(&context,kind)==f.expected);owner_free(&context,baseline);
    }
    printf("Default access kind=%u: live loan / %zu allocation points / repeat / physical0%s PASS\n",
        kind,points,normal?"":" / OOM / held exact+minus1+plus1");
}
typedef struct DefaultAccessScopeCase {DefaultAccessFixture *fixture;const XrXirCompileContext *parent;uint32_t mode,calls;} DefaultAccessScopeCase;
static XrXirStatus da_scope_visit(void *data,const XrXirCompileContext *remaining,XirEffectDefaultAccessScope *scope) {
    DefaultAccessScopeCase *c=data;CHECK(remaining->resources==c->parent->resources);
    CHECK(remaining->limits.frame_bytes<c->parent->limits.frame_bytes);++c->calls;
    if (c->mode==1) {
        CHECK(xir_effects_default_access_site(scope,remaining,UINT32_MAX,0)==XR_XIR_BAD_STRUCTURE);
        CHECK(xir_effects_default_access_site(scope,remaining,0,c->fixture->site)==XR_XIR_BAD_STRUCTURE);
        return XR_XIR_OK;
    }
    CHECK(xir_effects_default_access_site(scope,remaining,0,c->fixture->site)==XR_XIR_OK);
    if (c->mode==2) c->fixture->ops[0][c->fixture->site].targets[0]=3;
    return XR_XIR_OK;
}
static void da_scope_admit(const XrXirCompileContext *context,const DefaultAccessFixture *f,XrXirConstruction **construction) {
    CHECK(xir_fixture_module_construction(context,&f->module,construction)==XR_XIR_OK);
    CHECK(xir_compile_view_prepare_ordinary(context,&f->module,*construction,NULL)==XR_XIR_OK);
    CHECK(xir_compile_view_return_origins_verified(context,&f->module,NULL)==XR_XIR_OK);
    CHECK(xir_compile_view_affine_verified(context,&f->module,NULL)==XR_XIR_OK);
}
static void da_scope_cases(void) {
    for (uint32_t mode=0;mode<3;++mode) {
        DefaultAccessFixture f;da_fixture(&f,0);XrXirCompileContext context=owner_new(caps());uint64_t baseline=stats(&context).held_bytes;
        XrXirConstruction *construction=NULL;da_scope_admit(&context,&f,&construction);
        CHECK(xir_compile_view_access_verified(&context,&f.module,NULL)==XR_XIR_UNRESOLVED);
        DefaultAccessScopeCase request={&f,&context,mode,0};
        CHECK(xir_effects_default_access_operation(&context,&f.module,da_scope_visit,&request)==(mode?XR_XIR_BAD_STRUCTURE:XR_XIR_OK));
        CHECK(request.calls==1);f.ops[0][f.site].targets[0]=2;
        CHECK(da_sequence(&context,0)==XR_XIR_OK);
        xr_xir_compile_construction_free(construction);owner_free(&context,baseline);
    }
    DefaultAccessFixture f;da_fixture(&f,0);XrXirCompileContext context=owner_new(caps());XrXirCellProvenance *cells=NULL;
    XrXirConstruction *construction=NULL;da_scope_admit(&context,&f,&construction);
    CHECK(xr_xir_compile_cell_provenance_verified(&context,&f.module,&cells,NULL)==XR_XIR_OK && cells);
    xr_xir_compile_construction_free(construction);
    xr_compile_resources_release(context.resources);context=(XrXirCompileContext){0};
    CHECK(live && live_bytes);CHECK((xr_xir_cell_provenance_origin(cells,0,f.cell)&XR_XIR_CELL_ORIGIN_OWNED)!=0);
    xr_xir_compile_cell_provenance_free(cells);CHECK(!live && !live_bytes);
}
static void da_packet_cases(void) {
    for (uint32_t route=0;route<2;++route) {
        DefaultAccessFixture f;da_fixture(&f,route?14u:0u);XrXirCompileContext context=owner_new(caps());
        XrXirArtifact *checked=NULL,*decoded=NULL;XrXirCheckedPacket packet={0};XrXirDiagnostic diagnostic={0};
        XrXirStatus status=xir_fixture_check(&context,&f.module,&checked,&diagnostic);
        if (status!=XR_XIR_OK) fprintf(stderr,"default packet route=%u check=%u at %u/%u/%u\n",route,status,
            diagnostic.function,diagnostic.block,diagnostic.instruction);
        CHECK(status==XR_XIR_OK && checked);
        CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
        XrXirCompileContext receiver=context;CHECK(xr_compile_resources_retain(receiver.resources)==XR_COMPILE_RESOURCE_OK);
        xr_xir_compile_artifact_free(checked);checked=NULL;memset(&f,0xa5,sizeof(f));
        xr_compile_resources_release(context.resources);context=(XrXirCompileContext){0};
        CHECK(xr_xir_compile_checked_read(&receiver,packet.bytes,packet.length,&decoded,&diagnostic)==XR_XIR_OK && decoded);
        xr_xir_compile_checked_packet_free(&packet);
        xr_compile_resources_release(receiver.resources);receiver=(XrXirCompileContext){0};
        CHECK(xr_xir_compile_artifact_verify(decoded,NULL)==XR_XIR_OK);
        const XrXirModule *module=xr_xir_compile_artifact_module(decoded);CHECK(module->defaults && module->defaults->count==1);
        xr_xir_compile_artifact_free(decoded);CHECK(!live && !live_bytes);
    }
    puts("Default CALL/INVOKE checked packet: destroyed raw/Artifact/request producers, reverify and physical0 PASS");
}
static bool da_frame_accepts(uint64_t bytes) {
    XrXirCompileContext context=owner_new(caps());uint64_t baseline=stats(&context).held_bytes;
    context.limits.frame_bytes=bytes;XrXirStatus status=da_sequence(&context,0);
    CHECK(status==XR_XIR_OK || status==XR_XIR_BUDGET);owner_free(&context,baseline);return status==XR_XIR_OK;
}
static void default_access_cases(bool normal) {
    for (uint32_t kind=0;kind<DA_CASES;++kind) da_resources(kind,normal);
    da_scope_cases();da_packet_cases();uint64_t low=1,high=xr_xir_compile_default_limits().frame_bytes;
    while (low<high) {uint64_t middle=low+(high-low)/2u;if (da_frame_accepts(middle)) high=middle;else low=middle+1;}
    CHECK(da_frame_accepts(low) && !da_frame_accepts(low-1u) && da_frame_accepts(low+1u));
    printf("Default access scoped frame boundary=%llu exact/minus1/plus1; producer death; first failure; current input PASS\n",(unsigned long long)low);
}
#endif // XIR_VIEW_ACCESS_DEFAULT_ACCESS_CASES_H