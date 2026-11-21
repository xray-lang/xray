/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_root_parameter_source.c - Source actual bindings through Checked
 */
#include "xir/xxir_compile_memory.h"
#include "xir/xxir_source.h"
#include "xir/xxir_effects.h"
#include "xir/xxir_types.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_internal.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_library_compile_owner.h"
#include "xir_root_parameter_oracles.h"
typedef struct RootParameterSourceOperation {
    const RootParameterSourceOracle *oracle;
    bool qualify;
} RootParameterSourceOperation;
static const RootParameterSourceOracle rps_capture_oracles[]={
    {"capture-pure.xr",
     "fn apply(callback:fn()->i64)->i64{return callback()}\n"
     "fn capture(callback:fn()->i64)->i64{const thunk=fn()->i64{return callback()};return apply(thunk)}\n"
     "fn pure()->i64{return 7}\n"
     "export fn run()->i64{return capture(pure)}\nprint(run())\n",false,false,7,"7\n",2},
    {"capture-root.xr",
     "var state:i64=9\n"
     "fn apply(callback:fn()->i64)->i64{return callback()}\n"
     "fn capture(callback:fn()->i64)->i64{const thunk=fn()->i64{return callback()};return apply(thunk)}\n"
     "fn rooted()->i64{return state}\n"
     "export fn run()->i64{return capture(rooted)}\nprint(run())\n",true,false,9,"9\n",2},
    {"capture-fixed.xr",
     "fn apply(callback:fn()->i64)->i64{return callback()}\n"
     "fn capture(callback:fn()->i64)->i64{const thunk=fn()->i64{return callback()};return apply(thunk)}\n"
     "fn pure()->i64{return 7}\n"
     "export fn run()->i64{const fixed:fn()->i64=pure;return capture(fixed)}\nprint(run())\n",false,true,7,"7\n",2}
};

static void rps_write(const RootParameterSourceOracle *oracle) {
    char path[1024];CHECK(snprintf(path,sizeof(path),"%s/%s",XR_ROOT_PARAMETER_SOURCE_FIXTURES,oracle->file)>0);
    FILE *file=NULL;
#ifdef _MSC_VER
    CHECK(fopen_s(&file,path,"wb")==0 && file);
#else
    file=fopen(path,"wb");CHECK(file);
#endif
    size_t bytes=strlen(oracle->text);
    CHECK(fwrite(oracle->text,1,bytes,file)==bytes && fclose(file)==0);
}
static uint32_t rps_named(const XrXirModule *module, const char *name) {
    size_t length=strlen(name);
    for (uint32_t f=0;f<module->function_count;++f)
        if (module->functions[f].name_length==length && !memcmp(module->functions[f].name,name,length)) return f;
    return UINT32_MAX;
}
static void rps_template(XrXirArtifact *artifact) {
    const XrXirModule *m=xr_xir_compile_artifact_module(artifact);
    CHECK(m && m->stage==XR_XIR_CHECKED && m->provenance && m->provenance->kind==1);
    uint32_t apply=rps_named(m,"apply");CHECK(apply!=UINT32_MAX);
    XrXirEffects *effects=NULL;
    CHECK(xr_xir_compile_effects_analyze(artifact,&effects)==XR_XIR_OK && effects);
    const XrXirFunctionEffectContract *c=xir_effects_contract(effects,apply);
    CHECK(c && c->parameter_count==1 && c->parameters[0].kind==1);
    CHECK(c->formula.constant_mask==0 && c->formula.term_count==1 &&
        c->formula.terms[0].kind==1 && c->formula.terms[0].index==0);
    xr_xir_compile_effects_free(effects);
}
static void rps_instance(XrXirArtifact *artifact, const RootParameterSourceOracle *oracle) {
    const XrXirModule *m=xr_xir_compile_artifact_module(artifact);
    CHECK(m && m->provenance && m->provenance->kind==2 && m->provenance->source);
    const XrXirModule *source=xr_xir_compile_artifact_module(m->provenance->source);
    uint32_t run=rps_named(source,"run");CHECK(run!=UINT32_MAX);
    uint32_t destination=UINT32_MAX;
    for (uint32_t f=0;f<m->function_count;++f)
        if (m->provenance->origins[f].function==run && !m->provenance->origins[f].argument_count &&
            !m->provenance->origins[f].effect_argument_count) destination=f;
    CHECK(destination!=UINT32_MAX);
    XrXirEffects *effects=NULL;
    CHECK(xr_xir_compile_effects_analyze(artifact,&effects)==XR_XIR_OK && effects);
    XrXirEffectFacts root_read=xr_xir_effects_read(effects,destination);
    const XrXirRootEffects *root=(root_read.status==XR_XIR_OK?&root_read.root:NULL);
    CHECK(root && root->requires_root==oracle->root && root->unresolved==oracle->unresolved);
    uint32_t instances=0;
    uint32_t apply=rps_named(source,"apply");CHECK(apply!=UINT32_MAX);
    for (uint32_t f=0;f<m->function_count;++f) {
        const XrXirOrigin *origin=&m->provenance->origins[f];
        if (origin->function!=apply) continue;
        CHECK(!origin->effect_argument_count && !origin->effect_arguments && !origin->argument_count);
        ++instances;
    }
    CHECK(instances==1);xr_xir_compile_effects_free(effects);
}
static XrXirStatus rps_operation(const XrXirCompileContext *context, void *opaque) {
    const RootParameterSourceOperation *operation=opaque;
    const RootParameterSourceOracle *oracle=operation->oracle;
    char path[1024];CHECK(snprintf(path,sizeof(path),"%s/%s",XR_ROOT_PARAMETER_SOURCE_FIXTURES,oracle->file)>0);
    XrCompilerSession *session=NULL;
    XrCompileResourceStatus created=xr_compile_session_new(context->resources,&session);
    if (created!=XR_COMPILE_RESOURCE_OK)
        return xir_compile_resource_status(created);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_ROOT_PARAMETER_SOURCE_FIXTURES};
    XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL, XR_XIR_HOST_TARGET};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};XrXirSourceFailure failure={0};
    XrXirArtifact *read=NULL,*instance=NULL,*lowered=NULL;XrXirCheckedPacket packet={0};
    const XrXirTarget target=XR_XIR_HOST_TARGET;
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,&failure);
    xr_compile_session_free(session);
    if (status==XR_XIR_OK) {
        CHECK(result.checked && result.snapshot && !failure.path);
        if (operation->qualify) rps_template(result.checked);
        status=xr_xir_compile_checked_write(result.checked,&packet,NULL);
    }
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&read,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_specialize(read,&instance,NULL, NULL);
    if (status==XR_XIR_OK) {
        if (operation->qualify) rps_instance(instance,oracle);
        status=xr_xir_compile_lower(instance,&target, NULL,&lowered,NULL);
    }
    if (status==XR_XIR_OK) { CHECK(lowered);status=xr_xir_compile_artifact_verify(lowered,NULL); }
    if (status!=XR_XIR_OK && source_program_compile_fail_at==SIZE_MAX && status!=XR_XIR_BUDGET)
        fprintf(stderr,"%s status%u %d:%d %s\n",oracle->file,status,diagnostic.line,diagnostic.column,diagnostic.message);
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(instance);xr_xir_compile_artifact_free(read);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_source_failure_free(&failure);
    xr_xir_compile_source_result_free(&result);return status;
}
enum { RPS_COMPILER_WORKERS=2, RPS_COMPILER_FIXTURES=2 };
static RootParameterSourceOperation rps_compiler_operation(unsigned fixture) {
    CHECK(fixture<RPS_COMPILER_FIXTURES);
    return (RootParameterSourceOperation){fixture?&rps_capture_oracles[0]:&rps_oracles[0],false};
}
static void rps_write_all(void) {
    for(size_t i=0;i<sizeof(rps_oracles)/sizeof(rps_oracles[0]);++i)rps_write(&rps_oracles[i]);
    for(size_t i=0;i<sizeof(rps_capture_oracles)/sizeof(rps_capture_oracles[0]);++i)rps_write(&rps_capture_oracles[i]);
}
static void rps_compiler_physical(FILE *report) {
    CHECK(!source_program_compile_live && !source_program_compile_bytes);
    CHECK(fprintf(report,"\"blocks\":%zu,\"bytes\":%zu}\n",source_program_compile_live,source_program_compile_bytes)>0);
}
static size_t rps_compiler_census(FILE *report,unsigned fixture,XrCompileResourceStats *required) {
    RootParameterSourceOperation operation=rps_compiler_operation(fixture);
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    source_program_compile_attempts=0;CHECK(rps_operation(&owner.context,&operation)==XR_XIR_OK);
    size_t sites=source_program_compile_attempts;CHECK(sites);
    *required=library_compile_stats(&owner.context);uint64_t baseline=owner.baseline.held_bytes;
    CHECK(required->held_bytes==baseline && required->peak_bytes>0 && required->peak_bytes<UINT64_MAX);
    library_compile_owner_drop(&owner);
    CHECK(fprintf(report,"{\"kind\":\"census\",\"fixture\":%u,\"file\":\"%s\",\"sites\":%zu,"
        "\"allocated\":%llu,\"peak\":%llu,\"allocations\":%llu,\"baseline\":%llu,\"held\":%llu,",
        fixture,operation.oracle->file,sites,(unsigned long long)required->allocated_bytes,
        (unsigned long long)required->peak_bytes,(unsigned long long)required->allocation_count,
        (unsigned long long)baseline,(unsigned long long)required->held_bytes)>0);
    rps_compiler_physical(report);CHECK(fflush(report)==0);return sites;
}
static size_t rps_compiler_faults(FILE *report,unsigned fixture,size_t sites,unsigned index) {
    RootParameterSourceOperation operation=rps_compiler_operation(fixture);size_t points=0;
    for(size_t site=index;site<sites;) {
        LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
        source_program_compile_attempts=0;source_program_compile_fail_at=site;source_program_compile_injected=false;
        XrXirStatus status=rps_operation(&owner.context,&operation);source_program_compile_fail_at=SIZE_MAX;
        size_t attempts=source_program_compile_attempts;bool injected=source_program_compile_injected;
        if(status!=XR_XIR_OUT_OF_MEMORY)fprintf(stderr,"%s compiler fault %zu/%zu status%u\n",operation.oracle->file,site,sites,status);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && injected && attempts>site);
        XrCompileResourceStats stats=library_compile_stats(&owner.context);uint64_t baseline=owner.baseline.held_bytes;
        CHECK(stats.held_bytes==baseline);library_compile_owner_drop(&owner);
        CHECK(fprintf(report,"{\"kind\":\"point\",\"fixture\":%u,\"ordinal\":%zu,\"attempts\":%zu,"
            "\"status\":%u,\"injected\":true,\"baseline\":%llu,\"held\":%llu,",fixture,site,attempts,status,
            (unsigned long long)baseline,(unsigned long long)stats.held_bytes)>0);
        rps_compiler_physical(report);++points;
        if(sites-site<=RPS_COMPILER_WORKERS)break;
        site+=RPS_COMPILER_WORKERS;
    }
    return points;
}
static void rps_compiler_bounds(FILE *report,unsigned fixture,const XrCompileResourceStats *required) {
    RootParameterSourceOperation operation=rps_compiler_operation(fixture);
    for(unsigned boundary=0;boundary<3;++boundary) {
        XrCompileResourceLimits limits=library_compile_limits;limits.memory_limit=required->peak_bytes-1+boundary;
        LibraryCompileOwner owner={0};XrXirStatus status=library_compile_owner_new(&owner,&limits);
        if(status==XR_XIR_OK)status=rps_operation(&owner.context,&operation);
        CHECK(status==(boundary==0?XR_XIR_MEMORY_LIMIT:XR_XIR_OK));
        library_compile_owner_drop(&owner);
        CHECK(fprintf(report,"{\"kind\":\"boundary\",\"fixture\":%u,\"boundary\":%u,\"limit\":%llu,"
            "\"status\":%u,",fixture,boundary,(unsigned long long)limits.memory_limit,status)>0);
        rps_compiler_physical(report);
    }
}
static bool rps_compiler_dispatch(int argc,char **argv) {
    if(argc<2)return false;
    bool census=!strcmp(argv[1],"--compiler-census"),shard=!strcmp(argv[1],"--compiler-shard");
    bool bounds=!strcmp(argv[1],"--compiler-bounds");if(!census && !shard && !bounds)return false;
    CHECK(argc==(shard?5:4));unsigned index=0;
    if(shard){CHECK(strlen(argv[2])==1 && (argv[2][0]=='0' || argv[2][0]=='1'));index=(unsigned)(argv[2][0]-'0');}
    const char *nonce=argv[argc-2];CHECK(strlen(nonce)==32);
    for(size_t n=0;n<32;++n)CHECK((nonce[n]>='0' && nonce[n]<='9') || (nonce[n]>='a' && nonce[n]<='f'));
    FILE *report=NULL;
#ifdef _MSC_VER
    CHECK(fopen_s(&report,argv[argc-1],"wb")==0 && report);
#else
    report=fopen(argv[argc-1],"wb");CHECK(report);
#endif
    const char *mode=census?"census":shard?"shard":"bounds";
    CHECK(fprintf(report,"{\"kind\":\"identity\",\"mode\":\"%s\",\"index\":%u,\"workers\":%u,"
        "\"nonce\":\"%s\",\"memory_limit\":%llu}\n",mode,index,(unsigned)RPS_COMPILER_WORKERS,nonce,
        (unsigned long long)library_compile_limits.memory_limit)>0);
    /* Only the parent's completed census prepares files. Parallel trials must
     * never truncate the same Source input while another session reads it. */
    if(census)rps_write_all();
    size_t total=0;
    for(unsigned fixture=0;fixture<RPS_COMPILER_FIXTURES;++fixture) {
        XrCompileResourceStats required={0};size_t sites=rps_compiler_census(report,fixture,&required);
        size_t points=shard?rps_compiler_faults(report,fixture,sites,index):0;
        if(bounds)rps_compiler_bounds(report,fixture,&required);
        CHECK(points<=SIZE_MAX-total);total+=points;
        CHECK(fprintf(report,"{\"kind\":\"complete\",\"fixture\":%u,\"points\":%zu,\"boundaries\":%u,",
            fixture,points,bounds?3u:0u)>0);rps_compiler_physical(report);CHECK(fflush(report)==0);
    }
    library_compile_observer_free();
    CHECK(fprintf(report,"{\"kind\":\"finished\",\"points\":%zu,\"boundaries\":%u,",total,bounds?6u:0u)>0);
    rps_compiler_physical(report);CHECK(!ferror(report) && fclose(report)==0);return true;
}
int main(int argc, char **argv) {
    if(rps_compiler_dispatch(argc,argv))return 0;
    CHECK(argc<=2);
    bool resources=argc==2;CHECK(!resources || !strcmp(argv[1],"--compiler"));
    rps_write_all();
    RootParameterSourceOperation operation={&rps_oracles[0],!resources};
    if (resources) {
        library_compile_operation_cases("H1 Source apply full same-ledger pipeline",rps_operation,&operation);
        operation.oracle=&rps_capture_oracles[0];
        library_compile_operation_cases("H1 Source capture full same-ledger pipeline",rps_operation,&operation);
    } else {
        for (size_t i=0;i<sizeof(rps_oracles)/sizeof(rps_oracles[0]);++i) {
            LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
            operation.oracle=&rps_oracles[i];
            CHECK(rps_operation(&owner.context,&operation)==XR_XIR_OK);library_compile_owner_drop(&owner);
        }
        for (size_t i=0;i<sizeof(rps_capture_oracles)/sizeof(rps_capture_oracles[0]);++i) {
            LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
            operation.oracle=&rps_capture_oracles[i];
            CHECK(rps_operation(&owner.context,&operation)==XR_XIR_OK);library_compile_owner_drop(&owner);
        }
    }
    library_compile_observer_free();return 0;
}
