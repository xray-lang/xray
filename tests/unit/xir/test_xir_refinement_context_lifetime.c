/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_refinement_context_lifetime.c - Retire stale refinement views safely
 */
#include "xir/xxir_internal.h"
#include "xir/xxir_types.h"
#include "xir/xxir_source.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "xir_root_parameter_fixture.h"
#include "xir/xxir_effects.c"

typedef struct LifetimeFixture {
    XrXirCompileContext context;
    XrXirSourceResult result;
    uint64_t baseline;
    uint32_t function,instruction;
    XrXirType narrow,wide;
} LifetimeFixture;
static void lifetime_fixture(LifetimeFixture *f) {
    memset(f,0,sizeof(*f));f->context=rp_owner(rp_caps());f->baseline=rp_stats(&f->context).live_bytes;
    static const char text[]="fn answer()->i64{return 42}\nexport fn references()->i64{const first=answer;const second=answer;return 0}\n";
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(f->context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"refinement-lifetime",NULL};
    XrXirSourceRequest request={session,NULL,&authority,&f->context,NULL,NULL,XR_XIR_LIBRARY,NULL};
    XrXirSourceText input={NULL,text,sizeof(text)-1};XrXirSourceDiagnostic diagnostic={0};char *failure=NULL;
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&f->result,&diagnostic,&failure);
    if (status!=XR_XIR_OK) fprintf(stderr,"LIFETIME_SOURCE status%u line%d col%d work%llu %s\n",status,
        diagnostic.line,diagnostic.column,(unsigned long long)rp_stats(&f->context).work,diagnostic.message);
    xr_compile_resources_free(failure);xr_compile_session_free(session);CHECK(status==XR_XIR_OK && f->result.checked);
    const XrXirModule *module=xr_xir_compile_artifact_module(f->result.checked);
    f->function=UINT32_MAX;
    for (uint32_t n=0;n<module->function_count;++n) if (module->functions[n].name_length==10 &&
        !memcmp(module->functions[n].name,"references",10)) f->function=n;
    CHECK(f->function!=UINT32_MAX && module->function_count<=32);
    const XrXirFunction *body=&module->functions[f->function];f->instruction=UINT32_MAX;
    for (uint32_t i=0;i<body->instruction_count;++i) if (body->instructions[i].op==XR_XIR_FUNCTION_REF) {
        f->instruction=i;break;
    }
    CHECK(f->instruction!=UINT32_MAX && body->instruction_count<=128);
    f->narrow=body->instructions[f->instruction].type;f->wide=XR_XIR_UNIT;
    const XrXirTypeNode *narrow=xr_xir_callable_signature(module->types,f->narrow);CHECK(narrow);
    for (uint32_t t=0;t<module->types->count;++t) {
        const XrXirTypeNode *node=&module->types->nodes[t];
        if (node->kind==XR_XIR_TYPE_CALLABLE && node->result==narrow->result && !node->parameter_count &&
            node->flags==((narrow->flags&XR_XIR_CALLABLE_NO_SUSPEND)|XR_XIR_CALLABLE_ROOT_UNRESOLVED))
            f->wide=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+t);
    }
    CHECK(f->wide!=XR_XIR_UNIT && xr_xir_compile_callable_weakening(&f->context,module->types,f->narrow,f->wide)==XR_XIR_OK);
}

typedef struct LifetimeOperation {
    const XrXirCompileContext *context;
    const LifetimeFixture *fixture;
    XrXirModule source;
    XrXirInstruction *operations;
    EffectBodyMaterial *retained;
    size_t fail,sites;
    uint32_t calls;
    bool refined,require_retirement,injected,retired,certificate_intact,keep;
} LifetimeOperation;
typedef struct LifetimeBorrow {
    LifetimeOperation *operation;
    XrXirEffects *effects;
} LifetimeBorrow;
static XrXirStatus lifetime_borrowed_refresh(void *opaque,XirEffectContextScope *scope) {
    LifetimeBorrow *borrow=opaque;LifetimeOperation *op=borrow->operation;CHECK(scope);
    size_t attempts=rp_attempts;XrCompileResourceStats before=rp_stats(op->context);
    CHECK(xir_effects_context_refresh(op->context,&op->source,borrow->effects)==XR_XIR_BAD_STRUCTURE);
    CHECK(effect_context_owner_refresh(op->context,&op->source,borrow->effects,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(rp_attempts==attempts && rp_stats(op->context).work==before.work);
    return XR_XIR_BAD_STRUCTURE;
}
static void lifetime_controls(LifetimeOperation *op,XrXirEffects *effects) {
    LifetimeBorrow borrow={op,effects};size_t attempts=rp_attempts;
    CHECK(xir_effects_context_operation(op->context,effects,&op->source,op->source.types,
        lifetime_borrowed_refresh,&borrow)==XR_XIR_BAD_STRUCTURE && rp_attempts==attempts);
    CHECK(xir_effects_context_available(op->context,effects));
    XrXirCompileContext foreign=rp_owner(rp_caps());uint64_t live=rp_stats(&foreign).live_bytes;
    attempts=rp_attempts;XrCompileResourceStats before=rp_stats(&foreign);
    CHECK(xir_effects_context_refresh(&foreign,&op->source,effects)==XR_XIR_BAD_STRUCTURE &&
        rp_attempts==attempts && rp_stats(&foreign).work==before.work);
    rp_owner_free(&foreign,live);
    if (!op->refined) return;
    XrXirFunctionIdentity identities[32];
    memcpy(identities,op->source.declarations->functions,op->source.function_count*sizeof(*identities));
    XrXirDeclarations declarations=*op->source.declarations;declarations.functions=identities;
    XrXirModule wrong=op->source;wrong.declarations=&declarations;
    identities[op->fixture->function].cleanup_owner=op->fixture->function+1;
    CHECK(xir_effects_context_refresh(op->context,&wrong,effects)==XR_XIR_BAD_STRUCTURE &&
        xir_effects_context_available(op->context,effects));
}

static XrXirStatus lifetime_refresh(LifetimeOperation *op,XrXirEffects *effects) {
    const LifetimeFixture *fixture=op->fixture;
    CHECK(effects->contexts && effects->invocations && !!effects->refinement==op->refined);
    lifetime_controls(op,effects);
    EffectInvocationCertificate *certificate=effects->invocations,saved=*certificate;
    if (op->keep) {
        XrXirStatus status=effect_body_material_retain(op->context,certificate->body);
        if (status!=XR_XIR_OK) return status;
        op->retained=certificate->body;
    }
    XrXirFunction previous[32];XrXirInstruction instructions[128];
    memcpy(previous,op->source.functions,op->source.function_count*sizeof(*previous));
    uint32_t count=previous[fixture->function].instruction_count;
    memcpy(instructions,op->operations,count*sizeof(*instructions));previous[fixture->function].instructions=instructions;
    XrXirModule old=op->source;old.functions=previous;
    CHECK(effect_body_source_match(op->context,certificate->body,&old)==XR_XIR_OK);
    op->operations[fixture->instruction].type=fixture->narrow;
    CHECK(effect_body_source_match(op->context,certificate->body,&op->source)==XR_XIR_BAD_STRUCTURE);
    size_t before=rp_attempts;rp_injected=false;
    if (op->fail!=SIZE_MAX) rp_fail_at=before+op->fail;
    XrXirStatus status=xir_effects_context_refresh(op->context,&op->source,effects);
    op->sites=rp_attempts-before;op->injected=rp_injected;rp_fail_at=SIZE_MAX;
    op->retired=!effects->contexts;
    op->certificate_intact=effects->invocations==certificate && !memcmp(certificate,&saved,sizeof(saved));
    if (status!=XR_XIR_BUDGET) op->certificate_intact=op->certificate_intact &&
        effect_body_source_match(op->context,certificate->body,&old)==XR_XIR_OK;
    CHECK(op->certificate_intact);
    if (op->require_retirement) CHECK(status==XR_XIR_OUT_OF_MEMORY && op->retired &&
        !xir_effects_context_available(op->context,effects));
    if (status==XR_XIR_OK) {
        uint32_t mask=UINT32_MAX;
        status=xir_effects_reference_root(op->context,effects,&op->source,fixture->function,fixture->instruction,&mask);
        if (status==XR_XIR_OK) CHECK(!mask);
        else if (status!=XR_XIR_BUDGET) fprintf(stderr,"LIFETIME_QUERY status%u mask%u\n",status,mask);
    }
    return status;
}
static XrXirStatus lifetime_refine(void *opaque,const XrXirEffects *effects,bool *changed) {
    LifetimeOperation *op=opaque;*changed=false;
    if (!op->calls++) return xir_effects_refinement_capture(op->context,&op->source,effects);
    if (op->calls!=2) return XR_XIR_OK;
    XrXirStatus status=lifetime_refresh(op,(XrXirEffects *)effects);*changed=status==XR_XIR_OK;return status;
}
static XrXirStatus lifetime_operation(const XrXirCompileContext *context,const LifetimeFixture *fixture,
    uint32_t mode,size_t fail,bool require_retirement,LifetimeOperation *record) {
    const XrXirModule *source=xr_xir_compile_artifact_module(fixture->result.checked);XrXirStatus status=XR_XIR_OK;
    XrXirFunction *functions=xir_compile_copy(context,source->functions,(size_t)source->function_count*sizeof(*functions),&status);
    uint32_t count=source->functions[fixture->function].instruction_count;
    XrXirInstruction *instructions=status==XR_XIR_OK?xir_compile_copy(context,source->functions[fixture->function].instructions,
        (size_t)count*sizeof(*instructions),&status):NULL;
    LifetimeOperation op={.context=context,.fixture=fixture,.source=*source,.operations=instructions,
        .fail=fail,.refined=mode==0,.require_retirement=require_retirement,.keep=record && record->keep};
    if (status==XR_XIR_OK) {
        op.source.functions=functions;functions[fixture->function].instructions=instructions;
        instructions[fixture->instruction].type=fixture->wide;
        XrXirEffects *effects=NULL;XrXirRootRefiner refiner={&op,lifetime_refine};
        status=mode==0?xr_xir_compile_effects_refine_verified(context,&op.source,&refiner,&effects):
            xr_xir_compile_effects_infer_verified(context,&op.source,&effects);
        if (status==XR_XIR_OK && mode==1) status=lifetime_refresh(&op,effects);
        if (status==XR_XIR_OK) CHECK(effects && op.certificate_intact);
        else if (mode==0) CHECK(!effects);
        xr_xir_compile_effects_free(effects);
    }
    xr_compile_resources_free(instructions);xr_compile_resources_free(functions);if (record) *record=op;return status;
}
static size_t lifetime_resources(const LifetimeFixture *fixture,uint32_t mode,bool all) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;LifetimeOperation census={0};
    XrXirStatus status=lifetime_operation(&context,fixture,mode,SIZE_MAX,false,&census);
    if (status!=XR_XIR_OK) fprintf(stderr,"LIFETIME_OPERATION mode%u status%u calls%u sites%zu work%llu\n",mode,status,
        census.calls,census.sites,(unsigned long long)rp_stats(&context).work);
    CHECK(status==XR_XIR_OK && census.sites);XrCompileResourceStats required=rp_stats(&context);
    printf("Lifetime mode%u refresh_sites=%zu work=%llu peak=%llu\n",mode,census.sites,
        (unsigned long long)required.work,(unsigned long long)required.peak_bytes);
    rp_owner_free(&context,baseline);rp_balanced(physical);
    for (size_t i=0;i<census.sites;++i) {
        if (!all && i && i+1!=census.sites) continue;
        context=rp_owner(rp_caps());baseline=rp_stats(&context).live_bytes;LifetimeOperation failed={0};
        CHECK(lifetime_operation(&context,fixture,mode,i,mode==0 && i+1==census.sites,&failed)==XR_XIR_OUT_OF_MEMORY && failed.injected);
        if (mode==1) CHECK(!failed.retired);
        XrCompileResourceStats paid=rp_stats(&context);CHECK(paid.live_bytes==baseline);
        CHECK(lifetime_operation(&context,fixture,mode,SIZE_MAX,false,NULL)==XR_XIR_OK);
        CHECK(rp_stats(&context).allocated_bytes>paid.allocated_bytes && rp_stats(&context).work>paid.work);
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    for (uint32_t axis=0;axis<3;++axis) for (uint32_t less=0;less<2;++less) {
        XrCompileResourceLimits caps=rp_caps();
        if (!axis) caps.allocated_bytes=required.allocated_bytes-less;
        else if (axis==1) caps.live_bytes=required.peak_bytes-less;else caps.work=required.work-less;
        context=rp_owner(caps);baseline=rp_stats(&context).live_bytes;
        CHECK(lifetime_operation(&context,fixture,mode,SIZE_MAX,false,NULL)==(less?XR_XIR_BUDGET:XR_XIR_OK));
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    printf("Lifetime mode%u injection=%s certificate retained same-owner retry exact/minus-one allocated/live/work physical0/0 PASS\n",
        mode,all?"ALL_REFRESH_SITES":"FIRST_LAST_ONLY");
    return census.sites;
}
int main(int argc,char **argv) {
    bool all=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || all);
    RootParameterMark physical=rp_mark();LifetimeFixture fixture;lifetime_fixture(&fixture);
    size_t sites=lifetime_resources(&fixture,0,all);lifetime_resources(&fixture,1,all);
    XrXirCompileContext receiver=rp_owner(rp_caps());uint64_t baseline=rp_stats(&receiver).live_bytes;
    LifetimeOperation escaped={.keep=true};
    const XrXirModule *source=xr_xir_compile_artifact_module(fixture.result.checked);
    XrXirInstruction expected=source->functions[fixture.function].instructions[fixture.instruction];
    expected.type=fixture.wide;
    CHECK(lifetime_operation(&receiver,&fixture,0,sites-1,true,&escaped)==XR_XIR_OUT_OF_MEMORY &&
        escaped.injected && escaped.retired && escaped.retained && escaped.certificate_intact);
    xr_xir_compile_source_result_free(&fixture.result);rp_owner_free(&fixture.context,fixture.baseline);
    EffectBodyRequest request=effect_body_owned_request(&receiver,escaped.retained);
    XrXirInstruction storage={0};const XrXirInstruction *operation=NULL;
    CHECK(effect_body_request_instruction(&request,fixture.function,fixture.instruction,&storage,&operation)==XR_XIR_OK &&
        operation->op==expected.op && operation->type==expected.type && operation->immediate==expected.immediate &&
        operation->args[0]==expected.args[0] && operation->args[1]==expected.args[1]);
    EffectCellBodyAdapter adapter={0};XrXirCellProofBodyRead read={0};
    CHECK(effect_cell_body_owned(&receiver,escaped.retained,&adapter,&read)==XR_XIR_OK &&
        xr_xir_compile_cell_provenance_body_matches(&receiver,escaped.retained->cells,&read,NULL)==XR_XIR_OK);
    effect_body_material_free(escaped.retained);rp_owner_free(&receiver,baseline);rp_balanced(physical);
    puts("Retained material after refresh OOM, enclosing owner cleanup and Source producer death physical0/0 PASS");
    puts("Stale refinement retirement and independent certificate lifetime passed");return 0;
}
