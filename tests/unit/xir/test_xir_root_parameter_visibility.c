/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_root_parameter_visibility.c - Source permission for private instances
 *
 * KEY CONCEPT:
 *   Exported declarations authorize real source edges, never public instances.
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_root_parameter_fixture.h"

typedef struct VisibilityFixture {
    RootParameterBindingFixture higher;
    XrXirFunction functions[5];
    XrXirFunctionIdentity identities[5];
    XrXirSourceModule modules[2];
    XrXirDeclarations declarations;
    XrXirInstruction entry[7], body[2], done;
    XrXirBlock entry_blocks[3], body_block, init_block;
    XrXirType parameter, concrete;
    XrXirConstraint constraint;
    XrXirGeneric generics[4];
    XrXirCallableParameter callable_parameter;
    XrXirTypeNode callable;
    XrXirTypes types;
    XrXirEffectParameter scalar_parameter;
    XrXirRootValueIdentity reference_value;
    XrXirFunctionEffectContract contracts[5];
    XrXirProvenance evidence;
    uint32_t operand, dependency;
    XrXirModule module;
} VisibilityFixture;

/* Modes are ordinary generic CALL, generic FUNCTION_REF, generic INVOKE,
 * and a finite higher-order actual parameter binding. */
static void visibility_fixture(VisibilityFixture *f, const XrXirCompileContext *c, unsigned mode) {
    memset(f,0,sizeof(*f));CHECK(mode<4);f->dependency=1;
    f->modules[0]=(XrXirSourceModule){"root",4,&f->dependency,1,2};
    f->modules[1]=(XrXirSourceModule){"alpha",5,NULL,0,3};
    f->identities[1]=(XrXirFunctionIdentity){.module=1,.exported=1};
    f->identities[3].module=1;
    f->declarations=(XrXirDeclarations){.modules=f->modules,.module_count=2,
        .functions=f->identities,.entry_function=0};
    f->done=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->init_block=(XrXirBlock){.count=1};
    f->functions[2]=(XrXirFunction){.name="root_init",.name_length=9,.result=XR_XIR_UNIT,
        .instructions=&f->done,.instruction_count=1,.blocks=&f->init_block,.block_count=1};
    f->functions[3]=f->functions[2];f->functions[3].name="alpha_init";f->functions[3].name_length=10;
    f->parameter=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE;f->concrete=XR_XIR_I64;
    f->constraint=(XrXirConstraint){.markers=XR_XIR_CONSTRAINT_SENDABLE};
    f->generics[0]=(XrXirGeneric){.arguments=&f->concrete,.argument_count=1};
    f->generics[1]=(XrXirGeneric){.constraints=&f->constraint,.parameter_count=1};
    f->body[0]=(XrXirInstruction){.op=XR_XIR_COPY,.type=f->parameter,.args={0,0}};
    f->body[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1,0}};
    f->body_block=(XrXirBlock){.count=2};
    f->functions[1]=(XrXirFunction){.name="identity",.name_length=8,.parameters=&f->parameter,
        .parameter_count=1,.result=f->parameter,.instructions=f->body,.instruction_count=2,
        .blocks=&f->body_block,.block_count=1};
    f->entry[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=37};
    f->entry[1]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.args={0,1},
        .immediate=1,.type_arguments={0,1}};
    f->entry[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1,0}};
    f->entry_blocks[0]=(XrXirBlock){.count=3};
    f->functions[0]=(XrXirFunction){.name="run",.name_length=3,.result=XR_XIR_I64,
        .instructions=f->entry,.instruction_count=3,.blocks=f->entry_blocks,.block_count=1,
        .operands=&f->operand,.operand_count=1};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=4,
        .declarations=&f->declarations,.generics=f->generics,.linkage_kind=XR_XIR_PROGRAM};
    f->contracts[1]=(XrXirFunctionEffectContract){.parameter_count=1,.parameters=&f->scalar_parameter};
    f->contracts[2].formula.constant_mask=XR_XIR_CALLABLE_ROOT_REQUIRED;
    f->contracts[3].formula.constant_mask=XR_XIR_CALLABLE_ROOT_REQUIRED;
    f->evidence=(XrXirProvenance){.kind=XR_XIR_EVIDENCE_TEMPLATE,.contracts=f->contracts,.contract_count=4};
    f->module.provenance=&f->evidence;
    if (mode==1) {
        f->callable_parameter=(XrXirCallableParameter){.type=XR_XIR_I64};
        f->callable=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.parameters=&f->callable_parameter,
            .parameter_count=1,.result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_NONE};
        f->types=(XrXirTypes){.nodes=&f->callable,.count=1};f->module.types=&f->types;
        f->entry[1].op=XR_XIR_FUNCTION_REF;f->entry[1].type=(XrXirType)256;f->entry[1].args[1]=0;
        f->entry[2]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.args={0,1},.immediate=1};
        f->entry[3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2,0}};
        f->functions[0].instruction_count=4;f->entry_blocks[0].count=4;
        f->reference_value=(XrXirRootValueIdentity){1,XR_XIR_EFFECT_VALUE_AUTHENTIC_REF,(XrXirType)256};
        f->contracts[0].values=&f->reference_value;f->contracts[0].value_count=1;
    } else if (mode==2) {
        f->entry[1].op=XR_XIR_INVOKE;f->entry[1].targets[0]=1;f->entry[1].targets[1]=2;
        f->entry[2]=(XrXirInstruction){.op=XR_XIR_INVOKE_RESULT,.type=XR_XIR_I64,.immediate=1};
        f->entry[3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2,0}};
        f->entry[4]=(XrXirInstruction){.op=XR_XIR_INVOKE_ERROR,.type=XR_XIR_ERROR,.immediate=1};
        f->entry[5]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=91};
        f->entry[6]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={5,0}};
        f->entry_blocks[0].count=2;f->entry_blocks[1]=(XrXirBlock){.first=2,.count=2};
        f->entry_blocks[2]=(XrXirBlock){.first=4,.count=3};
        f->functions[0].instruction_count=7;f->functions[0].block_count=3;
    } else if (mode==3) {
        rp_binding_fixture(&f->higher,c,false);
        memcpy(f->functions,f->higher.functions,sizeof(f->higher.functions));
        memcpy(f->contracts,f->higher.contracts,sizeof(f->higher.contracts));
        f->functions[4]=f->functions[3];f->functions[4].name="alpha_init";f->functions[4].name_length=10;
        f->contracts[4].formula.constant_mask=XR_XIR_CALLABLE_ROOT_REQUIRED;
        f->modules[0].initializer=3;f->modules[1].initializer=4;
        f->identities[2]=(XrXirFunctionIdentity){0};f->identities[3]=(XrXirFunctionIdentity){0};
        f->identities[4].module=1;
        f->evidence=f->higher.evidence;f->evidence.contracts=f->contracts;f->evidence.contract_count=5;
        f->module=f->higher.module;f->module.functions=f->functions;f->module.function_count=5;
        f->module.declarations=&f->declarations;f->module.provenance=&f->evidence;
    }
}

static uint32_t visibility_shape(const XrXirArtifact *artifact, unsigned mode) {
    const XrXirModule *m=xr_xir_compile_artifact_module(artifact);
    CHECK(m && m->stage==XR_XIR_CHECKED && m->declarations && m->provenance);
    const XrXirProvenance *p=m->provenance;
    CHECK(p->kind==XR_XIR_EVIDENCE_INSTANCE && p->count==m->function_count && p->source);
    const XrXirModule *source=xr_xir_compile_artifact_module(p->source);
    CHECK(source && source->stage==XR_XIR_CHECKED && source->declarations->module_count==2 &&
        source->provenance && source->provenance->kind==XR_XIR_EVIDENCE_TEMPLATE);
    const XrXirInstruction *edge=&m->functions[0].instructions[mode==3?2:1];
    CHECK(edge->op==(mode==1?XR_XIR_FUNCTION_REF:mode==2?XR_XIR_INVOKE:XR_XIR_CALL));
    CHECK(edge->immediate>=0 && (uint64_t)edge->immediate<m->function_count);
    uint32_t callee=(uint32_t)edge->immediate;
    CHECK(p->origins[callee].function==1 && source->declarations->functions[1].exported==1);
    CHECK(m->declarations->functions[callee].module==1 && !m->declarations->functions[callee].exported);
    if (mode==3) {
        CHECK(p->origins[callee].effect_argument_count==1);
        const XrXirTypeNode *signature=xr_xir_callable_signature(m->types,m->functions[callee].parameters[0]);
        CHECK(signature && signature->flags==XR_XIR_CALLABLE_ROOT_NONE);
    } else {
        CHECK(p->origins[callee].argument_count==1 && p->origins[callee].arguments[0]==XR_XIR_I64);
        CHECK(m->functions[callee].result==XR_XIR_I64 && m->functions[callee].parameters[0]==XR_XIR_I64);
    }
    return callee;
}

static XrXirStatus visibility_pipeline(const XrXirCompileContext *c, unsigned mode,
    XrXirArtifact **output, bool qualify) {
    VisibilityFixture f;visibility_fixture(&f,c,mode);
    XrXirArtifact *checked=NULL,*read=NULL,*instance=NULL,*reloaded=NULL,*lowered=NULL;
    XrXirCheckedPacket first={0},second={0};XrXirDiagnostic d={0};
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    const char *phase="check";
    XrXirStatus status=xir_fixture_check(c, &f.module, &checked, &d);
    if (status==XR_XIR_OK) { phase="write-template";status=xr_xir_compile_checked_write(checked,&first,&d); }
    xr_xir_compile_artifact_free(checked);checked=NULL;
    memset(&f,0,sizeof(f));
    if (status==XR_XIR_OK) { phase="read-template";status=xr_xir_compile_checked_read(c,first.bytes,first.length,&read,&d); }
    xr_xir_compile_checked_packet_free(&first);
    if (status==XR_XIR_OK) { phase="specialize";status=xr_xir_compile_specialize(read,&instance,&d); }
    xr_xir_compile_artifact_free(read);read=NULL;
    if (status==XR_XIR_OK) { phase="write-instance";status=xr_xir_compile_checked_write(instance,&second,&d); }
    xr_xir_compile_artifact_free(instance);instance=NULL;
    if (status==XR_XIR_OK) { phase="read-instance";status=xr_xir_compile_checked_read(c,second.bytes,second.length,&reloaded,&d); }
    xr_xir_compile_checked_packet_free(&second);
    if (status==XR_XIR_OK && qualify) (void)visibility_shape(reloaded,mode);
    if (status==XR_XIR_OK) { phase="verify-instance";status=xr_xir_compile_artifact_verify(reloaded,&d); }
    if (status==XR_XIR_OK) { phase="lower";status=xr_xir_compile_lower(reloaded,&target,&lowered,&d); }
    if (status==XR_XIR_OK) { phase="verify-lowered";status=xr_xir_compile_artifact_verify(lowered,&d); }
    if (status==XR_XIR_OK) { *output=reloaded;reloaded=NULL; }
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(reloaded);
    if (qualify && status!=XR_XIR_OK) fprintf(stderr,"visibility mode%u phase%s status%u f%u b%u i%u\n",
        mode,phase,status,d.function,d.block,d.instruction);
    return status;
}

static void visibility_literals(void) {
    for (unsigned mode=0;mode<4;++mode) {
        RootParameterMark physical=rp_mark();XrXirCompileContext c=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&c).live_bytes;XrXirArtifact *artifact=NULL;
        XrXirStatus status=visibility_pipeline(&c,mode,&artifact,true);
        if (status!=XR_XIR_OK) fprintf(stderr,"visibility mode%u status%u\n",mode,status);
        CHECK(status==XR_XIR_OK && artifact);
        const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
        XrXirArtifact *lowered=NULL;
        CHECK(xr_xir_compile_lower(artifact,&target,&lowered,NULL)==XR_XIR_OK && lowered);
        xr_xir_compile_artifact_free(artifact);
        CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);
        xr_xir_compile_artifact_free(lowered);rp_owner_free(&c,baseline);rp_balanced(physical);
    }
}

static void visibility_rejected(XrXirArtifact *artifact, XrXirStatus expected) {
    XrXirDiagnostic d={0};XrXirCheckedPacket packet={0};
    XrXirStatus status=xr_xir_compile_artifact_verify(artifact,&d);
    if (status!=expected) fprintf(stderr,"visibility reject actual%u expected%u f%u b%u i%u\n",
        status,expected,d.function,d.block,d.instruction);
    CHECK(status==expected);
    CHECK(xr_xir_compile_checked_write(artifact,&packet,&d)==expected && !packet.bytes && !packet.length);
}

static void visibility_forged(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext c=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&c).live_bytes;XrXirArtifact *artifact=NULL;
    CHECK(visibility_pipeline(&c,0,&artifact,true)==XR_XIR_OK && artifact);
    uint32_t target=visibility_shape(artifact,0);
    XrXirModule *m=&artifact->module;XrXirProvenance *p=(XrXirProvenance *)m->provenance;
    XrXirFunctionIdentity *original=(XrXirFunctionIdentity *)p->source->module.declarations->functions;
    XrXirFunctionIdentity *actual=(XrXirFunctionIdentity *)m->declarations->functions;
    XrXirSourceModule *modules=(XrXirSourceModule *)p->source->module.declarations->modules;
    XrXirOrigin *origins=(XrXirOrigin *)p->origins;
    XrXirInstruction *edge=(XrXirInstruction *)m->functions[0].instructions;
    XrXirInstruction *source_edge=(XrXirInstruction *)p->source->module.functions[0].instructions;
    original[1].exported=0;visibility_rejected(artifact,XR_XIR_BAD_STRUCTURE);original[1].exported=1;
    const uint32_t *dependencies=modules[0].dependencies;
    modules[0].dependencies=NULL;modules[0].dependency_count=0;
    visibility_rejected(artifact,XR_XIR_BAD_STRUCTURE);
    modules[0].dependencies=dependencies;modules[0].dependency_count=1;
    origins[target].function=2;visibility_rejected(artifact,XR_XIR_BAD_STRUCTURE);origins[target].function=1;
    source_edge[1].immediate=2;visibility_rejected(artifact,XR_XIR_BAD_STRUCTURE);source_edge[1].immediate=1;
    source_edge[1].op=XR_XIR_FUNCTION_REF;visibility_rejected(artifact,XR_XIR_BAD_STRUCTURE);source_edge[1].op=XR_XIR_CALL;
    edge[1].args[1]=0;visibility_rejected(artifact,XR_XIR_BAD_STRUCTURE);edge[1].args[1]=1;
    XrXirType *arguments=(XrXirType *)origins[target].arguments;
    arguments[0]=XR_XIR_BOOL;visibility_rejected(artifact,XR_XIR_BAD_TYPE);arguments[0]=XR_XIR_I64;
    actual[target].exported=1;visibility_rejected(artifact,XR_XIR_BAD_STRUCTURE);actual[target].exported=0;
    CHECK(xr_xir_compile_artifact_verify(artifact,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(artifact);rp_owner_free(&c,baseline);rp_balanced(physical);
}

static void visibility_resources(void) {
    /* Each operation count is measured on this exact graph, including both
     * packets, producer destruction, specialization and Lowered validation. */
    for (unsigned mode=0;mode<4;++mode) {
        RootParameterMark physical=rp_mark();XrXirCompileContext normal=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&normal).live_bytes;XrXirArtifact *artifact=NULL;rp_attempts=0;
        CHECK(visibility_pipeline(&normal,mode,&artifact,false)==XR_XIR_OK && artifact);
        size_t sites=rp_attempts;CHECK(sites);XrCompileResourceStats required=rp_stats(&normal);
        xr_xir_compile_artifact_free(artifact);rp_owner_free(&normal,baseline);rp_balanced(physical);
        for (size_t fault=0;fault<sites;++fault) {
            XrXirCompileContext c=rp_owner(rp_caps());baseline=rp_stats(&c).live_bytes;
            RootParameterMark mark=rp_mark();rp_attempts=0;rp_fail_at=fault;rp_injected=false;artifact=NULL;
            CHECK(visibility_pipeline(&c,mode,&artifact,false)==XR_XIR_OUT_OF_MEMORY && rp_injected && !artifact);
            rp_balanced(mark);XrCompileResourceStats failed=rp_stats(&c);rp_fail_at=SIZE_MAX;
            CHECK(visibility_pipeline(&c,mode,&artifact,false)==XR_XIR_OK && artifact);
            CHECK(rp_stats(&c).work>=failed.work && rp_stats(&c).allocated_bytes>=failed.allocated_bytes);
            xr_xir_compile_artifact_free(artifact);rp_balanced(mark);
            rp_owner_free(&c,baseline);rp_balanced(physical);
        }
        for (unsigned axis=0;axis<3;++axis) for (unsigned shortfall=0;shortfall<2;++shortfall) {
            XrCompileResourceLimits caps=rp_caps();
            if (axis==0) caps.allocated_bytes=required.allocated_bytes-shortfall;
            if (axis==1) caps.live_bytes=required.peak_bytes-shortfall;
            if (axis==2) caps.work=required.work-shortfall;
            XrXirCompileContext c=rp_owner(caps);baseline=rp_stats(&c).live_bytes;artifact=NULL;
            CHECK(visibility_pipeline(&c,mode,&artifact,false)==(shortfall?XR_XIR_BUDGET:XR_XIR_OK));
            CHECK(shortfall?!artifact:artifact!=NULL);xr_xir_compile_artifact_free(artifact);
            rp_owner_free(&c,baseline);rp_balanced(physical);
        }
        printf("visibility mode%u full pipeline OOM sites=%zu allocated=%llu peak=%llu work=%llu\n",mode,sites,
            (unsigned long long)required.allocated_bytes,(unsigned long long)required.peak_bytes,
            (unsigned long long)required.work);
    }
}

#include "xir_root_parameter_visibility_runtime.h"

int main(void) {
    visibility_literals();visibility_forged();visibility_resources();visibility_runtime();
    CHECK(!rp_live && !rp_live_bytes);return 0;
}
