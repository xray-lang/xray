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
typedef struct RootConditionalOperation { const RootConditionalOracle *oracle; bool inspect, observe; } RootConditionalOperation;
typedef struct RootConditionalStageObservation { XrCompileResourceStats stats; size_t attempts; } RootConditionalStageObservation;
static void conditional_stage_observe(const XrXirCompileContext *context, const RootConditionalOperation *operation,
    uint32_t phase, XrXirStatus status, RootConditionalStageObservation *previous) {
    if (!operation->observe) return;
    XrCompileResourceStats stats=library_compile_stats(context);size_t attempts=source_program_compile_attempts;
    fprintf(stderr,"%s normal_stage phase=%u status=%u sites=%zu delta_sites=%zu allocated=%llu delta_allocated=%llu "
        "live=%llu peak=%llu work=%llu delta_work=%llu\n",operation->oracle->name,phase,(unsigned)status,
        attempts,attempts-previous->attempts,(unsigned long long)stats.allocated_bytes,
        (unsigned long long)(stats.allocated_bytes-previous->stats.allocated_bytes),
        (unsigned long long)stats.live_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work,
        (unsigned long long)(stats.work-previous->stats.work));
    previous->stats=stats;previous->attempts=attempts;
}
static uint32_t conditional_named(const XrXirModule *module, const char *name) {
    size_t size=strlen(name);
    for (uint32_t f=0;f<module->function_count;++f)
        if (module->functions[f].name_length==size && !memcmp(module->functions[f].name,name,size)) return f;
    return UINT32_MAX;
}
/* Negative fixtures borrow the actual live Template; its original bytes and
 * ordinary oracle remain intact. Copied advertisements are test inputs only. */
static void conditional_collector_controls(const XrXirArtifact *artifact,XrXirEffects *occupied) {
    const XrXirCompileContext *context=&artifact->context;
    XrCompileResourceStats before=library_compile_stats(context);
    size_t attempts=source_program_compile_attempts;
    XrXirEffects *output=NULL,*sentinel=occupied;CHECK(occupied);
    CHECK(xr_xir_compile_artifact_verify_owned_effects(NULL,&output,NULL)==XR_XIR_BAD_STRUCTURE && !output);
    CHECK(xr_xir_compile_artifact_verify_owned_effects(artifact,NULL,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(xr_xir_compile_artifact_verify_owned_effects(artifact,&sentinel,NULL)==XR_XIR_BAD_STRUCTURE && sentinel==occupied);
    CHECK(xr_xir_compile_verify_owned_effects_v2(context,&artifact->module,artifact->construction,&sentinel,NULL)==XR_XIR_BAD_STRUCTURE && sentinel==occupied);
    XrCompileResourceStats early=library_compile_stats(context);
    CHECK(early.allocated_bytes==before.allocated_bytes && early.work==before.work && source_program_compile_attempts==attempts);
    size_t blocks=source_program_compile_live,bytes=source_program_compile_bytes;
    CHECK(xr_xir_compile_artifact_verify_owned_effects(artifact,&output,NULL)==XR_XIR_OK && output);
    uint32_t forward=conditional_named(&artifact->module,"forward");CHECK(forward!=UINT32_MAX);
    const XrXirFunctionEffectContract *facts=xir_effects_contract(output,forward);
    CHECK(facts && facts->parameter_count==2 && facts->parameters[1].kind==XR_XIR_EFFECT_PARAMETER_CONDITIONAL_STATIC);
    xr_xir_compile_effects_free(output);output=NULL;
    CHECK(source_program_compile_live==blocks && source_program_compile_bytes==bytes && library_compile_stats(context).live_bytes==before.live_bytes);
    const XrXirProvenance *original=artifact->module.provenance;
    CHECK(original && original->kind==XR_XIR_EVIDENCE_TEMPLATE && original->contract_count==artifact->module.function_count);
    uint64_t contract_bytes=(uint64_t)original->contract_count*sizeof(*original->contracts);
    size_t bounded_bytes=(size_t)contract_bytes;CHECK((uint64_t)bounded_bytes==contract_bytes);
    XrXirFunctionEffectContract *contracts=NULL;
    CHECK(xr_compile_resources_alloc(context->resources,bounded_bytes,(void **)&contracts)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_work(context->resources,contract_bytes)==XR_COMPILE_RESOURCE_OK);
    memcpy(contracts,original->contracts,bounded_bytes);
    contracts[forward].formula.constant_mask^=XR_XIR_CALLABLE_ROOT_REQUIRED;
    XrXirProvenance provenance=*original;provenance.contracts=contracts;
    XrXirArtifact invalid=*artifact;invalid.module.provenance=&provenance;
    /* The advertisement is legal shape but contradicts full actual inference. */
    CHECK(xr_xir_compile_structure_verify_v2(context,&invalid.module,invalid.construction,NULL)==XR_XIR_OK);
    size_t hidden_blocks=source_program_compile_live,hidden_bytes=source_program_compile_bytes;
    uint64_t hidden_live=library_compile_stats(context).live_bytes;attempts=source_program_compile_attempts;
    CHECK(xr_xir_compile_artifact_verify_owned_effects(&invalid,&output,NULL)==XR_XIR_BAD_TYPE && !output);
    CHECK(source_program_compile_attempts>attempts && source_program_compile_live==hidden_blocks && source_program_compile_bytes==hidden_bytes);
    CHECK(library_compile_stats(context).live_bytes==hidden_live);
    xr_compile_resources_free(contracts);invalid=*artifact;
    invalid.target.architecture=XR_XIR_ARCH_X86_64;
    attempts=source_program_compile_attempts;
    /* Complete checking creates the hidden owner before layout rejects it. */
    CHECK(xr_xir_compile_artifact_verify_owned_effects(&invalid,&output,NULL)==XR_XIR_BAD_LAYOUT && !output);
    CHECK(source_program_compile_attempts>attempts && source_program_compile_live==blocks && source_program_compile_bytes==bytes);
    CHECK(library_compile_stats(context).live_bytes==before.live_bytes && sentinel==occupied);
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
    CHECK(context);conditional_collector_controls(artifact,effects);xr_xir_compile_effects_free(effects);
}
static void conditional_closed(const XrXirArtifact *artifact, const RootConditionalOracle *oracle) {
    const XrXirModule *m=xr_xir_compile_artifact_module(artifact);
    CHECK(m && m->provenance && m->provenance->kind==XR_XIR_EVIDENCE_INSTANCE && m->provenance->source);
    XrXirEffects *collector=NULL;XrCompileResourceStats before=library_compile_stats(&artifact->context);
    size_t attempts=source_program_compile_attempts;
    CHECK(xr_xir_compile_artifact_verify_owned_effects(artifact,&collector,NULL)==XR_XIR_BAD_STRUCTURE && !collector);
    XrCompileResourceStats after=library_compile_stats(&artifact->context);
    CHECK(after.allocated_bytes==before.allocated_bytes && after.work==before.work && source_program_compile_attempts==attempts);
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
    RootConditionalStageObservation observation={0};
    if (operation->observe) { observation.stats=library_compile_stats(context);observation.attempts=source_program_compile_attempts; }
    XrCompilerSession *session=NULL;XrCompilerSessionStatus created=xr_compile_session_new(context->resources,&session);
    if (created!=XR_COMPILER_SESSION_OK) return created==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"root-conditional",NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};char *failure=NULL;
    XrXirArtifact *read=NULL,*instance=NULL,*reloaded=NULL,*lowered=NULL;XrXirCheckedPacket first={0},second={0};
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    const XrXirSourceText input={NULL,oracle->text,strlen(oracle->text)};
    uint32_t phase=1;
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&result,&diagnostic,&failure);
    xr_compile_session_free(session);
    conditional_stage_observe(context,operation,1,status,&observation);
    if (status==XR_XIR_OK && operation->inspect) conditional_template(result.checked);
    if (status==XR_XIR_OK) { phase=2;status=xr_xir_compile_checked_write(result.checked,&first,NULL);conditional_stage_observe(context,operation,2,status,&observation); }
    xr_xir_compile_source_result_free(&result);
    if (status==XR_XIR_OK) { phase=3;status=xr_xir_compile_checked_read(context,first.bytes,first.length,&read,NULL);conditional_stage_observe(context,operation,3,status,&observation); }
    if (status==XR_XIR_OK) { phase=4;status=xr_xir_compile_specialize(read,&instance,NULL);conditional_stage_observe(context,operation,4,status,&observation); }
    xr_xir_compile_artifact_free(read);read=NULL;
    if (status==XR_XIR_OK) { phase=5;status=xr_xir_compile_checked_write(instance,&second,NULL);conditional_stage_observe(context,operation,5,status,&observation); }
    xr_xir_compile_artifact_free(instance);instance=NULL;
    if (status==XR_XIR_OK) { phase=6;status=xr_xir_compile_checked_read(context,second.bytes,second.length,&reloaded,NULL);conditional_stage_observe(context,operation,6,status,&observation); }
    if (status==XR_XIR_OK && operation->inspect) conditional_closed(reloaded,oracle);
    if (status==XR_XIR_OK) { phase=7;status=xr_xir_compile_lower(reloaded,&target,&lowered,NULL);conditional_stage_observe(context,operation,7,status,&observation); }
    xr_xir_compile_artifact_free(reloaded);reloaded=NULL;
    if (status==XR_XIR_OK) { phase=8;status=xr_xir_compile_artifact_verify(lowered,NULL);conditional_stage_observe(context,operation,8,status,&observation); }
    if (status!=XR_XIR_OK && source_program_compile_fail_at==SIZE_MAX && status!=XR_XIR_BUDGET)
        fprintf(stderr,"%s phase%u status%u %d:%d %s\n",oracle->name,phase,status,diagnostic.line,diagnostic.column,diagnostic.message);
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(reloaded);xr_xir_compile_artifact_free(instance);
    xr_xir_compile_artifact_free(read);xr_xir_compile_checked_packet_free(&first);xr_xir_compile_checked_packet_free(&second);
    xr_compile_resources_free(failure);conditional_stage_observe(context,operation,9,status,&observation);return status;
}
int main(int argc, char **argv) {
    CHECK(argc<=2);bool census=argc==2 && !strcmp(argv[1],"--census");
    bool compiler=argc==2 && !census;CHECK(!compiler || !strcmp(argv[1],"--compiler"));
    for (size_t i=0;i<sizeof(root_conditional_oracles)/sizeof(root_conditional_oracles[0]);++i) {
        RootConditionalOperation operation={&root_conditional_oracles[i],!compiler && !census,census};
        if (compiler) library_compile_operation_cases(operation.oracle->name,conditional_operation,&operation);
        else {
            LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
            source_program_compile_attempts=0;
            CHECK(conditional_operation(&owner.context,&operation)==XR_XIR_OK);
            if (census) {
                XrCompileResourceStats stats=library_compile_stats(&owner.context);
                fprintf(stderr,"%s normal_census sites=%zu allocated=%llu peak=%llu work=%llu\n",
                    operation.oracle->name,source_program_compile_attempts,(unsigned long long)stats.allocated_bytes,
                    (unsigned long long)stats.peak_bytes,(unsigned long long)stats.work);
            }
            library_compile_owner_drop(&owner);
        }
    }
    library_compile_observer_free();return 0;
}
