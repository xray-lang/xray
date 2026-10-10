/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_effect_value_refine.c - Real producer identities after Cell joins
 *
 * KEY CONCEPT:
 *   Inferred Cell inputs normalize to their actual common bound. Explicit
 *   advertisements and consuming CALL_BIND declarations retain their roles.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_effects.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_types.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_library_compile_owner.h"

static const char source_effect_value_text[] =
    "var state:i64=9\n"
    "fn plain()->i64{return 7}\n"
    "fn rooted()->i64{return state}\n"
    "fn apply(callback:fn()->i64)->i64{return callback()}\n"
    "export fn run()->i64{var empty:()=();const pair=(plain,empty);const heldPair=pair;"
    "const fixed:fn()->i64=plain;var callback=plain;const kept=callback;callback=rooted;"
    "return apply(kept)+fixed()+heldPair.0()}\nprint(run())\n";

/* Complete public checking must still reject the old stale mode. Copied
 * contracts are negative test input, never prepared permission for Source. */
static void source_effect_value_negative(const XrXirArtifact *artifact, uint32_t f, uint32_t v) {
    const XrXirModule *module=&artifact->module;
    const XrXirProvenance *original=module->provenance;
    uint64_t contract_bytes=(uint64_t)original->contract_count*sizeof(*original->contracts);
    uint64_t value_bytes=(uint64_t)original->contracts[f].value_count*sizeof(XrXirRootValueIdentity);
    CHECK((uint64_t)(size_t)contract_bytes==contract_bytes && (uint64_t)(size_t)value_bytes==value_bytes);
    XrXirFunctionEffectContract *contracts=NULL;XrXirRootValueIdentity *values=NULL;
    CHECK(xr_compile_resources_alloc(artifact->context.resources,(size_t)contract_bytes,(void **)&contracts)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_alloc(artifact->context.resources,(size_t)value_bytes,(void **)&values)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_work(artifact->context.resources,contract_bytes+value_bytes)==XR_COMPILE_RESOURCE_OK);
    memcpy(contracts,original->contracts,(size_t)contract_bytes);
    memcpy(values,original->contracts[f].values,(size_t)value_bytes);
    CHECK(values[v].mode==XR_XIR_EFFECT_VALUE_FIXED);
    XrXirRootValueIdentity preserved=original->contracts[f].values[v];
    values[v].mode=XR_XIR_EFFECT_VALUE_PROPAGATE;contracts[f].values=values;
    XrXirProvenance provenance=*original;provenance.contracts=contracts;
    XrXirArtifact invalid=*artifact;invalid.module.provenance=&provenance;
    CHECK(xr_xir_compile_artifact_verify(&invalid,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(!memcmp(&preserved,&original->contracts[f].values[v],sizeof(preserved)));
    xr_compile_resources_free(values);xr_compile_resources_free(contracts);
}

static void source_effect_value_inspect(const XrXirArtifact *artifact) {
    const XrXirModule *module=&artifact->module;
    const XrXirProvenance *p=module->provenance;
    CHECK(p && p->kind==XR_XIR_EVIDENCE_TEMPLATE && p->contract_count==module->function_count);
    uint32_t counts[5]={0}, fixed_function=UINT32_MAX, fixed_value=UINT32_MAX;
    bool explicit_unknown=false, run=false;
    for(uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *body=&module->functions[f];const XrXirFunctionEffectContract *contract=&p->contracts[f];
        bool current_run=body->name_length==3 && !memcmp(body->name,"run",3);
        if(current_run){CHECK(!run);run=true;}
        for(uint32_t v=0;v<contract->value_count;++v) {
            const XrXirRootValueIdentity *value=&contract->values[v];
            CHECK(value->instruction<body->instruction_count && value->mode>=1 && value->mode<=4);
            CHECK(!v || contract->values[v-1].instruction<value->instruction);
            const XrXirInstruction *op=&body->instructions[value->instruction];
            const XrXirTypeNode *declared=xr_xir_callable_signature(module->types,value->declared_type);
            CHECK(declared && xr_xir_callable_signature(module->types,op->type));
            if(current_run)++counts[value->mode];
            if(value->mode==XR_XIR_EFFECT_VALUE_FIXED) {
                CHECK(op->op==XR_XIR_FUNCTION_WEAKEN && value->declared_type==op->type);
                if(current_run){
                    if(declared->flags&XR_XIR_CALLABLE_ROOT_UNRESOLVED)explicit_unknown=true;
                    fixed_function=f;fixed_value=v;
                }
            } else if(value->mode==XR_XIR_EFFECT_VALUE_AUTHENTIC_REF) {
                CHECK(op->op==XR_XIR_FUNCTION_REF && value->declared_type==op->type);
            } else if(value->mode==XR_XIR_EFFECT_VALUE_CALL_BIND) {
                CHECK(op->op==XR_XIR_COPY);
                CHECK(xr_xir_compile_callable_weakening(&artifact->context,module->types,op->type,value->declared_type)==XR_XIR_OK);
            } else CHECK(op->op!=XR_XIR_FUNCTION_WEAKEN);
        }
    }
    CHECK(run && counts[1] && counts[2] && counts[3] && counts[4] && explicit_unknown);
    CHECK(fixed_function!=UINT32_MAX && fixed_value!=UINT32_MAX);
    source_effect_value_negative(artifact,fixed_function,fixed_value);
}

/* One finite owner covers Source, both writers/readers, specialization,
 * Lowered and full public verification. Every producer dies before its reader. */
static XrXirStatus source_effect_value_operation(const XrXirCompileContext *context, void *opaque) {
    bool inspect=*(bool *)opaque;XrCompilerSession *session=NULL;
    XrCompilerSessionStatus opened=xr_compile_session_new(context->resources,&session);
    if(opened!=XR_COMPILER_SESSION_OK)return opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"source-effect-value-refine",NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={NULL,source_effect_value_text,sizeof(source_effect_value_text)-1};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};char *failure=NULL;
    XrXirArtifact *read=NULL,*instance=NULL,*reloaded=NULL,*lowered=NULL;
    XrXirCheckedPacket first={0},second={0};XrXirDiagnostic location={0};uint32_t phase=1;
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&result,&diagnostic,&failure);
    xr_compile_session_free(session);
    if(status==XR_XIR_OK && inspect)source_effect_value_inspect(result.checked);
    if(status==XR_XIR_OK){phase=2;status=xr_xir_compile_checked_write(result.checked,&first,&location);}
    xr_xir_compile_source_result_free(&result);
    if(status==XR_XIR_OK){phase=3;status=xr_xir_compile_checked_read(context,first.bytes,first.length,&read,&location);}
    if(status==XR_XIR_OK){phase=4;status=xr_xir_compile_specialize(read,&instance,&location);}
    xr_xir_compile_artifact_free(read);read=NULL;
    if(status==XR_XIR_OK){phase=5;status=xr_xir_compile_checked_write(instance,&second,&location);}
    xr_xir_compile_artifact_free(instance);instance=NULL;
    if(status==XR_XIR_OK){phase=6;status=xr_xir_compile_checked_read(context,second.bytes,second.length,&reloaded,&location);}
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    if(status==XR_XIR_OK){phase=7;status=xr_xir_compile_lower(reloaded,&target,&lowered,&location);}
    xr_xir_compile_artifact_free(reloaded);reloaded=NULL;
    if(status==XR_XIR_OK){phase=8;status=xr_xir_compile_artifact_verify(lowered,&location);}
    if(status!=XR_XIR_OK && source_program_compile_fail_at==SIZE_MAX && status!=XR_XIR_BUDGET)
        fprintf(stderr,"effect-value-refine phase%u status%u %d:%d %s f%u/b%u/i%u\n",phase,(unsigned)status,
            diagnostic.line,diagnostic.column,diagnostic.message,location.function,location.block,location.instruction);
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(reloaded);xr_xir_compile_artifact_free(instance);
    xr_xir_compile_artifact_free(read);xr_xir_compile_checked_packet_free(&first);xr_xir_compile_checked_packet_free(&second);
    xr_compile_resources_free(failure);return status;
}

int main(int argc, char **argv) {
    CHECK(argc==1 || (argc==2 && !strcmp(argv[1],"--compiler")));
    bool inspect=argc==1;
    if(!inspect)library_compile_operation_cases("source-effect-value-refine",source_effect_value_operation,&inspect);
    else {
        LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
        CHECK(source_effect_value_operation(&owner.context,&inspect)==XR_XIR_OK);library_compile_owner_drop(&owner);
    }
    library_compile_observer_free();return 0;
}
