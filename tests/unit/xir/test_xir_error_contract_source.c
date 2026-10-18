/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_error_contract_source.c - Independent root and escaping-error facts
 *
 * KEY CONCEPT:
 *   Real Source and Checked replay retain independent root bounds and exact
 *   error outcomes after all producer storage has died.
 */
#include "xir/xxir_internal.h"
#include "xir/xxir_source.h"
#include "xir/xxir_source_query.h"
#include "xir/xxir_effects.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_nominal.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "xir_library_compile_owner.h"

static const char *const error_contract_sources[]={
    "fn plain()->i64{return 7}\nfn worker()->i64{const f=plain;return f()}\n",
    "enum Failure{Bad}\nfn opaque(error:Error)->i64{throw error}\nfn worker()->i64{const f=opaque;return f(Failure.Bad)}\n",
    "enum Failure{Bad}\nfn plain()->i64{throw Failure.Bad}\nfn worker()->i64{const f=plain;return f()}\n",
    "fn plain()->i64{return 7}\nfn worker()->i64{const f:fn()->i64=plain;return f()}\n",
    "enum E{Bad}\nfn worker()->i64{\ntry{throw E.Bad}catch(e:E){return 7}\n}\n",
    "enum E{Bad}\nfn worker()->i64{try{\ntry{throw E.Bad}\ncatch(e:E){throw e}\n}catch(e:E){return 11}}\n"
};
typedef struct ErrorContractExpected {
    bool root_unknown,error_unknown,error_unidentified,known_variant;
    XrXirEffect throws;
    XrXirStatus task,go;
} ErrorContractExpected;
static const ErrorContractExpected error_contract_expected[]={
    {false,false,false,false,XR_XIR_EFFECT_NONE,XR_XIR_OK,XR_XIR_OK},
    {false,false,true,false,XR_XIR_EFFECT_MAY,XR_XIR_BAD_TYPE,XR_XIR_OK},
    {false,false,false,true,XR_XIR_EFFECT_MAY,XR_XIR_OK,XR_XIR_OK},
    {true,true,false,false,XR_XIR_EFFECT_UNKNOWN,XR_XIR_BAD_TYPE,XR_XIR_BAD_TYPE}
};
typedef struct ErrorContractCase { uint32_t which;bool inspect; } ErrorContractCase;

static XrXirStatus error_contract_program(const XrXirCompileContext *context,uint32_t which,
    bool go,XrXirSourceResult *output) {
    char bytes[512];int length=snprintf(bytes,sizeof(bytes),"%sfn main()->i64{return %sworker()}\n",
        error_contract_sources[which],go?"await go ":"");CHECK(length>0 && (size_t)length<sizeof(bytes));
    XrCompilerSession *session=NULL;XrCompilerSessionStatus created=xr_compile_session_new(context->resources,&session);
    if (created!=XR_COMPILER_SESSION_OK)
        return created==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"error-contract-source",NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={NULL,bytes,(size_t)length};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,output,&diagnostic,NULL);
    xr_compile_session_free(session);memset(bytes,0xff,sizeof(bytes));
    if (status!=XR_XIR_OK) CHECK(!output->checked && !output->snapshot);
    return status;
}

static XrXirStatus error_contract_facts(XrXirArtifact **artifact,const ErrorContractCase *fixture,uint32_t pass) {
    const XrXirModule *module=xr_xir_compile_artifact_module(*artifact);CHECK(module);
    uint32_t worker=UINT32_MAX;XrXirType error=XR_XIR_UNIT;
    for (uint32_t f=0;f<module->function_count;++f)
        if (module->functions[f].name_length==6 && !memcmp(module->functions[f].name,"worker",6)) worker=f;
    CHECK(worker!=UINT32_MAX);
    for (uint32_t t=0;module->types && module->types->nominals && t<module->types->count;++t) {
        const XrXirTypeNode *node=&module->types->nodes[t];
        if (node->kind!=XR_XIR_TYPE_NOMINAL || node->nominal.declaration>=module->types->nominals->count) continue;
        const XrXirNominalDeclaration *declaration=&module->types->nominals->declarations[node->nominal.declaration];
        if (declaration->name.length==7 && !memcmp(declaration->name.bytes,"Failure",7))
            error=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+t);
    }
    XrXirEffects *effects=NULL;XrXirStatus status=xr_xir_compile_effects_analyze(*artifact,&effects);
    if (status!=XR_XIR_OK) { CHECK(!effects);return status; }
    xr_xir_compile_artifact_free(*artifact);*artifact=NULL;
    const XrXirRootEffects *root=xr_xir_effects_root(effects,worker);
    const XrXirFunctionEffects *function=xr_xir_effects_function(effects,worker);CHECK(root && function);
    const ErrorContractExpected *expected=&error_contract_expected[fixture->which];
    bool unknown=xr_xir_effects_error_unknown(effects,worker),unidentified=xr_xir_effects_error_unidentified(effects,worker);
    bool known=error!=XR_XIR_UNIT && xr_xir_effects_error(effects,worker,error,0);
    XrXirStatus task=xr_xir_effects_task_errors(effects,worker),go=xr_xir_effects_go_safe(effects,worker);
    CHECK(!root->requires_root && root->unresolved==expected->root_unknown);
    CHECK(unknown==expected->error_unknown && unidentified==expected->error_unidentified);
    CHECK(known==expected->known_variant && function->throws==expected->throws && task==expected->task && go==expected->go);
    if (fixture->inspect) printf("error contract%u pass%u rootNONE unknown%u throws%u error_unknown%u unidentified%u known%u task%u go%u\n",
        fixture->which,pass,(unsigned)root->unresolved,(unsigned)function->throws,(unsigned)unknown,
        (unsigned)unidentified,(unsigned)known,(unsigned)task,(unsigned)go);
    xr_xir_compile_effects_free(effects);return XR_XIR_OK;
}

static XrXirStatus error_contract_operation(const XrXirCompileContext *context,void *opaque) {
    const ErrorContractCase *fixture=opaque;XrXirSourceResult source={0};
    XrXirCheckedPacket packet={0};XrXirArtifact *original=NULL,*decoded=NULL;
    XrXirStatus status=error_contract_program(context,fixture->which,false,&source);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_write(source.checked,&packet,NULL);
    original=source.checked;source.checked=NULL;xr_xir_compile_source_result_free(&source);
    if (status==XR_XIR_OK) status=error_contract_facts(&original,fixture,0);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&decoded,NULL);
    xr_xir_compile_checked_packet_free(&packet);
    if (status==XR_XIR_OK) status=error_contract_facts(&decoded,fixture,1);
    xr_xir_compile_artifact_free(original);xr_xir_compile_artifact_free(decoded);return status;
}

static void error_contract_go_verdict(const XrXirCompileContext *context,uint32_t which) {
    XrXirSourceResult source={0};XrXirStatus status=error_contract_program(context,which,true,&source);
    CHECK(status==((which==1 || which==3)?XR_XIR_BAD_TYPE:XR_XIR_OK));
    CHECK(status==XR_XIR_OK ? source.checked && source.snapshot : !source.checked && !source.snapshot);
    xr_xir_compile_source_result_free(&source);
}

static XrXirStatus error_contract_retry(const XrXirCompileContext *context,void *opaque) {
    size_t blocks=source_program_compile_live,bytes=source_program_compile_bytes;
    XrXirStatus status=error_contract_operation(context,opaque);
    CHECK(source_program_compile_live==blocks && source_program_compile_bytes==bytes);
    if (status==XR_XIR_OUT_OF_MEMORY && source_program_compile_injected && source_program_compile_fail_at!=SIZE_MAX) {
        size_t failed=source_program_compile_fail_at;XrCompileResourceStats paid=library_compile_stats(context);
        source_program_compile_fail_at=SIZE_MAX;
        CHECK(error_contract_operation(context,opaque)==XR_XIR_OK);
        XrCompileResourceStats retried=library_compile_stats(context);
        CHECK(retried.live_bytes==paid.live_bytes && retried.allocated_bytes>paid.allocated_bytes && retried.work>paid.work);
        CHECK(source_program_compile_live==blocks && source_program_compile_bytes==bytes);
        source_program_compile_fail_at=failed;
    }
    return status;
}

static XrXirStatus throw_facts_operation(const XrXirCompileContext *context,void *opaque) {
    (void)opaque;
    for (uint32_t which=4;which<6;++which) {
        XrXirSourceResult source={0};XrXirStatus status=error_contract_program(context,which,false,&source);
        if (status!=XR_XIR_OK) { xr_xir_compile_source_result_free(&source);return status; }
        const XrXirModule *module=xr_xir_compile_artifact_module(source.checked);uint32_t worker=UINT32_MAX;
        for (uint32_t f=0;f<module->function_count;++f)
            if (module->functions[f].name_length==6 && !memcmp(module->functions[f].name,"worker",6)) worker=f;
        CHECK(worker!=UINT32_MAX);
        const XrXirSourceDebugView *debug=xr_xir_compile_source_snapshot_debug(source.snapshot);CHECK(debug);
        const XrXirSourceDebugFunction *function=&debug->functions[worker];
        uint32_t expected=which==4 ? 1 : 2;CHECK(function->throw_count==expected);
        for (uint32_t t=0;t<function->throw_count;++t) {
            XrXirOp op=module->functions[worker].instructions[function->throw_sites[t].instruction].op;
            CHECK(op==XR_XIR_JUMP || op==XR_XIR_CLEANUP_ERROR);
        }
        xr_xir_compile_artifact_free(source.checked);source.checked=NULL;
        uint32_t lines=0;
        for (uint32_t t=0;t<function->throw_count;++t) {
            const XrXirSourceDebugThrow *site=&function->throw_sites[t];
            CHECK(site->instruction<function->instruction_count &&
                (uint64_t)site->operand<(uint64_t)function->parameter_count+function->instruction_count);
            CHECK(!t || function->throw_sites[t-1].instruction<site->instruction);
            const XrXirSourceDebugPoint *point=&function->points[site->instruction];
            CHECK(point->statement && point->range.line>=3 && point->range.line<=4);
            lines|=UINT32_C(1)<<(uint32_t)(point->range.line-3);
        }
        CHECK(lines==(expected==1 ? 1u : 3u));xr_xir_compile_source_result_free(&source);
    }
    return XR_XIR_OK;
}
static XrXirStatus throw_facts_retry(const XrXirCompileContext *context,void *opaque) {
    size_t blocks=source_program_compile_live,bytes=source_program_compile_bytes;
    XrXirStatus status=throw_facts_operation(context,opaque);
    CHECK(source_program_compile_live==blocks && source_program_compile_bytes==bytes);
    if (status==XR_XIR_OUT_OF_MEMORY && source_program_compile_injected && source_program_compile_fail_at!=SIZE_MAX) {
        size_t failed=source_program_compile_fail_at;XrCompileResourceStats paid=library_compile_stats(context);
        source_program_compile_fail_at=SIZE_MAX;CHECK(throw_facts_operation(context,opaque)==XR_XIR_OK);
        XrCompileResourceStats retried=library_compile_stats(context);
        CHECK(retried.live_bytes==paid.live_bytes && retried.allocated_bytes>paid.allocated_bytes && retried.work>paid.work);
        CHECK(source_program_compile_live==blocks && source_program_compile_bytes==bytes);
        source_program_compile_fail_at=failed;
    }
    return status;
}
int main(int argc,char **argv) {
    if (argc==2 && !strcmp(argv[1],"--source-throws")) {
        library_compile_operation_cases("Source local throws and rethrow observations, producer death and same-owner retry",throw_facts_retry,NULL);
    } else if (argc==3 && !strcmp(argv[1],"--compiler")) {
        char *end=NULL;unsigned long index=strtoul(argv[2],&end,10);CHECK(end && end!=argv[2] && !*end && index<4);
        ErrorContractCase fixture={(uint32_t)index,false};
        library_compile_operation_cases("Source independent ROOT/error facts, Checked replay and same-owner retry",error_contract_retry,&fixture);
    } else if (argc==1 || (argc==2 && !strcmp(argv[1],"--census"))) {
        for (uint32_t which=0;which<4;++which) {
            LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
            ErrorContractCase fixture={which,argc==1};source_program_compile_attempts=0;
            CHECK(error_contract_operation(&owner.context,&fixture)==XR_XIR_OK);
            printf("error contract%u census=%zu actual allocation sites\n",which,source_program_compile_attempts);
            if (argc==1) error_contract_go_verdict(&owner.context,which);
            library_compile_owner_drop(&owner);
        }
        if (argc==1) {
            LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
            source_program_compile_attempts=0;CHECK(throw_facts_operation(&owner.context,NULL)==XR_XIR_OK);
            printf("Source local/rethrow observations after producer death census=%zu actual allocation sites\n",source_program_compile_attempts);
            library_compile_owner_drop(&owner);
        }
    } else return 2;
    library_compile_observer_free();return 0;
}
