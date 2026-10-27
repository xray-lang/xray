/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * literal_cases.h - Genuine generic results survive AST-independent receiving
 *
 * KEY CONCEPT:
 *   The batch owns current results after all syntax producers are dead. Ordinary
 *   checking, packets and specialization retain the original generic inventory.
 */
#include "xir/xxir_literal_internal.h"
#include "literal_runtime.h"
static XirCtfeInstanceBatch *literal_batch(XrXirCompileContext *context,const char *kind) {
    if (!strcmp(kind,"multi")) {
        XirSourceComptimeInstances stage={0};
        CHECK(comptime_instances_operation(context,comptime_instances_text("multi"),&stage,
            comptime_instances_limits(),XR_XIR_CTFE_MAX_CALL_DEPTH,false)==XR_XIR_OK);
        XirCtfeInstanceView views[2]={0};XirSourceComptimeBinding rows[2]={0};
        CHECK(xr_xir_compile_source_comptime_instances_view(&stage,views,rows,2)==XR_XIR_CTFE_OK);
        uint32_t functions[2]={views[0].instance_function,views[1].instance_function};
        XirCtfeInstanceBatchRequest request=instance_batch_request(views[0].checked,functions,2);XirCtfeInstanceBatch *batch=NULL;
        CHECK(xr_xir_compile_ctfe_instance_batch(&request,NULL,&batch)==XR_XIR_CTFE_OK && batch);
        xr_xir_compile_source_comptime_instances_free(&stage);return batch;
    }
    XrXirArtifact *closed=instance_source(context,kind);
    uint32_t functions[2]={instance_named(closed,kind,0),instance_named(closed,kind,1)};
    XirCtfeInstanceBatchRequest request=instance_batch_request(closed,functions,2);XirCtfeInstanceBatch *batch=NULL;
    CHECK(xr_xir_compile_ctfe_instance_batch(&request,NULL,&batch)==XR_XIR_CTFE_OK && batch);
    xr_xir_compile_artifact_free(closed);return batch;
}
static void literal_template(const XrXirModule *before,const XrXirModule *after) {
    CHECK(before->provenance && before->provenance->kind==XR_XIR_EVIDENCE_TEMPLATE);
    CHECK(after->provenance && after->provenance->kind==XR_XIR_EVIDENCE_TEMPLATE);
    for (uint32_t f=0;f<before->function_count;++f) {
        const XrXirFunctionEffectContract *a=&before->provenance->contracts[f],*b=&after->provenance->contracts[f];
        CHECK(a->parameter_count==b->parameter_count && a->value_count==b->value_count && a->binding_count==b->binding_count);
        CHECK(a->formula.constant_mask==b->formula.constant_mask && a->formula.term_count==b->formula.term_count);
        CHECK(!a->parameter_count || !memcmp(a->parameters,b->parameters,(size_t)a->parameter_count*sizeof(*a->parameters)));
        CHECK(!a->value_count || !memcmp(a->values,b->values,(size_t)a->value_count*sizeof(*a->values)));
        CHECK(!a->binding_count || !memcmp(a->bindings,b->bindings,(size_t)a->binding_count*sizeof(*a->bindings)));
        CHECK(!a->formula.term_count || !memcmp(a->formula.terms,b->formula.terms,(size_t)a->formula.term_count*sizeof(*a->formula.terms)));
    }
}
static void literal_value(const char *kind,const XrXirValue *value) {
    CHECK(xr_xir_value_valid(value));
    if (!strcmp(kind,"scalar") || !strcmp(kind,"foreign")) CHECK(value->type==XR_XIR_I64 && value->payload==42);
    if (!strcmp(kind,"unit")) CHECK(!value->type && !value->reserved && !value->payload);
    if (!strcmp(kind,"text")) {
        const char *bytes=NULL;size_t length=0;
        CHECK(xr_xir_string_view(value,&bytes,&length) && length==3 && !memcmp(bytes,"a\0b",3));
    }
    if (!strcmp(kind,"array") || !strcmp(kind,"nested")) {
        XrXirValueAdmission admission={.arena=xr_xir_value_arena(value),.work=65536,.scratch_bytes=65536};
        int64_t length=-1;CHECK(xr_xir_array_len(value,&admission,&length)==XR_XIR_VALUE_OK && !length);
    }
    if (!strcmp(kind,"optional")) {
        bool some=true;const XrXirValue *payload=NULL;CHECK(xr_xir_nullable_view(value,&some,&payload) && !some);
    }
}
static void literal_normal(const char *kind) {
    XrXirCompileContext context;context_new(&context);XirCtfeInstanceBatch *batch=literal_batch(&context,kind);
    XirCtfeInstanceView views[2]={0};CHECK(xr_xir_compile_ctfe_instance_batch_views(batch,views,2)==XR_XIR_CTFE_OK);
    uint32_t original_count=views[0].definition->module.function_count,definition=views[0].definition_function;
    if (!strcmp(kind,"nested")) {
        CHECK(instance_argument_absent(views[0].checked,views[0].checked->module.provenance->origins[views[0].instance_function].arguments[0]));
    }
    XirLiteralRequest request={&context,batch,2,NULL};XirLiteralDefinitions result={0};XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_instance_literals(&request,&result,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"Literal %s status=%u f=%u b=%u i=%u\n",kind,status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_OK && result.definition && result.count==2);
    CHECK(result.definition->context.resources==context.resources && result.definition->module.function_count>=original_count+2);
    CHECK(result.definition->module.generics[definition].parameter_count==views[0].definition->module.generics[definition].parameter_count);
    literal_template(&views[0].definition->module,&result.definition->module);
    uint32_t providers[2];
    for (unsigned i=0;i<2;++i) {
        XirLiteralBinding *binding=&result.bindings[i];providers[i]=binding->provider;
        CHECK(binding->definition==definition && binding->instance==views[i].instance_function &&
            binding->argument_count==(!strcmp(kind,"multi")?2u:1u));
        CHECK(binding->provider>=original_count && !result.definition->module.generics[binding->provider].parameter_count);
        CHECK(!result.definition->module.declarations->functions[binding->provider].exported);
        for (uint32_t a=0;a<binding->argument_count;++a)
            CHECK(xr_xir_compile_type_substitution_matches_between(&context,views[i].checked->module.types,result.definition->module.types,NULL,0,
                views[i].checked->module.provenance->origins[binding->instance].arguments[a],binding->arguments[a])==XR_XIR_OK);
    }
    CHECK(providers[0]!=providers[1] && result.bindings[0].arguments[0]!=result.bindings[1].arguments[0]);
    if (!strcmp(kind,"multi")) CHECK(result.bindings[0].arguments[0]==result.bindings[1].arguments[1] &&
        result.bindings[0].arguments[1]==result.bindings[1].arguments[0]);
    if (!strcmp(kind,"boxed")) CHECK(xr_xir_compile_construction_materialization_count(result.definition->construction)==2);
    xr_xir_compile_ctfe_instance_batch_free(batch);memset(views,0xa5,sizeof(views));
    CHECK(xr_xir_compile_artifact_verify(result.definition,NULL)==XR_XIR_OK);
    if (!strcmp(kind,"scalar")) {
        XrXirArtifact *routed=literal_runtime_callers(&context,&result);
        xr_xir_compile_artifact_free(result.definition);result.definition=routed;
    }
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(result.definition,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_instance_literals_free(&result);CHECK(!result.definition && !result.bindings && !result.count);
    XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    if (!strcmp(kind,"foreign")) instance_foreign_edges(closed);
    uint32_t functions[2]={UINT32_MAX,UINT32_MAX};
    for (uint32_t f=0;f<closed->module.function_count;++f) for (unsigned i=0;i<2;++i)
        if (closed->module.provenance->origins[f].function==providers[i]) { CHECK(functions[i]==UINT32_MAX);functions[i]=f; }
    CHECK(functions[0]!=UINT32_MAX && functions[1]!=UINT32_MAX);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    XrXirValue values[2]={0};
    for (unsigned i=0;i<2;++i) {
        XrXirCtfeCallRequest call=import_request(lowered,"run",NULL);call.evaluation.function=functions[i];
        CHECK(xr_xir_compile_ctfe_calls(&call,&values[i])==XR_XIR_CTFE_OK);literal_value(kind,&values[i]);
    }
    if (strcmp(kind,"scalar") && strcmp(kind,"foreign") && strcmp(kind,"unit") && strcmp(kind,"text")) CHECK(values[0].type!=values[1].type);
    xr_compile_resources_release(context.resources);
    if (!strcmp(kind,"scalar")) literal_runtime_instances(&lowered);
    xr_xir_compile_artifact_free(lowered);
    size_t attempts=source_fixture_compile_attempts;source_fixture_compile_fail_at=attempts;
    xr_xir_value_drop(&values[0]);xr_xir_value_drop(&values[1]);CHECK(source_fixture_compile_attempts==attempts);
    source_fixture_compile_fail_at=SIZE_MAX;CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Typed literal %s two genuine substitutions AST/producers dead full generic definitions ordinary Checker/packet/Specializer/CTFE last-owner physical0\n",kind);
}
