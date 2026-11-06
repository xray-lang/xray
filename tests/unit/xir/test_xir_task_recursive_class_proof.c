/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_task_recursive_class_proof.c - Finite resources for owned type proofs
 */
#define main recursive_class_unused_emitter
#include "test_xir_task_constructed_emit.c"
#undef main
#include "xir/xxir_compile_memory.h"
static XrXirStatus recursive_class_query(const XrXirModule *module,XrXirType type,
    XrCompileResourceLimits limits,XrCompileResourceStats *cost) {
    size_t live=effects_compile_live,bytes=effects_compile_bytes;
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    XrCompileResourceStatus made=xr_compile_resources_new(&limits,&context.resources);
    XrXirStatus status=xir_compile_resource_status(made);
    CHECK(made==XR_COMPILE_RESOURCE_OK || status!=XR_XIR_OK);
    if (status==XR_XIR_OK) {
        XrCompileResourceStats baseline={0};
        CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
        XrXirProofContext proof={module,{XR_XIR_CONTEXT_FUNCTION,0,0}};
        status=xr_xir_compile_type_markers_prove(&context,&proof,type,XR_XIR_CONSTRAINT_SENDABLE);
        XrCompileResourceStats stats={0};
        CHECK(xr_compile_resources_stats(context.resources,&stats)==XR_COMPILE_RESOURCE_OK);
        CHECK(stats.held_bytes==baseline.held_bytes);
        if (cost) *cost=stats;
    }
    xr_compile_resources_release(context.resources);
    CHECK(effects_compile_live==live && effects_compile_bytes==bytes);return status;
}
static void recursive_class_value_cycles(const XrXirModule *module,XrXirType payload,
    XrCompileResourceLimits limits) {
    XrXirModule bad=*module;XrXirTypes types=*module->types;XrXirNominalTable table=*types.nominals;
    XrXirNominalIdentity identities[128];
    CHECK(table.identities && table.count<=128);
    memcpy(identities,table.identities,table.count*sizeof(*identities));
    const XrXirTypeNode *node=xr_xir_type_node(&types,payload);CHECK(node);
    identities[node->nominal.declaration].kind=XR_XIR_NOMINAL_STRUCT;
    identities[node->nominal.declaration].flags=0;
    table.identities=identities;types.nominals=&table;bad.types=&types;
    CHECK(recursive_class_query(&bad,payload,limits,NULL)==XR_XIR_BAD_TYPE);
    unsigned boxes=0;
    for (uint32_t t=0;t<types.count;++t) {
        node=&types.nodes[t];if (node->kind!=XR_XIR_TYPE_NOMINAL) continue;
        XrXirLiteral name=identities[node->nominal.declaration].name;
        if (name.length!=3 || memcmp(name.bytes,"Box",3)) continue;
        /* An identity outside the active value-only cycle cannot close it. */
        CHECK(recursive_class_query(&bad,(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+t),limits,NULL)==XR_XIR_BAD_TYPE);
        ++boxes;
    }
    CHECK(boxes);
}
int main(int argc,char **argv) {
    CHECK(argc==2 && (!strcmp(argv[1],"normal") || !strcmp(argv[1],"fi")));
    const XrXirCompileContext *context=effects_source_owner();
    uint32_t entries[CONSTRUCTED_CASES][2]={{0}};
    XrXirArtifact *lowered=constructed_lower(context,entries,false);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    XrXirType type=module->functions[entries[0][0]].result;
    CHECK(xr_xir_type_is_class(module->types,type));
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_FINITE,UINT64_C(8388608),true};
    recursive_class_value_cycles(module,type,limits);
    XrCompileResourceStats cost={0};effects_compile_attempts=0;
    CHECK(recursive_class_query(module,type,limits,&cost)==XR_XIR_OK);
    size_t sites=effects_compile_attempts;CHECK(sites && sites<4096);
    if (!strcmp(argv[1],"fi")) for (size_t site=0;site<sites;++site) {
        effects_compile_attempts=0;effects_compile_fail_at=site;effects_compile_injected=false;
        XrXirStatus status=recursive_class_query(module,type,limits,NULL);effects_compile_fail_at=SIZE_MAX;
        CHECK(effects_compile_injected && status==XR_XIR_OUT_OF_MEMORY);
    }
    for (unsigned less=0;less<2;++less) {
        CHECK(cost.peak_bytes);
        XrCompileResourceLimits exact={XR_COMPILE_MEMORY_FINITE,cost.peak_bytes-less,true};
        CHECK(recursive_class_query(module,type,exact,NULL)==(less?XR_XIR_MEMORY_LIMIT:XR_XIR_OK));
    }
    printf("RECURSIVE_CLASS_PROOF %s sites=%zu allocated=%llu peak=%llu finiteHeld physicalBaseline PASS\n",
        argv[1],sites,(unsigned long long)cost.allocated_bytes,(unsigned long long)cost.peak_bytes);
    xr_xir_compile_artifact_free(lowered);effects_source_owners_free();return 0;
}
