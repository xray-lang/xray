/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_callable_root_source.c - Independent ordinary reference flow oracles
 */
#include "xir/xxir_source.h"
#include "xir/xxir_effects.h"
#include "xir/xxir_types.h"
#include "xir/xxir_checked.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_library_compile_owner.h"
typedef struct RootSourceOperation { const char *file; bool qualify; } RootSourceOperation;
typedef struct RootSourceExpected { const char *name; bool root, unknown; } RootSourceExpected;
static const RootSourceExpected root_source_expected[] = {
    {"direct",false,false},{"imported",false,false},{"copied",false,false},
    {"closure",false,false},{"capture",false,false},{"fixed",false,true},
    {"parameter",false,true},{"local",false,false},{"localRoot",true,false},
    {"phi",false,false},{"phiRoot",true,false},{"tuple",false,false},
    {"tupleFixed",false,true},{"array",false,false},{"arrayRoot",true,false},
    {"arrayFixed",false,true},{"arrayMutation",false,true},{"nullableFixed",false,true},
    {"nullablePrecise",false,false},{"moduleRead",true,false},{"pureCycle",false,false},{"rootCycle",true,false},
    {"mixed",true,true}
};
static uint32_t root_source_function(const XrXirModule *module, const char *name) {
    size_t length = strlen(name);
    for (uint32_t f = 0; f < module->function_count; ++f)
        if (module->functions[f].name_length == length && !memcmp(module->functions[f].name,name,length)) return f;
    return UINT32_MAX;
}
static void root_source_facts(XrXirArtifact *artifact, bool flows) {
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    XrXirEffects *effects = NULL;
    CHECK(module && module->stage == XR_XIR_CHECKED);
    CHECK(xr_xir_compile_effects_analyze(artifact,&effects) == XR_XIR_OK);
    if (flows) {
        for (size_t i = 0; i < sizeof(root_source_expected) / sizeof(*root_source_expected); ++i) {
            const RootSourceExpected *expected = &root_source_expected[i];
            uint32_t f = root_source_function(module,expected->name);
            const XrXirRootEffects *actual = xr_xir_effects_root(effects,f);
            if (!actual || actual->requires_root != expected->root || actual->unresolved != expected->unknown)
                fprintf(stderr,"%s expectedroot%u/unknown%u actual%u/%u\n",expected->name,
                    expected->root ? 1u : 0u,expected->unknown ? 1u : 0u,
                    actual && actual->requires_root ? 1u : 0u,actual && actual->unresolved ? 1u : 0u);
            CHECK(actual && actual->requires_root == expected->root && actual->unresolved == expected->unknown);
        }
    } else {
        uint32_t f = root_source_function(module,"worker");
        const XrXirRootEffects *actual = xr_xir_effects_root(effects,f);
        CHECK(actual && !actual->requires_root && !actual->unresolved);
        CHECK(xr_xir_effects_go_safe(effects,f) == XR_XIR_OK);
    }
    xr_xir_compile_effects_free(effects);
}
static void root_source_query(const XrXirSourceView *view) {
    CHECK(view && view->complete && view->types);
    bool exact_inferred = false, exact_fixed = false, exact_nullable = false;
    for (uint32_t d = 0; d < view->declaration_count; ++d) {
        const XrXirSourceDeclaration *record = &view->declarations[d];
        if (record->kind != XR_XIR_SOURCE_BINDING || !record->name) continue;
        if (!strcmp(record->name,"f")) {
            const XrXirTypeNode *type = xr_xir_callable_signature(view->types,record->type.type);
            if (type && type->flags == 2) exact_inferred = true;
            if (type && type->flags == 8) exact_fixed = true;
        } else if (!strcmp(record->name,"maybe")) {
            XrXirType element = xr_xir_nullable_element(view->types,record->type.type);
            const XrXirTypeNode *type = xr_xir_callable_signature(view->types,element);
            if (type && type->flags == 8) exact_nullable = true;
        }
    }
    CHECK(exact_inferred && exact_fixed && exact_nullable);
}
static XrXirStatus root_source_guard_callback(void *opaque, const XrXirEffects *effects, bool *changed) {
    uint32_t *calls = opaque; ++*calls; *changed = false;
    CHECK(effects); return XR_XIR_BAD_TYPE;
}
static void root_source_prepared_guards(const XrXirCompileContext *context, XrXirArtifact *artifact) {
    uint32_t calls = 0; XrXirRootRefiner refiner = {&calls,root_source_guard_callback};
    XrXirArtifact *output = artifact;
    XrCompileResourceStats before = library_compile_stats(context);
    CHECK(xr_xir_compile_check_refined_v2(context,xr_xir_compile_artifact_module(artifact),xr_xir_compile_artifact_construction(artifact),&refiner,&output,NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(output == artifact && !calls);
    XrXirCompileContext invalid = {0};
    CHECK(xr_xir_compile_check_refined_v2(&invalid,NULL,NULL,&refiner,&output,NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(output == artifact && !calls);
    output = NULL;
    CHECK(xr_xir_compile_check_refined_v2(context,xr_xir_compile_artifact_module(artifact),xr_xir_compile_artifact_construction(artifact),&refiner,&output,NULL) == XR_XIR_BAD_STAGE);
    CHECK(!output && !calls);
    XrCompileResourceStats after = library_compile_stats(context);
    CHECK(before.allocated_bytes == after.allocated_bytes && before.live_bytes == after.live_bytes && before.work == after.work);
}
static XrXirStatus root_source_operation(const XrXirCompileContext *context, void *opaque) {
    const RootSourceOperation *fixture = opaque;
    const char *file = fixture->file;
    char path[1024]; CHECK(snprintf(path,sizeof(path),"%s/%s",XR_CALLABLE_ROOT_SOURCE_FIXTURES,file) > 0);
    XrCompilerSession *session = NULL;
    XrCompilerSessionStatus created = xr_compile_session_new(context->resources,&session);
    if (created != XR_COMPILER_SESSION_OK)
        return created == XR_COMPILER_SESSION_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT,NULL,XR_CALLABLE_ROOT_SOURCE_FIXTURES};
    XrXirSourceRequest request = {session,path,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0}; char *failure = NULL;
    XrXirStatus status = xr_xir_compile_source_check(&request,&result,&diagnostic,&failure);
    xr_compile_session_free(session); session = NULL;
    if (status != XR_XIR_OK) {
        CHECK(!result.checked && !result.snapshot);
        if (source_program_compile_fail_at == SIZE_MAX && status != XR_XIR_BUDGET)
            fprintf(stderr,"%s status%u %d:%d %s\n",file,status,diagnostic.line,diagnostic.column,diagnostic.message);
        goto done;
    }
    CHECK(result.checked && result.snapshot && !failure);
    bool flows = !strcmp(file,"flows.xr");
    /* The producer session is already gone. All queried names and identities
     * belong to the snapshot, and replay uses only freshly decoded Checked. */
    if (flows && fixture->qualify) root_source_query(xr_xir_compile_source_snapshot_view(result.snapshot));
    if (fixture->qualify) {
        root_source_prepared_guards(context,result.checked);
        root_source_facts(result.checked,flows);
        XrXirCheckedPacket packet = {0}; XrXirArtifact *decoded = NULL;
        status = xr_xir_compile_checked_write(result.checked,&packet,NULL);
        if (status == XR_XIR_OK) status = xr_xir_compile_checked_read(context,packet.bytes,packet.length,&decoded,NULL);
        if (status == XR_XIR_OK) root_source_facts(decoded,flows);
        xr_xir_compile_artifact_free(decoded); xr_xir_compile_checked_packet_free(&packet);
    }
done:
    xr_compile_resources_free(failure); xr_xir_compile_source_result_free(&result);
    return status;
}
static void root_source_reject(const char *file, XrXirStatus expected, const char *reason) {
    LibraryCompileOwner owner = {0}; CHECK(library_compile_owner_new(&owner,&library_compile_limits) == XR_XIR_OK);
    XrCompilerSession *session = NULL; CHECK(xr_compile_session_new(owner.context.resources,&session) == XR_COMPILER_SESSION_OK);
    char path[1024]; CHECK(snprintf(path,sizeof(path),"%s/%s",XR_CALLABLE_ROOT_SOURCE_FIXTURES,file) > 0);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT,NULL,XR_CALLABLE_ROOT_SOURCE_FIXTURES};
    XrXirSourceRequest request = {session,path,&authority,&owner.context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0}; char *failure = NULL;
    XrXirStatus status = xr_xir_compile_source_check(&request,&result,&diagnostic,&failure);
    if (status != expected) fprintf(stderr,"reject %s status%u %s\n",file,status,diagnostic.message);
    CHECK(status == expected && !result.checked && !result.snapshot);
    if (reason) CHECK(strstr(diagnostic.message,reason));
    xr_compile_resources_free(failure); xr_xir_compile_source_result_free(&result); xr_compile_session_free(session);
    library_compile_owner_drop(&owner);
}
static void root_source_resource_cases(void) {
    RootSourceOperation fixture = {"worker.xr",false};
    LibraryCompileOwner owner = {0}; CHECK(library_compile_owner_new(&owner,&library_compile_limits) == XR_XIR_OK);
    source_program_compile_attempts = 0;
    CHECK(root_source_operation(&owner.context,&fixture) == XR_XIR_OK);
    size_t sites = source_program_compile_attempts;
    XrCompileResourceStats required = library_compile_stats(&owner.context);
    CHECK(sites && required.live_bytes == owner.baseline.live_bytes); library_compile_owner_drop(&owner);
    for (size_t site = 0; site < sites; ++site) {
        CHECK(library_compile_owner_new(&owner,&library_compile_limits) == XR_XIR_OK);
        size_t blocks = source_program_compile_live, bytes = source_program_compile_bytes;
        source_program_compile_attempts = 0; source_program_compile_injected = false; source_program_compile_fail_at = site;
        XrXirStatus status = root_source_operation(&owner.context,&fixture);
        source_program_compile_fail_at = SIZE_MAX;
        if (status != XR_XIR_OUT_OF_MEMORY) fprintf(stderr,"source FI%zu/%zu status%u\n",site,sites,status);
        CHECK(status == XR_XIR_OUT_OF_MEMORY && source_program_compile_injected);
        CHECK(source_program_compile_live == blocks && source_program_compile_bytes == bytes);
        XrCompileResourceStats paid = library_compile_stats(&owner.context);
        CHECK(paid.live_bytes == owner.baseline.live_bytes);
        CHECK(root_source_operation(&owner.context,&fixture) == XR_XIR_OK);
        XrCompileResourceStats retried = library_compile_stats(&owner.context);
        CHECK(retried.allocated_bytes > paid.allocated_bytes && retried.work > paid.work);
        library_compile_owner_drop(&owner);
    }
    for (uint32_t axis = 0; axis < 3; ++axis) for (uint32_t less = 0; less < 2; ++less) {
        XrCompileResourceLimits caps = library_compile_limits;
        uint64_t amount = axis == 0 ? required.allocated_bytes : axis == 1 ? required.peak_bytes : required.work;
        CHECK(amount);
        if (axis == 0) caps.allocated_bytes = amount - less;
        else if (axis == 1) caps.live_bytes = amount - less;
        else caps.work = amount - less;
        XrXirStatus status = library_compile_owner_new(&owner,&caps);
        if (status == XR_XIR_OK) status = root_source_operation(&owner.context,&fixture);
        if (status != (less ? XR_XIR_BUDGET : XR_XIR_OK))
            fprintf(stderr,"source axis%u less%u amount%llu status%u\n",axis,less,(unsigned long long)amount,status);
        CHECK(status == (less ? XR_XIR_BUDGET : XR_XIR_OK)); library_compile_owner_drop(&owner);
    }
    printf("prepared Source %zu actual OOM sites, same-owner retry, exact/minus1 threeaxes, physical0\n",sites);
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1],"--compiler")) {
        root_source_resource_cases();
    } else if (argc == 1) {
        const char *accepted[] = {"flows.xr","worker.xr","worker_tuple.xr","worker_array.xr"};
        for (size_t i = 0; i < sizeof(accepted) / sizeof(*accepted); ++i) {
            LibraryCompileOwner owner = {0}; CHECK(library_compile_owner_new(&owner,&library_compile_limits) == XR_XIR_OK);
            RootSourceOperation fixture = {accepted[i],true};
            CHECK(root_source_operation(&owner.context,&fixture) == XR_XIR_OK);
            library_compile_owner_drop(&owner);
        }
        root_source_reject("worker_root.xr",XR_XIR_BAD_TYPE,"requires the current instance root execution");
        root_source_reject("worker_fixed.xr",XR_XIR_BAD_TYPE,"lacks proof for worker execution");
        root_source_reject("worker_nullable_fixed.xr",XR_XIR_BAD_TYPE,"lacks proof for worker execution");
        root_source_reject("worker_uncaught.xr",XR_XIR_BAD_TYPE,NULL);
        root_source_reject("bad_generic.xr",XR_XIR_BAD_TYPE,NULL);
        root_source_reject("bad_visibility.xr",XR_XIR_BAD_STRUCTURE,"requires an exported declaration");
    } else return 2;
    library_compile_observer_free();
    puts("ordinary references: closed root flow, fixed boundaries, replay and physical0"); return 0;
}
