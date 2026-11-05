/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_builder_capacity_retirement.c - Owned Source evidence and actual AOT cache reception
 */
#include "analysis/xglobal_producer.h"
#include "analysis/xglobal_summary.h"
#include "aot/xaot_bundle.h"
#include "aot/xaot_verify.h"
#include "module/xmodule_graph.h"
#include "module/xmodule_identity.h"
#include "toolchain/xcompiler_session.h"
#include "frontend/parser/xparse.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while(0)

int main(void) {
    const char *source = "fn read(a:Array<i64>,s:string)->i64{return a[0]+len(s)}\n";
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY,0,false};
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline = {0},after = {0};
    CHECK(xr_compile_resources_stats(resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(resources,&session)==XR_COMPILE_RESOURCE_OK);
    AstNode *ast = NULL;
    CHECK(xr_compile_parse(session,source,&ast)==XR_PARSE_OK && ast);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_MEMORY,"builder-capacity",NULL};
    char *canonical = NULL;
    CHECK(xr_compile_module_identity_from_logical(resources,&authority,NULL,&canonical)==XR_MODULE_OK);
    XrModuleSpec spec = {.canonical=canonical,.source_path="capacity.xr",.kind=XR_MOD_MEMORY,.ast=ast};
    int order = 0;
    XrModuleGraph graph = {.resources=resources,.compiler_session=session,.specs=&spec,.spec_count=1,
        .topo_order=&order,.topo_count=1,.entry_index=0};
    XgGlobalEvidence evidence = {0};
    CHECK(xg_global_evidence_build_from_module_graph(&evidence,&graph,XG_BUILD_NATIVE_RELEASE,0));
    CHECK(evidence.nsequence_accesses);
    XaotBundle bundle = {0};
    CHECK(xaot_bundle_set_global_evidence(&bundle,&evidence,XG_BUILD_NATIVE_RELEASE));
    char error[256] = {0};
    bool verified = xaot_verify_global_evidence_plan(&bundle,error,sizeof(error));
    if (!verified) fprintf(stderr,"verify: %s\n",error);
    CHECK(verified);
    char *payload = xg_global_evidence_cache_payload_dump(&evidence,XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE);
    CHECK(payload);
    XgGlobalEvidence received = {0};
    CHECK(xg_evidence_cache_payload_materialize(payload,&received));
    CHECK(received.nsequence_accesses==evidence.nsequence_accesses);
    xaot_bundle_free(&bundle);xg_global_evidence_free(&evidence);xr_free(payload);
    xr_program_destroy(ast);xr_compile_resources_free(canonical);xr_compile_session_free(session);
    CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==baseline.held_bytes);
    CHECK(received.nsequence_accesses);xg_global_evidence_free(&received);
    xr_compile_resources_release(resources);
    puts("owned AST -> evidence -> AOT verification -> cache reception passed");
    return 0;
}
