/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_facts_cases.h - Genuine immutable Source ownership consumers
 */
#ifndef XIR_SOURCE_FACTS_CASES_H
#define XIR_SOURCE_FACTS_CASES_H

static XrXirStatus source_facts_case_fixture(const XrXirCompileContext *context,
    XrXirArtifact **output) {
    static const char text[]=
        "fn plain(value:i64)->i64{return value+1}\n"
        "fn keep<T>(callback:fn(T)->T)->fn(T)->T{return callback}\n"
        "fn use<T>(callback:fn(T)->T,value:T)->T{return callback(value)}\n"
        "export fn run()->i64{const callback=keep<i64>(plain);return use<i64>(plain,callback(40))}\nprint(run())\n";
    XrCompilerSession *session=NULL;
    XrCompilerSessionStatus opened=xr_compile_session_new(context->resources,&session);
    if (opened!=XR_COMPILER_SESSION_OK)
        return opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"source-facts",NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={NULL,text,sizeof(text)-1};XrXirSourceResult result={0};XrXirSourceFailure failure={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&result,NULL,&failure);
    if (status==XR_XIR_OK) { *output=result.checked;result.checked=NULL; }
    xr_xir_compile_source_result_free(&result);xr_xir_compile_source_failure_free(&failure);
    xr_compile_session_free(session);return status;
}

static void source_facts_case_refuse(XrXirArtifact *source) {
    XirSourceFacts *output=NULL;XrCompileResourceStats before=library_compile_stats(&source->context);
    CHECK(xir_source_facts_acquire(source,&output,NULL)==XR_XIR_BAD_STRUCTURE && !output);
    CHECK(library_compile_stats(&source->context).live_bytes==before.live_bytes);
}

/* These are complete current Template inputs. A shape-valid change cannot
 * use the unchanged immutable facts; each original field is then restored. */
static void source_facts_case_contracts(XrXirArtifact *source) {
    XrXirProvenance *provenance=(XrXirProvenance *)source->module.provenance;
    CHECK(provenance && provenance->kind==XR_XIR_EVIDENCE_TEMPLATE && provenance->contract_count);
    XrXirFunctionEffectContract *contracts=(XrXirFunctionEffectContract *)provenance->contracts;
    uint32_t parameters=0,values=0,bindings=0;
    for (uint32_t f=0;f<provenance->contract_count;++f) {
        uint32_t mask=contracts[f].formula.constant_mask;
        contracts[f].formula.constant_mask^=XR_XIR_CALLABLE_ROOT_REQUIRED;
        source_facts_case_refuse(source);contracts[f].formula.constant_mask=mask;
        for (uint32_t p=0;p<contracts[f].parameter_count;++p) {
            XrXirEffectParameter *record=(XrXirEffectParameter *)&contracts[f].parameters[p];
            uint32_t uses=record->uses;record->uses^=1;
            source_facts_case_refuse(source);record->uses=uses;++parameters;
        }
        for (uint32_t t=0;t<contracts[f].formula.term_count;++t) {
            XrXirRootTerm *record=(XrXirRootTerm *)&contracts[f].formula.terms[t];
            uint32_t held=record->index;record->index^=1;
            source_facts_case_refuse(source);record->index=held;
        }
        for (uint32_t v=0;v<contracts[f].value_count;++v) {
            XrXirRootValueIdentity *record=(XrXirRootValueIdentity *)&contracts[f].values[v];
            XrXirType declared=record->declared_type;
            record->declared_type=declared==XR_XIR_I64?XR_XIR_BOOL:XR_XIR_I64;
            source_facts_case_refuse(source);record->declared_type=declared;++values;
        }
        for (uint32_t b=0;b<contracts[f].binding_count;++b) {
            XrXirEffectCallBinding *record=(XrXirEffectCallBinding *)&contracts[f].bindings[b];
            uint32_t held=record->value;record->value^=1;
            source_facts_case_refuse(source);record->value=held;++bindings;
        }
    }
    CHECK(parameters && values && bindings);
    XirSourceFacts *lease=NULL;CHECK(xir_source_facts_acquire(source,&lease,NULL)==XR_XIR_OK && lease);
    xir_source_facts_release(lease);
}

static void source_facts_case_requests(XrXirArtifact *source,const XrXirCompileContext *foreign) {
    XirSourceFacts *occupied=(XirSourceFacts *)(uintptr_t)1;
    XrCompileResourceStats entry=library_compile_stats(&source->context),other=library_compile_stats(foreign);
    size_t attempts=source_program_compile_attempts;
    CHECK(xir_source_facts_acquire(source,&occupied,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(occupied==(XirSourceFacts *)(uintptr_t)1);
    XrXirArtifact view=*source;view.context=*foreign;occupied=NULL;
    CHECK(xir_source_facts_acquire(&view,&occupied,NULL)==XR_XIR_BAD_STRUCTURE && !occupied);
    view=*source;view.source_facts=NULL;
    CHECK(xir_source_facts_acquire(&view,&occupied,NULL)==XR_XIR_BAD_STRUCTURE && !occupied);
    view=*source;view.module.stage=XR_XIR_BUILT;
    CHECK(xir_source_facts_acquire(&view,&occupied,NULL)==XR_XIR_BAD_STRUCTURE && !occupied);
    CHECK(source_program_compile_attempts==attempts);
    XrCompileResourceStats after=library_compile_stats(&source->context),foreign_after=library_compile_stats(foreign);
    CHECK(after.work==entry.work && after.allocated_bytes==entry.allocated_bytes && after.live_bytes==entry.live_bytes);
    CHECK(foreign_after.work==other.work && foreign_after.allocated_bytes==other.allocated_bytes &&
        foreign_after.live_bytes==other.live_bytes);
}

/* Primitive-only source has a genuine empty physical type domain. The
 * normalized request must not accept an added unused physical type record. */
static void source_facts_case_empty_types(void) {
    LibraryCompileOwner owner={0};
    CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILER_SESSION_OK);
    static const char text[]="export fn plain(value:i64)->i64{return value+1}\nprint(plain(41))\n";
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"source-empty-types",NULL};
    XrXirSourceRequest request={session,NULL,&authority,&owner.context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={NULL,text,sizeof(text)-1};XrXirSourceResult output={0};
    CHECK(xr_xir_compile_source_check_text(&request,&input,&output,NULL,NULL)==XR_XIR_OK);
    XrXirArtifact *source=output.checked;output.checked=NULL;
    xr_xir_compile_source_result_free(&output);xr_compile_session_free(session);
    CHECK(source && source->source_facts && !source->module.types);
    XirSourceFacts *lease=NULL;
    CHECK(xir_source_facts_acquire(source,&lease,NULL)==XR_XIR_OK && lease);
    xir_source_facts_release(lease);lease=NULL;
    XrXirTypeNode unused={.kind=XR_XIR_TYPE_TUPLE};
    XrXirTypes changed={.nodes=&unused,.count=1};
    source->module.types=&changed;source_facts_case_refuse(source);
    source->module.types=NULL;
    CHECK(xr_xir_compile_artifact_verify(source,NULL)==XR_XIR_OK);
    CHECK(xir_source_facts_acquire(source,&lease,NULL)==XR_XIR_OK && lease);
    xir_source_facts_release(lease);xr_xir_compile_artifact_free(source);
    library_compile_owner_drop(&owner);
}

/* The producer's context is overwritten after its Artifact dies. Both the
 * transferred Source lease and a freshly decoded receiver remain independent. */
static void source_facts_case_lifetime(void) {
    source_facts_case_empty_types();
    LibraryCompileOwner owner={0},foreign={0};
    CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    CHECK(library_compile_owner_new(&foreign,&library_compile_limits)==XR_XIR_OK);
    XrXirArtifact *source=NULL,*instance=NULL,*peer=NULL,*decoded=NULL,*receiver=NULL;
    XrXirCompileContext producer=owner.context;
    CHECK(source_facts_case_fixture(&producer,&source)==XR_XIR_OK && source->source_facts);
    memset(&producer,0xA5,sizeof(producer));
    source_facts_case_requests(source,&foreign.context);source_facts_case_contracts(source);
    XirSourceFacts *lease=NULL;CHECK(xir_source_facts_acquire(source,&lease,NULL)==XR_XIR_OK && lease);
    CHECK(xir_source_facts_effects(lease));
    CHECK(xr_xir_compile_specialize(source,&instance,NULL)==XR_XIR_OK);
    CHECK(instance->module.provenance->source->source_facts==source->source_facts);
    CHECK(xr_xir_compile_recheck_v2(&owner.context,&instance->module,instance->construction,&peer,NULL)==XR_XIR_OK);
    CHECK(peer->module.provenance->source->source_facts==source->source_facts);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(instance,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(source);source=NULL;
    xr_xir_compile_artifact_free(instance);instance=NULL;
    xir_source_facts_release(lease);lease=NULL;
    CHECK(xr_xir_compile_artifact_verify(peer,NULL)==XR_XIR_OK);
    CHECK(xir_source_facts_acquire(peer->module.provenance->source,&lease,NULL)==XR_XIR_OK);
    xir_source_facts_release(lease);lease=NULL;
    CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
    CHECK(decoded->module.provenance->source->source_facts!=peer->module.provenance->source->source_facts);
    CHECK(xr_xir_compile_recheck_v2(&foreign.context,&peer->module,peer->construction,&receiver,NULL)==XR_XIR_OK);
    CHECK(receiver->module.provenance->source->source_facts!=peer->module.provenance->source->source_facts);
    xr_xir_compile_artifact_free(peer);peer=NULL;
    CHECK(xr_xir_compile_artifact_verify(decoded,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_verify(receiver,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(receiver);xr_xir_compile_artifact_free(decoded);
    xr_xir_compile_checked_packet_free(&packet);
    library_compile_owner_drop(&foreign);library_compile_owner_drop(&owner);
}
#endif // XIR_SOURCE_FACTS_CASES_H
