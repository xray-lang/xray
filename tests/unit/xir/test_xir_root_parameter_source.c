/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_root_parameter_source.c - Source actual bindings through Checked
 */
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
     "fn apply(callback:fn()->i64)->i64{try{return callback()}catch(error){return 0}}\n"
     "fn capture(callback:fn()->i64)->i64{const thunk=fn()->i64{try{return callback()}catch(error){return 0}};return apply(thunk)}\n"
     "fn pure()->i64{return 7}\n"
     "export fn run()->i64{return capture(pure)}\nprint(run())\n",false,false,7,"7\n",2},
    {"capture-root.xr",
     "var state:i64=9\n"
     "fn apply(callback:fn()->i64)->i64{try{return callback()}catch(error){return 0}}\n"
     "fn capture(callback:fn()->i64)->i64{const thunk=fn()->i64{try{return callback()}catch(error){return 0}};return apply(thunk)}\n"
     "fn rooted()->i64{return state}\n"
     "export fn run()->i64{return capture(rooted)}\nprint(run())\n",true,false,9,"9\n",2},
    {"capture-fixed.xr",
     "fn apply(callback:fn()->i64)->i64{try{return callback()}catch(error){return 0}}\n"
     "fn capture(callback:fn()->i64)->i64{const thunk=fn()->i64{try{return callback()}catch(error){return 0}};return apply(thunk)}\n"
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
    CHECK(c && c->parameter_count==1 && c->parameters[0].kind==1 && c->parameters[0].uses==1);
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
    const XrXirRootEffects *root=xr_xir_effects_root(effects,destination);
    CHECK(root && root->requires_root==oracle->root && root->unresolved==oracle->unresolved);
    bool default_bound=false,actual_bound=false;
    uint32_t apply=rps_named(source,"apply");CHECK(apply!=UINT32_MAX);
    for (uint32_t f=0;f<m->function_count;++f) {
        const XrXirOrigin *origin=&m->provenance->origins[f];
        if (origin->function!=apply) continue;
        CHECK(origin->effect_argument_count==1 && origin->effect_arguments[0].parameter==0);
        const XrXirTypeNode *signature=xr_xir_callable_signature(m->types,origin->effect_arguments[0].type);
        CHECK(signature);
        if (signature->flags==8) default_bound=true;
        if (signature->flags==(oracle->unresolved?8u:oracle->root?4u:2u)) actual_bound=true;
    }
    CHECK(default_bound && actual_bound);xr_xir_compile_effects_free(effects);
}
static XrXirStatus rps_operation(const XrXirCompileContext *context, void *opaque) {
    const RootParameterSourceOperation *operation=opaque;
    const RootParameterSourceOracle *oracle=operation->oracle;
    char path[1024];CHECK(snprintf(path,sizeof(path),"%s/%s",XR_ROOT_PARAMETER_SOURCE_FIXTURES,oracle->file)>0);
    XrCompilerSession *session=NULL;
    XrCompilerSessionStatus created=xr_compile_session_new(context->resources,&session);
    if (created!=XR_COMPILER_SESSION_OK)
        return created==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_ROOT_PARAMETER_SOURCE_FIXTURES};
    XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};char *failure=NULL;
    XrXirArtifact *read=NULL,*instance=NULL,*lowered=NULL;XrXirCheckedPacket packet={0};
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,&failure);
    xr_compile_session_free(session);
    if (status==XR_XIR_OK) {
        CHECK(result.checked && result.snapshot && !failure);
        if (operation->qualify) rps_template(result.checked);
        status=xr_xir_compile_checked_write(result.checked,&packet,NULL);
    }
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&read,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_specialize(read,&instance,NULL);
    if (status==XR_XIR_OK) {
        if (operation->qualify) rps_instance(instance,oracle);
        status=xr_xir_compile_lower(instance,&target,&lowered,NULL);
    }
    if (status==XR_XIR_OK) { CHECK(lowered);status=xr_xir_compile_artifact_verify(lowered,NULL); }
    if (status!=XR_XIR_OK && source_program_compile_fail_at==SIZE_MAX && status!=XR_XIR_BUDGET)
        fprintf(stderr,"%s status%u %d:%d %s\n",oracle->file,status,diagnostic.line,diagnostic.column,diagnostic.message);
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(instance);xr_xir_compile_artifact_free(read);
    xr_xir_compile_checked_packet_free(&packet);xr_compile_resources_free(failure);
    xr_xir_compile_source_result_free(&result);return status;
}
int main(int argc, char **argv) {
    CHECK(argc<=2);
    bool resources=argc==2;CHECK(!resources || !strcmp(argv[1],"--compiler"));
    for (size_t i=0;i<sizeof(rps_oracles)/sizeof(rps_oracles[0]);++i) rps_write(&rps_oracles[i]);
    for (size_t i=0;i<sizeof(rps_capture_oracles)/sizeof(rps_capture_oracles[0]);++i) rps_write(&rps_capture_oracles[i]);
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
