/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_task_record_seed_controls.h - Owned field projections from genuine Source
 *
 * KEY CONCEPT:
 *   Real Source supplies the declarations. Physical effect allocations test
 *   failure recovery; projection metadata grants no worker permission.
 */
#include "xir/xxir_compile_memory.h"
static size_t record_seed_attempts,record_seed_fail=SIZE_MAX;
static bool record_seed_cancel,record_seed_cancel_enabled;
static void *record_seed_allocate(const XrXirCompileContext *context,size_t bytes,XrXirStatus *status) {
    if(record_seed_attempts++==record_seed_fail) {*status=XR_XIR_OUT_OF_MEMORY;return NULL;}
    void *memory=xir_compile_alloc(context,bytes,status);
    if(memory&&record_seed_cancel_enabled)record_seed_cancel=true;
    return memory;
}
#define xir_compile_alloc record_seed_allocate
#include "xir/xxir_effects.c"
#undef xir_compile_alloc
static bool record_seed_cancelled(const void *unused) {(void)unused;return record_seed_cancel;}
static const char record_seed_source[]=
    "const config={name:\"owned\",value:42}\nfn read()->i64{return config.value}\nscope{go read()}";
static void scope_record_seed_controls(const XrXirCompileContext *context) {
    uint64_t baseline=held(context->resources);XrXirSourceResult source={0};
    CHECK(check(context,record_seed_source,&source,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(source.checked);
    const XrXirTypes *types=module->types;XrXirType record=XR_XIR_UNIT;
    for(uint32_t n=0;n<types->count;++n) {
        const XrXirTypeNode *node=types->nodes+n;
        if(node->kind==XR_XIR_TYPE_NOMINAL&&!node->parameter_span&&node->nominal.field_count==2&&
            (types->nominals->declarations[node->nominal.declaration].flags&XR_XIR_NOMINAL_RECORD)) {
            record=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+n);break;
        }
    }
    CHECK(record!=XR_XIR_UNIT);uint32_t index=(uint32_t)record-XR_XIR_CONSTRUCTED_TYPE_BASE;
    const XrXirTypeNode *original=types->nodes+index;
    uint64_t live=held(context->resources);EffectTerms owner={.remaining=context};record_seed_attempts=0;
    CHECK(effect_terms_seed(&owner,types)==XR_XIR_OK);
    size_t allocations=record_seed_attempts;CHECK(allocations>0);
    CHECK(owner.types.nodes[index].nominal.fields!=original->nominal.fields&&
        owner.types.nodes[index].nominal.field_count==original->nominal.field_count&&
        !memcmp(owner.types.nodes[index].nominal.fields,original->nominal.fields,2*sizeof(XrXirType)));
    effect_terms_free(&owner);CHECK(held(context->resources)==live);
    for(size_t fail=0;fail<allocations;++fail) {
        owner=(EffectTerms){.remaining=context};record_seed_attempts=0;record_seed_fail=fail;
        CHECK(effect_terms_seed(&owner,types)==XR_XIR_OUT_OF_MEMORY);
        record_seed_fail=SIZE_MAX;effect_terms_free(&owner);CHECK(held(context->resources)==live);
    }
    XrXirTypeNode *nodes=xr_malloc((size_t)types->count*sizeof(*nodes));CHECK(nodes);
    memcpy(nodes,types->nodes,(size_t)types->count*sizeof(*nodes));XrXirType fields[2];
    memcpy(fields,original->nominal.fields,sizeof(fields));XrXirTypes changed=*types;changed.nodes=nodes;
    XrXirModule built=*module;built.stage=XR_XIR_BUILT;built.provenance=NULL;built.types=&changed;
    for(unsigned mode=0;mode<2;++mode) {
        nodes[index]=*original;
        if(!mode) {fields[0]=fields[0]==XR_XIR_I64?XR_XIR_STRING:XR_XIR_I64;nodes[index].nominal.fields=fields;}
        else {nodes[index].nominal.fields=NULL;nodes[index].nominal.field_count=0;}
        XrXirArtifact *output=NULL;XrXirDiagnostic issue={0};
        XrXirStatus status=xr_xir_compile_check_v2(context,&built,xr_xir_compile_artifact_construction(source.checked),&output,&issue);
        if(status!=XR_XIR_BAD_TYPE)fprintf(stderr,"record projection mode%u status%u f%u i%u\n",mode,status,issue.function,issue.instruction);
        CHECK(status==XR_XIR_BAD_TYPE&&!output&&held(context->resources)==live);
    }
    xr_free(nodes);owner=(EffectTerms){.remaining=context};CHECK(effect_terms_seed(&owner,types)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    const XirMetadataDomain domain={.types=&owner.types};
    XirMetadataProofContext proof={&domain,{XR_XIR_CONTEXT_CLOSED,0,0}};
    CHECK(xir_metadata_type_markers_prove(context,&proof,record,XR_XIR_CONSTRAINT_SENDABLE)==XR_XIR_OK);
    effect_terms_free(&owner);CHECK(held(context->resources)==baseline);
    XrCompileRequestOptions options={.cancel_poll=record_seed_cancelled};XrCompileRequestControl control={0};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    record_seed_cancel=false;record_seed_cancel_enabled=true;
    CHECK(check(context,record_seed_source,&source,&control)==XR_XIR_CANCELLED);
    record_seed_cancel_enabled=false;CHECK(record_seed_cancel&&!source.checked&&!source.snapshot);
    xr_xir_compile_source_result_free(&source);CHECK(held(context->resources)==baseline);
    CHECK(check(context,record_seed_source,&source,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);CHECK(held(context->resources)==baseline);
    CHECK(check(context,"final class Local{value:i64 init(value:i64){this.value=value}}\n"
        "fn run(){const ch=Channel<{payload:Local}>(1)}",&source,NULL)==XR_XIR_BAD_TYPE);
    CHECK(!source.checked&&!source.snapshot);xr_xir_compile_source_result_free(&source);
    CHECK(held(context->resources)==baseline);
    printf("Record seed: %zu physical OOM points, exact projections, retired Source and cancellation recovery\n",allocations);
}
