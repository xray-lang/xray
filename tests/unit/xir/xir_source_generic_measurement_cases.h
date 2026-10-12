/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_generic_measurement_cases.h - Original generic pipeline inventory
 *
 * KEY CONCEPT:
 *   Observation reads current owned outputs before release without deriving
 *   facts or substituting a call-site count for specialized instances.
 */
#ifndef XIR_SOURCE_GENERIC_MEASUREMENT_CASES_H
#define XIR_SOURCE_GENERIC_MEASUREMENT_CASES_H
#include "xir/xxir_declarations.h"
#include "xir/xxir_effect_contract_internal.h"

typedef struct SgmPoint {
    XrCompileResourceStats stats;
    size_t attempts, physical_blocks, physical_bytes;
} SgmPoint;
static bool sgm_enabled;
static uint32_t sgm_calls;
static SgmPoint sgm_before;
static SgmPoint sgm_point(const XrXirCompileContext *context) {
    SgmPoint point={0}; point.stats=library_compile_stats(context);
    point.attempts=source_program_compile_attempts;
    point.physical_blocks=source_program_compile_live; point.physical_bytes=source_program_compile_bytes;
    return point;
}
static void sgm_begin(const XrXirCompileContext *context) {
    if(sgm_enabled)sgm_before=sgm_point(context);
}
static void sgm_interval(const char *name,const char *kind,const SgmPoint *before,
    const SgmPoint *after,XrXirStatus status) {
    printf("{\"schema\":\"task324.generic.original.v1\",\"event\":\"interval\",\"phase\":\"%s\","
        "\"kind\":\"%s\",\"parent\":\"operation\",\"status\":%u,\"work_before\":%llu,\"work_after\":%llu,"
        "\"work_delta\":%llu,\"allocated_before\":%llu,\"allocated_after\":%llu,\"allocated_delta\":%llu,"
        "\"allocation_count_delta\":%llu,\"attempts_before\":%zu,\"attempts_after\":%zu,\"attempts_delta\":%zu,"
        "\"live_before\":%llu,\"live_after\":%llu,\"peak_before\":%llu,\"peak_after\":%llu,"
        "\"physical_blocks_before\":%zu,\"physical_blocks_after\":%zu,\"physical_bytes_before\":%zu,\"physical_bytes_after\":%zu}\n",
        name,kind,(unsigned)status,(unsigned long long)before->stats.work,(unsigned long long)after->stats.work,
        (unsigned long long)(after->stats.work-before->stats.work),
        (unsigned long long)before->stats.allocated_bytes,(unsigned long long)after->stats.allocated_bytes,
        (unsigned long long)(after->stats.allocated_bytes-before->stats.allocated_bytes),
        (unsigned long long)(after->stats.allocation_count-before->stats.allocation_count),
        before->attempts,after->attempts,after->attempts-before->attempts,
        (unsigned long long)before->stats.live_bytes,(unsigned long long)after->stats.live_bytes,
        (unsigned long long)before->stats.peak_bytes,(unsigned long long)after->stats.peak_bytes,
        before->physical_blocks,after->physical_blocks,before->physical_bytes,after->physical_bytes);
}
static void sgm_end(const XrXirCompileContext *context,const char *name,const char *kind,XrXirStatus status) {
    if(!sgm_enabled)return;
    if(!strcmp(kind,"public"))++sgm_calls;
    SgmPoint after=sgm_point(context); sgm_interval(name,kind,&sgm_before,&after,status);
}
static void sgm_origin(const char *phase,const XrXirModule *module,uint32_t index) {
    const XrXirProvenance *proof=module->provenance;
    const XrXirOrigin *origin=&proof->origins[index];
    const XrXirModule *source=xr_xir_compile_artifact_module(proof->source);
    CHECK(source && source->declarations && module->declarations && origin->function<source->function_count);
    const XrXirFunctionIdentity *original=&source->declarations->functions[origin->function];
    const XrXirFunctionIdentity *actual=&module->declarations->functions[index];
    uint64_t ordinary=(uint64_t)origin->argument_count*sizeof(*origin->arguments);
    uint64_t effects=(uint64_t)origin->effect_argument_count*sizeof(*origin->effect_arguments);
    printf("{\"schema\":\"task324.generic.original.v1\",\"event\":\"origin\",\"phase\":\"%s\","
        "\"instance\":%u,\"source_declaration\":%u,\"source_module\":%u,\"source_cleanup_owner\":%u,"
        "\"cleanup_owner\":%u,\"argument_pool\":\"current_destination\",\"source_type_count\":%u,"
        "\"destination_type_count\":%u,\"argument_count\":%u,\"ordinary_vector_bytes\":%llu,"
        "\"effect_argument_count\":%u,\"ordered_effect_vector_bytes\":%llu,\"logical_key_fields_bytes\":%llu,\"arguments\":[",
        phase,index,origin->function,original->module,original->cleanup_owner,actual->cleanup_owner,
        source->types?source->types->count:0,module->types?module->types->count:0,origin->argument_count,
        (unsigned long long)ordinary,origin->effect_argument_count,(unsigned long long)effects,
        (unsigned long long)(4*sizeof(uint32_t)+ordinary+effects));
    for(uint32_t a=0;a<origin->argument_count;++a)printf("%s%u",a?",":"",(unsigned)origin->arguments[a]);
    printf("],\"ordered_effect_arguments\":[");
    for(uint32_t a=0;a<origin->effect_argument_count;++a)
        printf("%s{\"parameter\":%u,\"type\":%u}",a?",":"",origin->effect_arguments[a].parameter,
            (unsigned)origin->effect_arguments[a].type);
    printf("]}\n");
}
static void sgm_function(const char *phase,const XrXirModule *module,const XrXirEffects *effects,uint32_t f) {
    const XrXirFunction *function=&module->functions[f];
    const XrXirFunctionIdentity *identity=module->declarations?&module->declarations->functions[f]:NULL;
    const XrXirFunctionEffectContract *contract=effects?xir_effects_contract(effects,f):NULL;
    uint32_t references=0,indirect=0,returns=0;
    for(uint32_t i=0;i<function->instruction_count;++i) {
        references+=function->instructions[i].op==XR_XIR_FUNCTION_REF;
        indirect+=function->instructions[i].op==XR_XIR_CALL_INDIRECT;
        returns+=function->instructions[i].op==XR_XIR_RETURN;
    }
    printf("{\"schema\":\"task324.generic.original.v1\",\"event\":\"function\",\"phase\":\"%s\","
        "\"function\":%u,\"has_identity\":%s,\"module\":%u,\"cleanup_owner\":%u,\"generic_parameters\":%u,"
        "\"parameters\":%u,\"result\":%u,\"instructions\":%u,\"blocks\":%u,\"function_refs\":%u,"
        "\"indirect_calls\":%u,\"returns\":%u,\"has_owned_contract\":%s,\"contract_values\":%u,\"contract_bindings\":%u}\n",
        phase,f,identity?"true":"false",identity?identity->module:0,identity?identity->cleanup_owner:0,
        module->generics?module->generics[f].parameter_count:0,function->parameter_count,(unsigned)function->result,
        function->instruction_count,function->block_count,references,indirect,returns,contract?"true":"false",
        contract?contract->value_count:0,contract?contract->binding_count:0);
}
static void sgm_artifact(const char *phase,const XrXirArtifact *artifact) {
    if(!sgm_enabled)return;
    CHECK(artifact); const XrXirModule *module=xr_xir_compile_artifact_module(artifact); CHECK(module);
    const XrXirProvenance *proof=module->provenance;
    const XrXirEffects *effects=xir_source_facts_effects(artifact->source_facts);
    uint64_t rows=0,blocks=0,formals=0; uint32_t generic_definitions=0;
    for(uint32_t f=0;f<module->function_count;++f) {
        rows+=module->functions[f].instruction_count; blocks+=module->functions[f].block_count;
        formals+=module->functions[f].parameter_count;
        if(module->generics && module->generics[f].parameter_count)++generic_definitions;
    }
    printf("{\"schema\":\"task324.generic.original.v1\",\"event\":\"artifact\",\"phase\":\"%s\","
        "\"stage\":%u,\"functions\":%u,\"rows\":%llu,\"blocks\":%llu,\"formals\":%llu,\"types\":%u,"
        "\"has_declarations\":%s,\"modules\":%u,\"slots\":%u,\"literals\":%u,\"generic_definitions\":%u,"
        "\"has_provenance\":%s,\"evidence_kind\":%u,\"origin_count\":%u,\"contract_count\":%u,"
        "\"binding_count\":%u,\"has_owned_source_facts\":%s,\"has_owned_source_effects\":%s}\n",
        phase,(unsigned)module->stage,module->function_count,(unsigned long long)rows,(unsigned long long)blocks,
        (unsigned long long)formals,module->types?module->types->count:0,module->declarations?"true":"false",
        module->declarations?module->declarations->module_count:0,module->declarations?module->declarations->slot_count:0,
        module->declarations?module->declarations->literal_count:0,generic_definitions,proof?"true":"false",proof?proof->kind:0,
        proof?proof->count:0,proof?proof->contract_count:0,proof?proof->binding_count:0,
        artifact->source_facts?"true":"false",effects?"true":"false");
    for(uint32_t f=0;f<module->function_count;++f)sgm_function(phase,module,effects,f);
    if(proof && proof->kind==XR_XIR_EVIDENCE_INSTANCE) {
        CHECK(proof->count==module->function_count && proof->source && proof->origins);
        for(uint32_t f=0;f<proof->count;++f)sgm_origin(phase,module,f);
    }
}
static void sgm_packet(const char *phase,const XrXirCheckedPacket *packet) {
    if(sgm_enabled)
        printf("{\"schema\":\"task324.generic.original.v1\",\"event\":\"packet\",\"phase\":\"%s\",\"bytes\":%zu}\n",
            phase,packet->length);
}
static void sgm_original(void) {
    CHECK(!sgm_enabled && source_program_compile_fail_at==SIZE_MAX);
    LibraryCompileOwner owner={0}; CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    SgmPoint prepared=sgm_point(&owner.context);
    printf("{\"schema\":\"task324.generic.original.v1\",\"event\":\"input\",\"source_bytes\":%zu,"
        "\"original\":true,\"factor\":1,\"scale_family\":\"original_only\",\"declared_user_functions\":9,"
        "\"declared_user_generic_functions\":7,\"expected_public_calls\":8,\"inspect\":false,\"FI\":false,"
        "\"allocated_cap\":%llu,\"live_cap\":%llu,\"work_cap\":%llu,\"prepare_work\":%llu,"
        "\"prepare_allocated\":%llu,\"prepare_sites\":%zu}\n",
        sizeof(source_generic_definition_text)-1,(unsigned long long)library_compile_limits.allocated_bytes,
        (unsigned long long)library_compile_limits.live_bytes,(unsigned long long)library_compile_limits.work,
        (unsigned long long)prepared.stats.work,(unsigned long long)prepared.stats.allocated_bytes,prepared.attempts);
    source_program_compile_attempts=0; sgm_calls=0; sgm_enabled=true;
    SgmPoint before=sgm_point(&owner.context); bool inspect=false;
    XrXirStatus status=source_generic_definition_operation(&owner.context,&inspect);
    SgmPoint after=sgm_point(&owner.context); sgm_interval("operation","inclusive",&before,&after,status);
    printf("{\"schema\":\"task324.generic.original.v1\",\"event\":\"call_count\",\"actual_public_calls\":%u}\n",sgm_calls);
    fflush(stdout);
    sgm_enabled=false; CHECK(status==XR_XIR_OK && sgm_calls==8 && after.stats.live_bytes==owner.baseline.live_bytes);
    library_compile_owner_drop(&owner); library_compile_observer_free();
    CHECK(!source_program_compile_live && !source_program_compile_bytes);
    printf("{\"schema\":\"task324.generic.original.v1\",\"event\":\"owner_drop\","
        "\"physical_blocks\":0,\"physical_bytes\":0,\"qualification\":\"MEASUREMENT_ONLY\"}\n");
    fflush(stdout);
}
#endif /* XIR_SOURCE_GENERIC_MEASUREMENT_CASES_H */
