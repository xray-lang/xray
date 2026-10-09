/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_root_conditional_source.c - Conditional witnesses through owned XIR
 *
 * KEY CONCEPT:
 *   Definition facts remain open. A concrete ordinary witness closes a real
 *   effect environment before a reader or execution consumer may use it.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_effects.h"
#include "xir/xxir_internal.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_library_compile_owner.h"
#include "xir_root_conditional_oracles.h"
typedef struct RootConditionalOperation { const RootConditionalOracle *oracle; bool inspect; } RootConditionalOperation;
static uint32_t conditional_named(const XrXirModule *module, const char *name) {
    size_t size=strlen(name);
    for (uint32_t f=0;f<module->function_count;++f)
        if (module->functions[f].name_length==size && !memcmp(module->functions[f].name,name,size)) return f;
    return UINT32_MAX;
}
static void conditional_template(const XrXirArtifact *artifact) {
    const XrXirModule *m=xr_xir_compile_artifact_module(artifact);
    CHECK(m && m->stage==XR_XIR_CHECKED && m->provenance && m->provenance->kind==XR_XIR_EVIDENCE_TEMPLATE);
    uint32_t forward=conditional_named(m,"forward");CHECK(forward!=UINT32_MAX);
    XrXirEffects *effects=NULL;CHECK(xr_xir_compile_effects_analyze(artifact,&effects)==XR_XIR_OK && effects);
    const XrXirFunctionEffectContract *contract=xir_effects_contract(effects,forward);
    CHECK(contract && contract->parameter_count==2 && contract->parameters[1].kind==XR_XIR_EFFECT_PARAMETER_CONDITIONAL_STATIC);
    bool context=false;
    for (uint32_t t=0;t<contract->formula.term_count;++t) {
        const XrXirRootTerm *term=&contract->formula.terms[t];
        if (term->kind!=XR_XIR_ROOT_TERM_CONTEXT_CALL) continue;
        CHECK(term->index<m->functions[forward].instruction_count);
        CHECK(m->functions[forward].instructions[term->index].op==XR_XIR_CALL_REQUIREMENT);context=true;
    }
    CHECK(context);xr_xir_compile_effects_free(effects);
}
static void conditional_closed(const XrXirArtifact *artifact, const RootConditionalOracle *oracle) {
    const XrXirModule *m=xr_xir_compile_artifact_module(artifact);
    CHECK(m && m->provenance && m->provenance->kind==XR_XIR_EVIDENCE_INSTANCE && m->provenance->source);
    const XrXirModule *source=xr_xir_compile_artifact_module(m->provenance->source);
    uint32_t original=conditional_named(source,"run"),run=UINT32_MAX;CHECK(original!=UINT32_MAX);
    for (uint32_t f=0;f<m->function_count;++f)
        if (m->provenance->origins[f].function==original) { CHECK(run==UINT32_MAX);run=f; }
    CHECK(run!=UINT32_MAX);XrXirEffects *effects=NULL;
    CHECK(xr_xir_compile_effects_analyze(artifact,&effects)==XR_XIR_OK && effects);
    const XrXirRootEffects *facts=xr_xir_effects_root(effects,run);
    CHECK(facts && facts->requires_root==oracle->root && facts->unresolved==oracle->unresolved);
    for (uint32_t f=0;f<m->function_count;++f) for (uint32_t i=0;i<m->functions[f].instruction_count;++i)
        CHECK(m->functions[f].instructions[i].op!=XR_XIR_CALL_REQUIREMENT);
    xr_xir_compile_effects_free(effects);
}
static XrXirStatus conditional_operation(const XrXirCompileContext *context, void *opaque) {
    const RootConditionalOperation *operation=opaque;const RootConditionalOracle *oracle=operation->oracle;
    XrCompilerSession *session=NULL;XrCompilerSessionStatus created=xr_compile_session_new(context->resources,&session);
    if (created!=XR_COMPILER_SESSION_OK) return created==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"root-conditional",NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};char *failure=NULL;
    XrXirArtifact *read=NULL,*instance=NULL,*reloaded=NULL,*lowered=NULL;XrXirCheckedPacket first={0},second={0};
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    const XrXirSourceText input={NULL,oracle->text,strlen(oracle->text)};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&result,&diagnostic,&failure);
    xr_compile_session_free(session);
    if (status==XR_XIR_OK && operation->inspect) conditional_template(result.checked);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_write(result.checked,&first,NULL);
    xr_xir_compile_source_result_free(&result);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_read(context,first.bytes,first.length,&read,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_specialize(read,&instance,NULL);
    xr_xir_compile_artifact_free(read);read=NULL;
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_write(instance,&second,NULL);
    xr_xir_compile_artifact_free(instance);instance=NULL;
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_read(context,second.bytes,second.length,&reloaded,NULL);
    if (status==XR_XIR_OK && operation->inspect) conditional_closed(reloaded,oracle);
    if (status==XR_XIR_OK) status=xr_xir_compile_lower(reloaded,&target,&lowered,NULL);
    xr_xir_compile_artifact_free(reloaded);reloaded=NULL;
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(lowered,NULL);
    if (status!=XR_XIR_OK && source_program_compile_fail_at==SIZE_MAX && status!=XR_XIR_BUDGET)
        fprintf(stderr,"%s status%u %d:%d %s\n",oracle->name,status,diagnostic.line,diagnostic.column,diagnostic.message);
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(reloaded);xr_xir_compile_artifact_free(instance);
    xr_xir_compile_artifact_free(read);xr_xir_compile_checked_packet_free(&first);xr_xir_compile_checked_packet_free(&second);
    xr_compile_resources_free(failure);return status;
}
int main(int argc, char **argv) {
    CHECK(argc<=2);bool compiler=argc==2;CHECK(!compiler || !strcmp(argv[1],"--compiler"));
    for (size_t i=0;i<sizeof(root_conditional_oracles)/sizeof(root_conditional_oracles[0]);++i) {
        RootConditionalOperation operation={&root_conditional_oracles[i],!compiler};
        if (compiler) library_compile_operation_cases(operation.oracle->name,conditional_operation,&operation);
        else {
            LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
            CHECK(conditional_operation(&owner.context,&operation)==XR_XIR_OK);library_compile_owner_drop(&owner);
        }
    }
    library_compile_observer_free();return 0;
}
