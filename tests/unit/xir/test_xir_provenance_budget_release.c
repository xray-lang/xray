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
    f->source=(XrXirSourceModule){"owner",5,NULL,0,0, 0,XR_XIR_MODULE_FRESH_STORAGE};
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
    XrXirSourceModule identity={"zero-cleanup",12,NULL,0,4, 0,XR_XIR_MODULE_FRESH_STORAGE};
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
/* Graph density, key length and operation boundary are fixed explicitly.
 * The private pass and the original raw public correspondence are measured
 * separately; neither is a Source/cleanup whole-compiler speedup claim. */
static void origin_index_scales(void) {
    XrXirFunction functions[512],received[512];XrXirOrigin origins[512];
    XrXirFunctionIdentity identities[512]={0};
    ProvenanceFixture f;fixture(&f);
    XrXirInstruction entry[2]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7},
        {.op=XR_XIR_RETURN,.args={0,0}}};XrXirBlock two={.count=2};
    for (uint32_t n=128;n<=512;n*=2) {
        for (uint32_t at=0;at<n;++at) functions[at]=f.function;
        functions[0]=(XrXirFunction){.name="entry",.name_length=5,.result=XR_XIR_I64,
            .instructions=entry,.instruction_count=2,.blocks=&two,.block_count=1};
        for (uint32_t at=0;at<n;++at) {
            origins[at]=(XrXirOrigin){.function=n-at-1};received[at]=functions[n-at-1];
        }
        XrXirSourceModule source_module=f.source,destination_module=f.source;
        source_module.initializer=n-1;destination_module.initializer=0;
        XrXirDeclarations source_decl=f.declarations,destination_decl=f.declarations;
        source_decl.modules=&source_module;destination_decl.modules=&destination_module;
        source_decl.functions=destination_decl.functions=identities;
        source_decl.root_module=destination_decl.root_module=0;
        source_decl.entry_function=0;destination_decl.entry_function=n-1;
        XrXirModule source=f.module,destination=f.module;
        source.functions=functions;source.function_count=n;source.declarations=&source_decl;source.linkage_kind=XR_XIR_PROGRAM;
        destination.functions=received;destination.function_count=n;destination.declarations=&destination_decl;
        destination.linkage_kind=XR_XIR_PROGRAM;
        XrXirProvenance evidence={.kind=XR_XIR_EVIDENCE_INSTANCE,.origins=origins,.count=n};
        XrXirCompileContext c=owner_new(4000000);size_t physical=live;uint64_t bytes=live_bytes;
        ProvenanceMatch match={.source=&source,.destination=&destination,.origins=origins,.count=n,.remaining=&c};
        XrCompileResourceStats before=stats(&c);size_t start=attempts;
        CHECK(provenance_reachable(&match)==XR_XIR_OK);
        XrCompileResourceStats after=stats(&c);
        printf("ORIGIN_SCALE N=%u edges=0 key_words=4 key_elements=0 phase=reachable work=%llu allocated=%llu peak=%llu sites=%zu old_header_pairs=%llu\n",
            n,(unsigned long long)(after.work-before.work),(unsigned long long)(after.allocated_bytes-before.allocated_bytes),
            (unsigned long long)after.peak_bytes,attempts-start,(unsigned long long)n*(n-1)/2);
        CHECK(after.live_bytes==before.live_bytes);before=after;start=attempts;
        CHECK(xr_xir_compile_provenance_functions_match(&c,&source,&destination,&evidence,NULL)==XR_XIR_OK);
        after=stats(&c);
        printf("ORIGIN_SCALE N=%u edges=0 key_words=4 key_elements=0 phase=public-functions work=%llu allocated=%llu peak=%llu sites=%zu\n",
            n,(unsigned long long)(after.work-before.work),(unsigned long long)(after.allocated_bytes-before.allocated_bytes),
            (unsigned long long)after.peak_bytes,attempts-start);
        CHECK(after.live_bytes==before.live_bytes);owner_free(&c,physical,bytes);
    }
}

typedef struct OriginIndexFixture {
    ProvenanceFixture base;
    XrXirFunction source_functions[2],destination_functions[8];
    XrXirGeneric generics[2];XrXirConstraint constraints[2];
    XrXirFunctionIdentity source_identities[2],destination_identities[8];
    XrXirDeclarations source_declarations,destination_declarations;
    XrXirModule source,destination;
    XrXirOrigin origins[8];XrXirType arguments[8][2];
    XrXirEffectArgument effects[8][2];
} OriginIndexFixture;

/* These literal keys exercise equality, not compiler admission. Distinct
 * pointer storage and common prefixes cannot replace complete field equality.
 * Existing public correspondence and constraints controls remain independent. */
static void origin_index_fixture(OriginIndexFixture *f) {
    memset(f,0,sizeof(*f));fixture(&f->base);
    for (uint32_t i=0;i<2;++i) {
        f->source_functions[i]=f->base.function;
        f->generics[i]=(XrXirGeneric){.parameter_count=2,.constraints=f->constraints};
    }
    for (uint32_t i=0;i<8;++i) {
        f->destination_functions[i]=f->base.function;
        f->arguments[i][0]=f->arguments[i][1]=XR_XIR_I64;
        f->effects[i][0]=(XrXirEffectArgument){.parameter=0,.type=XR_XIR_I64};
        f->effects[i][1]=(XrXirEffectArgument){.parameter=1,.type=XR_XIR_F64};
        f->origins[i]=(XrXirOrigin){.function=0,.arguments=f->arguments[i],.argument_count=2,
            .effect_arguments=f->effects[i],.effect_argument_count=2};
    }
    f->arguments[1][1]=XR_XIR_F64;
    f->effects[2][1].type=XR_XIR_I64;
    f->effects[3][1].parameter=0;
    f->destination_identities[4].cleanup_owner=1;
    f->origins[5].function=1;
    f->arguments[6][0]=f->arguments[6][1]=XR_XIR_U64;
    f->origins[7].effect_argument_count=1;
    f->source_declarations=f->destination_declarations=f->base.declarations;
    f->source_declarations.functions=f->source_identities;
    f->destination_declarations.functions=f->destination_identities;
    f->source=f->destination=f->base.module;
    f->source.functions=f->source_functions;f->source.function_count=2;f->source.generics=f->generics;
    f->source.declarations=&f->source_declarations;
    f->destination.functions=f->destination_functions;f->destination.function_count=8;
    f->destination.declarations=&f->destination_declarations;
}

static uint32_t origin_index_reference(const OriginIndexFixture *f) {
    for (uint32_t at=0;at<8;++at) for (uint32_t prior=0;prior<at;++prior) {
        const XrXirOrigin *a=&f->origins[at],*b=&f->origins[prior];
        if (a->function!=b->function || a->argument_count!=b->argument_count ||
            a->effect_argument_count!=b->effect_argument_count ||
            f->destination_identities[at].cleanup_owner!=f->destination_identities[prior].cleanup_owner)
            continue;
        bool same=true;
        for (uint32_t v=0;v<a->argument_count;++v) if (a->arguments[v]!=b->arguments[v]) same=false;
        for (uint32_t v=0;v<a->effect_argument_count;++v)
            if (a->effect_arguments[v].parameter!=b->effect_arguments[v].parameter ||
                a->effect_arguments[v].type!=b->effect_arguments[v].type) same=false;
        if (same) return at;
    }
    return UINT32_MAX;
}


static XrXirStatus origin_hash_pass(ProvenanceMatch *match,uint32_t *index,uint32_t *rejected) {
    if (!xir_compile_work(match->remaining,(uint64_t)match->count*sizeof(*index))) return XR_XIR_BUDGET;
    memset(index,0xff,(size_t)match->count*sizeof(*index));
    for (uint32_t f=0;f<match->count;++f) {
        XrXirStatus status=provenance_origin_insert(match,index,f);
        if (status!=XR_XIR_OK) { *rejected=f;return status; }
    }
    return XR_XIR_OK;
}

static void origin_hash_keys(void) {
    OriginIndexFixture f;origin_index_fixture(&f);
    XrXirCompileContext c=owner_new(1000000);size_t physical=live;uint64_t bytes=live_bytes;
    ProvenanceMatch match={.source=&f.source,.destination=&f.destination,.origins=f.origins,.count=8,.remaining=&c};
    uint32_t index[8],rejected=UINT32_MAX;size_t held_attempts=attempts;
    CHECK(origin_index_reference(&f)==UINT32_MAX && origin_hash_pass(&match,index,&rejected)==XR_XIR_OK);
    CHECK(attempts==held_attempts && stats(&c).live_bytes==bytes);
    /* Two separately owned equal vectors reject at the first discovery
     * ordinal; another declaration's later equal pair cannot change it. */
    f.origins[2].function=1;memcpy(f.arguments[2],f.arguments[5],sizeof(f.arguments[2]));
    memcpy(f.effects[2],f.effects[5],sizeof(f.effects[2]));
    memcpy(f.arguments[3],f.arguments[1],sizeof(f.arguments[3]));
    memcpy(f.effects[3],f.effects[1],sizeof(f.effects[3]));
    CHECK(origin_index_reference(&f)==3 && origin_hash_pass(&match,index,&rejected)==XR_XIR_BAD_STRUCTURE && rejected==3);
    origin_index_fixture(&f);uint32_t hash=41;
    f.origins[6].arguments=NULL;
    CHECK(provenance_origin_hash(&match,6,&hash)==XR_XIR_BAD_STRUCTURE && hash==41);
    f.origins[6].arguments=f.arguments[6];f.origins[6].effect_arguments=NULL;
    CHECK(provenance_origin_hash(&match,6,&hash)==XR_XIR_BAD_STRUCTURE && hash==41);
    f.origins[6].effect_arguments=f.effects[6];f.origins[6].function=UINT32_MAX;
    CHECK(provenance_origin_hash(&match,6,&hash)==XR_XIR_BAD_STRUCTURE && hash==41);
    f.origins[6].function=0;rejected=UINT32_MAX;
    CHECK(origin_hash_pass(&match,index,&rejected)==XR_XIR_OK);
    owner_free(&c,physical,bytes);
}

static void origin_hash_collisions(void) {
    OriginIndexFixture f;origin_index_fixture(&f);
    XrXirCompileContext c=owner_new(1000000);size_t physical=live;uint64_t bytes=live_bytes;
    ProvenanceMatch match={.source=&f.source,.destination=&f.destination,.origins=f.origins,.count=8,.remaining=&c};
    const XrXirType scalars[]={XR_XIR_I8,XR_XIR_I16,XR_XIR_I32,XR_XIR_I64,
        XR_XIR_U8,XR_XIR_U16,XR_XIR_U32,XR_XIR_U64};
    uint32_t found=0,bucket=UINT32_MAX;
    /* The production hash selects collision inputs only. The independent
     * full-field reference, never that hash, decides their equality. */
    for (uint32_t a=0;a<8 && found<8;++a) for (uint32_t b=0;b<8 && found<8;++b) {
        f.arguments[found][0]=scalars[a];f.arguments[found][1]=scalars[b];
        f.effects[found][0]=(XrXirEffectArgument){.parameter=0,.type=XR_XIR_I64};
        f.effects[found][1]=(XrXirEffectArgument){.parameter=1,.type=XR_XIR_F64};
        f.origins[found]=(XrXirOrigin){.function=0,.arguments=f.arguments[found],.argument_count=2,
            .effect_arguments=f.effects[found],.effect_argument_count=2};
        f.destination_identities[found].cleanup_owner=0;uint32_t hash=0;
        CHECK(provenance_origin_hash(&match,found,&hash)==XR_XIR_OK);
        if (bucket==UINT32_MAX) bucket=hash%8;
        if (hash%8==bucket) ++found;
    }
    CHECK(found==8 && origin_index_reference(&f)==UINT32_MAX);
    uint32_t index[8],rejected=UINT32_MAX;
    CHECK(origin_hash_pass(&match,index,&rejected)==XR_XIR_OK);
    for (uint32_t i=0;i<8;++i) CHECK(index[i]<8);
    memcpy(f.arguments[7],f.arguments[0],sizeof(f.arguments[7]));
    CHECK(origin_index_reference(&f)==7 && origin_hash_pass(&match,index,&rejected)==XR_XIR_BAD_STRUCTURE && rejected==7);
    owner_free(&c,physical,bytes);
}

static uint64_t origin_hash_work_case(uint64_t cap,XrXirStatus expected) {
    OriginIndexFixture f;origin_index_fixture(&f);
    XrXirCompileContext c=owner_new(cap);size_t physical=live;uint64_t bytes=live_bytes;
    ProvenanceMatch match={.source=&f.source,.destination=&f.destination,.origins=f.origins,.count=8,.remaining=&c};
    uint32_t index[8],rejected=UINT32_MAX;uint64_t before=stats(&c).work;
    CHECK(origin_hash_pass(&match,index,&rejected)==expected);
    uint64_t used=stats(&c).work-before;CHECK(used<=cap);owner_free(&c,physical,bytes);return used;
}

static void origin_hash_resources(void) {
    uint64_t used=origin_hash_work_case(1000000,XR_XIR_OK);CHECK(used);
    CHECK(origin_hash_work_case(1+used,XR_XIR_OK)==used);
    for (uint64_t cap=1;cap<1+used;++cap) origin_hash_work_case(cap,XR_XIR_BUDGET);
    ProvenanceFixture f;fixture(&f);XrXirCompileContext c=owner_new(100000);
    size_t physical=live;uint64_t bytes=live_bytes;
    ProvenanceMatch match={.source=&f.module,.destination=&f.module,.origins=&f.origin,.count=1,.remaining=&c};
    attempts=0;fail_at=0;injected=false;
    CHECK(provenance_reachable(&match)==XR_XIR_OUT_OF_MEMORY && injected && live==physical && live_bytes==bytes);
    fail_at=SIZE_MAX;injected=false;uint64_t failed=stats(&c).work;
    CHECK(provenance_reachable(&match)==XR_XIR_OK && attempts==2 && stats(&c).work>failed);
    owner_free(&c,physical,bytes);
    c=owner_new(100000);physical=live;bytes=live_bytes;match.remaining=&c;
    CHECK(provenance_reachable(&match)==XR_XIR_OK);XrCompileResourceStats required=stats(&c);
    owner_free(&c,physical,bytes);
    for (unsigned axis=0;axis<3;++axis) for (unsigned less=0;less<2;++less) {
        XrCompileResourceLimits limits={65536,32768,100000};
        uint64_t exact=axis==0?required.allocated_bytes:axis==1?required.peak_bytes:required.work;
        uint64_t *limit=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;CHECK(exact);*limit=exact-less;
        CHECK(xr_compile_resources_new(&limits,&c.resources)==XR_COMPILE_RESOURCE_OK);
        c.limits=xr_xir_compile_default_limits();physical=live;bytes=live_bytes;match.remaining=&c;
        CHECK(provenance_reachable(&match)==(less?XR_XIR_BUDGET:XR_XIR_OK));owner_free(&c,physical,bytes);
    }
    printf("origin hash complete keys, full eight-slot collisions, workcuts=%llu and sameowner/allaxes physical0 PASS\n",
        (unsigned long long)used);
}

int main(void) {
    ProvenanceFixture f;fixture(&f);ample_control(&f);precise_rejection(&f);public_work_cuts(&f);
    scope_owner_control(&f);scope_generic_owner_control();
    origin_hash_keys();origin_hash_collisions();origin_hash_resources();origin_index_scales();
    puts("provenance queue owner released on second work rejection; physical=0/0 PASS");return 0;
}
