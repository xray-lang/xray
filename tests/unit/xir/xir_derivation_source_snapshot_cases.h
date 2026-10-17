/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_derivation_source_snapshot_cases.h - Real Source policy lease correspondence
 */
#ifndef XIR_DERIVATION_SOURCE_SNAPSHOT_CASES_H
#define XIR_DERIVATION_SOURCE_SNAPSHOT_CASES_H
#include "xir/xxir_internal.h"

static bool derive_snapshot_source_facts(XrXirArtifact *artifact) {
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    const XrXirCompileContext *context=xr_xir_compile_artifact_context(artifact);
    CHECK(module && module->stage==XR_XIR_CHECKED && module->provenance &&
        module->provenance->kind==XR_XIR_EVIDENCE_TEMPLATE && artifact->source_facts);
    const XrXirDerivationTable *table=module->declarations->derivations;
    CHECK(table && table->policy_count==1 && table->count);
    XrXirDerivationPolicy *policy=(XrXirDerivationPolicy *)&table->policies[0];
    const XrXirTypeNode *target=xr_xir_type_node(module->types,policy->target);
    CHECK(target && target->kind==XR_XIR_TYPE_NOMINAL && !policy->constructor);
    uint32_t constructor=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunctionIdentity *identity=&module->declarations->functions[f];
        if(identity->module==policy->grantor_module &&
            identity->nominal_owner==target->nominal.declaration+1 &&
            identity->method_kind==XR_XIR_CONSTRUCTOR) {
            CHECK(constructor==UINT32_MAX);constructor=f;
        }
    }
    CHECK(constructor<module->function_count);
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    size_t blocks=source_fixture_compile_live,bytes=source_fixture_compile_bytes;
    XirSourceFacts *lease=NULL;
    CHECK(xir_source_facts_acquire(artifact,&lease,NULL)==XR_XIR_OK && lease);
    xir_source_facts_release(lease);lease=NULL;
    /* This is another valid policy for the exact real constructor, so ordinary
     * policy validation alone cannot prove the retained input is unchanged. */
    policy->constructor=constructor+1;
    CHECK(xir_derivation_policy_verify(context,module,policy)==XR_XIR_OK);
    XrXirStatus status=xir_source_facts_acquire(artifact,&lease,NULL);
    bool refused=status==XR_XIR_BAD_STRUCTURE && !lease;
    xir_source_facts_release(lease);lease=NULL;policy->constructor=0;
    CHECK(xir_source_facts_acquire(artifact,&lease,NULL)==XR_XIR_OK && lease);
    xir_source_facts_release(lease);
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK);
    bool balanced=after.live_bytes==before.live_bytes && source_fixture_compile_live==blocks &&
        source_fixture_compile_bytes==bytes;
    fprintf(stderr,"Real Source policy constructor%u: changed lease status%u expected%u live%llu/%llu physical%zu/%zu\n",
        constructor,(unsigned)status,(unsigned)XR_XIR_BAD_STRUCTURE,
        (unsigned long long)after.live_bytes,(unsigned long long)before.live_bytes,
        source_fixture_compile_live,blocks);
    return refused && balanced;
}
#endif // XIR_DERIVATION_SOURCE_SNAPSHOT_CASES_H
