/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_provenance_budget_release.c - Release the owned reachability queue
 *
 * KEY CONCEPT:
 *   A fresh ledger rejects the second work charge after queue allocation.
 *   The interior seen view never becomes a separately releasable owner.
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_declarations.h"
#include "xir/xxir_effect_contract_internal.h"
#include "xir/xxir_internal.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1); } } while (0)
typedef struct Physical { void *pointer; size_t bytes; } Physical;
static Physical provenance_blocks[1024];
static size_t live, live_bytes, attempts, fail_at=SIZE_MAX;
static bool injected;
static void *provenance_malloc(size_t bytes) {
    if (attempts++==fail_at) { injected=true; return NULL; }
    void *pointer=xr_malloc(bytes); if (!pointer) return NULL;
    CHECK(live<1024 && bytes<=SIZE_MAX-live_bytes);
    provenance_blocks[live++]=(Physical){pointer,bytes};live_bytes+=bytes;return pointer;
}
static void provenance_free(void *pointer) {
    if (!pointer) return;
    size_t i=0;while (i<live && provenance_blocks[i].pointer!=pointer) ++i;
    CHECK(i<live && live_bytes>=provenance_blocks[i].bytes);
    live_bytes-=provenance_blocks[i].bytes;provenance_blocks[i]=provenance_blocks[--live];xr_free(pointer);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) provenance_malloc(bytes)
#define xr_free(pointer) provenance_free(pointer)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")

/* Canonical production definitions give this TU the private reachability entry. */
#include "xir/xxir.c"
typedef struct ProvenanceFixture {
    XrXirInstruction instruction;
    XrXirBlock block;
    XrXirFunction function;
    XrXirSourceModule source;
    XrXirFunctionIdentity identity;
    XrXirDeclarations declarations;
    XrXirModule module;
    XrXirOrigin origin;
} ProvenanceFixture;
static void fixture(ProvenanceFixture *f) {
    memset(f,0,sizeof(*f));f->instruction=(XrXirInstruction){.op=XR_XIR_RETURN,.type=XR_XIR_UNIT};
    f->block=(XrXirBlock){.first=0,.count=1};
    f->function=(XrXirFunction){.name="init",.name_length=4,.result=XR_XIR_UNIT,.blocks=&f->block,.block_count=1,.instructions=&f->instruction,.instruction_count=1};
    f->source=(XrXirSourceModule){"owner",5,NULL,0,0};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=&f->identity,.root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    f->module=(XrXirModule){.stage=XR_XIR_CHECKED,.functions=&f->function,.function_count=1,.declarations=&f->declarations,.linkage_kind=XR_XIR_LIBRARY};
    f->origin=(XrXirOrigin){.function=0,.argument_count=0,.arguments=NULL};
}
static XrCompileResourceStats stats(const XrXirCompileContext *c) {
    XrCompileResourceStats s={0};CHECK(xr_compile_resources_stats(c->resources,&s)==XR_COMPILE_RESOURCE_OK);return s;
}
static XrXirCompileContext owner_new(uint64_t work) {
    CHECK(!live && !live_bytes);XrXirCompileContext c={0};
    const XrCompileResourceLimits limits={65536,32768,work};
    CHECK(xr_compile_resources_new(&limits,&c.resources)==XR_COMPILE_RESOURCE_OK);
    c.limits=xr_xir_compile_default_limits();CHECK(stats(&c).work==1);return c;
}
static void owner_free(XrXirCompileContext *c,size_t physical,uint64_t bytes) {
    CHECK(live==physical && live_bytes==bytes && stats(c).live_bytes==bytes);
    xr_compile_resources_release(c->resources);*c=(XrXirCompileContext){0};CHECK(!live && !live_bytes);
}
static void ample_control(ProvenanceFixture *f) {
    XrXirCompileContext c=owner_new(100000);size_t physical=live;uint64_t bytes=live_bytes;
    CHECK(xir_fixture_verify(&c, &f->module, NULL)==XR_XIR_OK);
    ProvenanceMatch match={.source=&f->module,.destination=&f->module,.origins=&f->origin,.count=1,.remaining=&c};
    attempts=0;CHECK(provenance_reachable(&match)==XR_XIR_OK && attempts==1);
    owner_free(&c,physical,bytes);
}
static void precise_rejection(ProvenanceFixture *f) {
    /* New ledger initialization uses one unit. Three remaining units allow
     * count+source_count=2, then allocation=1, but reject the next two units. */
    XrXirCompileContext c=owner_new(4);size_t physical=live;uint64_t bytes=live_bytes;
    ProvenanceMatch match={.source=&f->module,.destination=&f->module,.origins=&f->origin,.count=1,.remaining=&c};
    attempts=0;CHECK(provenance_reachable(&match)==XR_XIR_BUDGET && attempts==1);
    CHECK(stats(&c).work==4);owner_free(&c,physical,bytes);
}
static void public_work_cuts(ProvenanceFixture *f) {
    /* Public correspondence currently expects an ordinary Program entry. */
    XrXirInstruction entry_ops[]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7},
        {.op=XR_XIR_RETURN,.type=XR_XIR_UNIT,.args={0}}};
    XrXirBlock entry_block={.first=0,.count=2};
    XrXirFunction functions[2]={f->function,{.name="entry",.name_length=5,.result=XR_XIR_I64,
        .blocks=&entry_block,.block_count=1,.instructions=entry_ops,.instruction_count=2}};
    XrXirFunctionIdentity identities[2]={{0}};
    XrXirDeclarations declarations=f->declarations;declarations.functions=identities;
    declarations.root_module=0;declarations.entry_function=1;
    XrXirModule module=f->module;module.functions=functions;module.function_count=2;
    module.declarations=&declarations;module.linkage_kind=XR_XIR_PROGRAM;
    XrXirOrigin origins[2]={{.function=0},{.function=1}};
    /* This borrowed metadata wrapper owns no storage; the ordinary graph is
     * independently verified below before its complete correspondence proof. */
    XrXirArtifact source={.module=module};
    XrXirProvenance instance={.kind=XR_XIR_EVIDENCE_INSTANCE,
        .source=&source,.origins=origins,.count=2};
    XrXirCompileContext c=owner_new(100000);size_t physical=live;uint64_t bytes=live_bytes;
    CHECK(xir_fixture_verify(&c, &module, NULL)==XR_XIR_OK);owner_free(&c,physical,bytes);
    c=owner_new(100000);physical=live;bytes=live_bytes;source.context=c;
    CHECK(xr_xir_compile_provenance_functions_match(&c,&module,&module,&instance,NULL)==XR_XIR_OK);
    XrCompileResourceStats required=stats(&c);
    uint64_t exact=required.work;CHECK(exact>4);owner_free(&c,physical,bytes);
    for(uint64_t work=1;work<=exact;++work) {
        c=owner_new(work);physical=live;bytes=live_bytes;source.context=c;
        XrXirStatus status=xr_xir_compile_provenance_functions_match(&c,&module,&module,&instance,NULL);
        CHECK(status==(work==exact?XR_XIR_OK:XR_XIR_BUDGET));owner_free(&c,physical,bytes);
    }
    printf("public provenance census: allocated=%llu peak=%llu work=%llu\n",
        (unsigned long long)required.allocated_bytes,(unsigned long long)required.peak_bytes,
        (unsigned long long)required.work);
    printf("public provenance complete work cuts=%llu each physical=0/0\n",(unsigned long long)exact);
}
typedef struct ScopeFailureControl {
    const XrXirCompileContext *context;
    XirEffectContextInput input;
    uint32_t mode, calls;
    uint64_t limit;
    XrXirStatus expected;
} ScopeFailureControl;

/* Intentionally ignore the first failed selection in the callback result.
 * The operation itself must retain the error and stop subsequent requests. */
static XrXirStatus scope_ignored_failure(void *data,XirEffectContextScope *scope) {
    ScopeFailureControl *control=data;++control->calls;
    XirEffectContextInput rejected=control->input;
    XrXirOrigin invalid={.function=UINT32_MAX};
    if (control->mode==0) rejected.origin=&invalid;
    if (control->mode==1) rejected.instruction=0;
    if (control->mode==3) {
        uint64_t current=stats(control->context).work;
        CHECK(current<=control->limit);
        CHECK(xir_compile_work(control->context,control->limit-current));
    }
    XirEffectContextView output={.declaration=UINT32_MAX},sentinel;
    memcpy(&sentinel,&output,sizeof(output));
    CHECK(xir_effects_context_select_scoped(scope,&rejected,&output)==control->expected);
    CHECK(!memcmp(&output,&sentinel,sizeof(output)));
    XrCompileResourceStats cut=stats(control->context);
    size_t physical_attempts=attempts,physical_live=live,physical_bytes=live_bytes;
    CHECK(xir_effects_context_select_scoped(scope,&control->input,&output)==control->expected);
    CHECK(xir_effects_context_select_scoped(scope,NULL,NULL)==control->expected);
    XrCompileResourceStats later=stats(control->context);
    CHECK(cut.work==later.work && cut.allocated_bytes==later.allocated_bytes &&
        cut.live_bytes==later.live_bytes && cut.allocation_count==later.allocation_count);
    CHECK(attempts==physical_attempts && live==physical_live && live_bytes==physical_bytes);
    CHECK(!memcmp(&output,&sentinel,sizeof(output)));
    return XR_XIR_OK;
}

static XrXirStatus scope_valid_control(void *data,XirEffectContextScope *scope) {
    ScopeFailureControl *control=data;++control->calls;
    for (uint32_t query=0;query<2;++query) {
        XirEffectContextView selected={0};
        XrXirStatus status=xir_effects_context_select_scoped(scope,&control->input,&selected);
        if (status!=XR_XIR_OK) return status;
        CHECK(selected.declaration==1 && selected.function && selected.function->result==XR_XIR_I64 &&
            !selected.parameter_count && !selected.argument_count && !selected.requires_root && !selected.unresolved);
    }
    return XR_XIR_OK;
}

static void scope_owner_control(ProvenanceFixture *f) {
    CHECK(!live && !live_bytes);
    const XrCompileResourceLimits limits={8388608,4194304,4000000};
    XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();
    size_t physical=live;uint64_t bytes=live_bytes;
    XrXirInstruction entry_ops[]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7},
        {.op=XR_XIR_RETURN,.type=XR_XIR_UNIT,.args={0}}};
    XrXirBlock entry_block={.first=0,.count=2};
    XrXirFunction functions[2]={f->function,{.name="entry",.name_length=5,.result=XR_XIR_I64,
        .blocks=&entry_block,.block_count=1,.instructions=entry_ops,.instruction_count=2}};
    XrXirFunctionIdentity identities[2]={{0}};
    XrXirDeclarations declarations=f->declarations;declarations.functions=identities;
    declarations.root_module=0;declarations.entry_function=1;
    /* ROOT belongs to the initializer; the scalar literal entry has no latent
     * callable, Cell or unknown input. The complete checker proves both facts. */
    XrXirFunctionEffectContract contracts[2]={0};
    contracts[0].formula.constant_mask=XR_XIR_CALLABLE_ROOT_REQUIRED;
    XrXirProvenance evidence={.kind=XR_XIR_EVIDENCE_TEMPLATE,.contracts=contracts,.contract_count=2};
    XrXirModule module=f->module;module.functions=functions;module.function_count=2;
    module.declarations=&declarations;module.linkage_kind=XR_XIR_PROGRAM;module.provenance=&evidence;
    XrXirConstruction *construction=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_construction(&context,module.types,&construction)==XR_XIR_OK);
    CHECK(xr_xir_compile_verify_owned_effects_v2(&context,&module,construction,&effects,NULL)==XR_XIR_OK);
    xr_xir_compile_construction_free(construction);
    CHECK(effects && xir_effects_context_available(&context,effects));
    XrXirOrigin origin={.function=1};
    ScopeFailureControl control={.context=&context,
        .input={&module,module.types,&origin,UINT32_MAX,NULL,0},.limit=limits.work};
    CHECK(xir_effects_context_operation(&context,effects,&module,module.types,
        scope_valid_control,&control)==XR_XIR_OK && control.calls==1);
    for (uint32_t mode=0;mode<3;++mode) {
        control.mode=mode;control.calls=0;control.expected=XR_XIR_BAD_STRUCTURE;
        if (mode==2) ++entry_ops[0].immediate;
        CHECK(xir_effects_context_operation(&context,effects,&module,module.types,
            scope_ignored_failure,&control)==XR_XIR_BAD_STRUCTURE && control.calls==1);
        if (mode==2) --entry_ops[0].immediate;
        control.calls=0;
        CHECK(xir_effects_context_operation(&context,effects,&module,module.types,
            scope_valid_control,&control)==XR_XIR_OK && control.calls==1);
    }
    control.mode=3;control.calls=0;control.expected=XR_XIR_BUDGET;
    CHECK(xir_effects_context_operation(&context,effects,&module,module.types,
        scope_ignored_failure,&control)==XR_XIR_BUDGET && control.calls==1);
    CHECK(stats(&context).work==limits.work);
    xr_xir_compile_effects_free(effects);owner_free(&context,physical,bytes);
    puts("private provenance scope ignores no failure; repeated rejected queries charge zero and preserve output; physical=0/0 PASS");
}
typedef struct ScopeGenericOwnerControl {
    const XrXirCompileContext *context;
    XirEffectContextInput input;
    XrXirOrigin *parent;
    const XrXirType *arguments;
    uint32_t calls;
} ScopeGenericOwnerControl;
static XrXirStatus scope_generic_ignored_owner(void *data,XirEffectContextScope *scope) {
    ScopeGenericOwnerControl *control=data;++control->calls;
    XirEffectContextView output={.declaration=UINT32_MAX},sentinel;
    memcpy(&sentinel,&output,sizeof(output));
    CHECK(control->parent->argument_count==1 && !control->parent->arguments);
    CHECK(xir_effects_context_select_scoped(scope,&control->input,&output)==XR_XIR_BAD_STRUCTURE);
    CHECK(!memcmp(&output,&sentinel,sizeof(output)));
    XrCompileResourceStats before=stats(control->context);size_t physical=attempts;
    control->parent->arguments=control->arguments;
    CHECK(xir_effects_context_select_scoped(scope,&control->input,&output)==XR_XIR_BAD_STRUCTURE);
    CHECK(xir_effects_context_select_scoped(scope,NULL,NULL)==XR_XIR_BAD_STRUCTURE);
    XrCompileResourceStats after=stats(control->context);
    CHECK(before.work==after.work && before.allocated_bytes==after.allocated_bytes &&
        before.live_bytes==after.live_bytes && before.allocation_count==after.allocation_count && attempts==physical);
    CHECK(!memcmp(&output,&sentinel,sizeof(output)));return XR_XIR_OK;
}
static XrXirStatus scope_generic_valid_owner(void *data,XirEffectContextScope *scope) {
    ScopeGenericOwnerControl *control=data;++control->calls;
    XirEffectContextView selected={0};
    XrXirStatus status=xir_effects_context_select_scoped(scope,&control->input,&selected);
    if (status==XR_XIR_OK) CHECK(selected.declaration==2 && selected.argument_count==1 &&
        !selected.parameter_count && !selected.requires_root && !selected.unresolved);
    return status;
}
static void scope_generic_owner_control(void) {
    CHECK(!live && !live_bytes);
    const XrCompileResourceLimits limits={8388608,4194304,4000000};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();size_t physical=live;uint64_t bytes=live_bytes;
    /* Two real generic REGISTER levels preserve their complete lexical vector.
     * The I64 and F64 calls create distinct concrete owner identities. */
    XrXirInstruction outer[2]={{.op=XR_XIR_CLEANUP_REGISTER,.immediate=1,
        .targets={1,0},.type_arguments={0,1}},{.op=XR_XIR_RETURN}};
    XrXirInstruction child[2]={{.op=XR_XIR_CLEANUP_REGISTER,.immediate=2,
        .targets={1,0},.type_arguments={0,1}},{.op=XR_XIR_RETURN}};
    XrXirInstruction done[2]={{.op=XR_XIR_RETURN},{.op=XR_XIR_RETURN}};
    XrXirInstruction entry[4]={{.op=XR_XIR_CALL,.immediate=0,.type_arguments={0,1}},
        {.op=XR_XIR_CALL,.immediate=0,.type_arguments={1,1}},
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64},{.op=XR_XIR_RETURN,.args={2,0}}};
    XrXirBlock split[2]={{.count=1},{.first=1,.count=1,.frontier=1}},one={.count=1},four={.count=4};
    XrXirFunction functions[5]={
        {.name="outer",.name_length=5,.blocks=split,.block_count=2,.instructions=outer,.instruction_count=2},
        {.name="child",.name_length=5,.blocks=split,.block_count=2,.instructions=child,.instruction_count=2},
        {.name="nested",.name_length=6,.blocks=&one,.block_count=1,.instructions=&done[0],.instruction_count=1},
        {.name="entry",.name_length=5,.result=XR_XIR_I64,.blocks=&four,.block_count=1,
            .instructions=entry,.instruction_count=4},
        {.name="init",.name_length=4,.blocks=&one,.block_count=1,.instructions=&done[1],.instruction_count=1}};
    XrXirType binder=XR_XIR_TYPE_PARAMETER_BASE,ordinary[2]={XR_XIR_I64,XR_XIR_F64};
    XrXirConstraint constraints[3]={0};
    XrXirGeneric generics[5]={
        {.constraints=&constraints[0],.parameter_count=1,.arguments=&binder,.argument_count=1},
        {.constraints=&constraints[1],.parameter_count=1,.arguments=&binder,.argument_count=1},
        {.constraints=&constraints[2],.parameter_count=1},
        {.arguments=ordinary,.argument_count=2},{0}};
    XrXirFunctionIdentity identities[5]={0};identities[1].cleanup_owner=1;identities[2].cleanup_owner=2;
    XrXirSourceModule identity={"zero-cleanup",12,NULL,0,4};
    XrXirDeclarations declarations={.modules=&identity,.module_count=1,.functions=identities,.entry_function=3};
    XrXirFunctionEffectContract contracts[5]={0};contracts[4].formula.constant_mask=XR_XIR_CALLABLE_ROOT_REQUIRED;
    XrXirProvenance evidence={.kind=XR_XIR_EVIDENCE_TEMPLATE,.contracts=contracts,.contract_count=5};
    XrXirModule module={.stage=XR_XIR_CHECKED,.functions=functions,.function_count=5,
        .declarations=&declarations,.generics=generics,.provenance=&evidence,.linkage_kind=XR_XIR_PROGRAM};
    XrXirConstruction *construction=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_construction(&context,module.types,&construction)==XR_XIR_OK);
    CHECK(xr_xir_compile_verify_owned_effects_v2(&context,&module,construction,&effects,NULL)==XR_XIR_OK);
    xr_xir_compile_construction_free(construction);
    CHECK(effects && xir_effects_context_available(&context,effects));
    XrXirOrigin nested={.function=2,.arguments=&ordinary[0],.argument_count=1};
    XrXirOrigin parents[2]={{.function=1,.arguments=&ordinary[0],.argument_count=1},
        {.function=0,.arguments=&ordinary[0],.argument_count=1}};
    XirEffectContextInput input={&module,module.types,&nested,UINT32_MAX,parents,2};
    XirEffectContextView selected={0};
    CHECK(xir_effects_context_select(&context,effects,&input,&selected)==XR_XIR_OK && selected.declaration==2);
    for (uint32_t owner=0;owner<2;++owner) {
        parents[owner].arguments=NULL;
        XirEffectContextView rejected={.declaration=UINT32_MAX},sentinel;memcpy(&sentinel,&rejected,sizeof(rejected));
        CHECK(xir_effects_context_select(&context,effects,&input,&rejected)==XR_XIR_BAD_STRUCTURE);
        CHECK(!memcmp(&rejected,&sentinel,sizeof(rejected)));
        ScopeGenericOwnerControl control={.context=&context,.input=input,.parent=&parents[owner],.arguments=&ordinary[0]};
        CHECK(xir_effects_context_operation(&context,effects,&module,module.types,
            scope_generic_ignored_owner,&control)==XR_XIR_BAD_STRUCTURE && control.calls==1);
        control.calls=0;
        CHECK(xir_effects_context_operation(&context,effects,&module,module.types,
            scope_generic_valid_owner,&control)==XR_XIR_OK && control.calls==1);
        CHECK(xir_effects_context_select(&context,effects,&input,&selected)==XR_XIR_OK);
    }
    nested.arguments=parents[0].arguments=parents[1].arguments=&ordinary[1];
    CHECK(xir_effects_context_select(&context,effects,&input,&selected)==XR_XIR_OK && selected.declaration==2);
    xr_xir_compile_effects_free(effects);owner_free(&context,physical,bytes);
    puts("raw and private scoped generic owner NULL vectors reject before access; physical=0/0 PASS");
}
int main(void) {
    ProvenanceFixture f;fixture(&f);ample_control(&f);precise_rejection(&f);public_work_cuts(&f);
    scope_owner_control(&f);scope_generic_owner_control();
    puts("provenance queue owner released on second work rejection; physical=0/0 PASS");return 0;
}
