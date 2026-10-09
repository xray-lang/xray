/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_root_parameter_lower_names.c - Effect identities after type compaction
 *
 * KEY CONCEPT:
 *   Variant names follow compacted argument IDs; default advertisements stay fixed.
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_types.h"
#include "xir/xxir_nominal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_root_parameter_fixture.h"

typedef struct LowerNameFixture {
    RootParameterBindingFixture base;
    XrXirTypeNode nodes[4];
    XrXirNominalField field;
    XrXirNominalDeclaration declaration;
    XrXirNominalTable table;
    XrXirConstraint constraint;
    XrXirType concrete;
    XrXirGeneric generics[4];
} LowerNameFixture;

static void lower_name_fixture(LowerNameFixture *f, const XrXirCompileContext *c, bool ordinary) {
    _Static_assert(XR_XIR_I64==2, "The fixed name oracle uses the I64 enum identity");
    memset(f,0,sizeof(*f));rp_binding_fixture(&f->base,c,false);
    f->constraint=(XrXirConstraint){.markers=XR_XIR_CONSTRAINT_SENDABLE};f->concrete=XR_XIR_I64;
    f->field=(XrXirNominalField){.name={"value",5},.type=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE};
    f->declaration=(XrXirNominalDeclaration){.module={"probe",5},.name={"Box",3},.exported=1,
        .constraints=&f->constraint,.parameter_count=1,.fields=&f->field,.field_count=1,
        .kind=XR_XIR_NOMINAL_STRUCT};
    f->table=(XrXirNominalTable){.declarations=&f->declaration,.count=1};
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,.parameter_span=1};
    f->nodes[1]=f->base.nodes[0];f->nodes[2]=f->base.nodes[1];
    f->nodes[3]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,
        .nominal={.declaration=0,.arguments=&f->concrete,.argument_count=1}};
    f->base.types=(XrXirTypes){.nodes=f->nodes,.count=4,.nominals=&f->table};
    f->base.apply.parameter=(XrXirType)257;
    f->base.caller_ops[0].type=(XrXirType)258;f->base.caller_ops[1].type=(XrXirType)258;
    f->base.values[0].declared_type=(XrXirType)258;f->base.values[1].declared_type=(XrXirType)257;
    f->base.identities[1].exported=1;
    if (ordinary) {
        f->generics[0]=(XrXirGeneric){.arguments=&f->concrete,.argument_count=1};
        f->generics[1]=(XrXirGeneric){.constraints=&f->constraint,.parameter_count=1};
        f->base.caller_ops[2].type_arguments[1]=1;f->base.module.generics=f->generics;
    }
}

static void lower_name_equal(const XrXirFunction *function, const char *expected) {
    size_t length=strlen(expected);
    if (function->name_length!=length || memcmp(function->name,expected,length)) {
        fprintf(stderr,"lower name actual[%u]=",function->name_length);
        fwrite(function->name,1,function->name_length,stderr);
        fprintf(stderr," expected[%zu]=%s I64=%u\n",length,expected,(unsigned)XR_XIR_I64);
    }
    CHECK(function->name_length==length && !memcmp(function->name,expected,length));
}

static void lower_name_shape(const XrXirArtifact *artifact, bool ordinary, bool lowered) {
    const XrXirModule *m=xr_xir_compile_artifact_module(artifact);
    CHECK(m && m->stage==(lowered?XR_XIR_LOWERED:XR_XIR_CHECKED));
    CHECK(m->types && m->types->count==(lowered?3u:4u) && m->types->nominals);
    const XrXirNominalTable *table=m->types->nominals;
    CHECK(table->count==1 && (lowered?table->identities!=NULL:table->declarations!=NULL));
    CHECK(lowered?!table->declarations:!table->identities);
    const XrXirTypeNode *nominal=&m->types->nodes[lowered?2:3];
    CHECK(nominal->kind==XR_XIR_TYPE_NOMINAL && nominal->nominal.field_count==1 &&
        nominal->nominal.fields[0]==XR_XIR_I64);
    if (!lowered) CHECK(m->types->nodes[0].parameter_span==1);
    else for (uint32_t t=0;t<m->types->count;++t) CHECK(!m->types->nodes[t].parameter_span);
    const XrXirProvenance *p=m->provenance;
    CHECK(p && p->kind==XR_XIR_EVIDENCE_INSTANCE && p->source && p->count==m->function_count);
    CHECK(p->source->module.provenance && p->source->module.provenance->kind==XR_XIR_EVIDENCE_TEMPLATE);
    const XrXirInstruction *call=&m->functions[0].instructions[2];
    CHECK(call->op==XR_XIR_CALL && call->immediate>=0 && (uint64_t)call->immediate<m->function_count);
    uint32_t selected=(uint32_t)call->immediate;
    const XrXirOrigin *origin=&p->origins[selected];
    CHECK(origin->function==1 && origin->argument_count==(ordinary?1u:0u));
    CHECK(origin->effect_argument_count==1 && origin->effect_arguments[0].parameter==0 &&
        origin->effect_arguments[0].type==(XrXirType)(lowered?257:258));
    CHECK(m->functions[selected].parameters[0]==(XrXirType)(lowered?257:258));
    CHECK(!m->declarations->functions[selected].exported);
    if (ordinary) {
        CHECK(origin->arguments[0]==XR_XIR_I64);
        lower_name_equal(&m->functions[selected],lowered?"apply$1:2@0:257":"apply$1:2@0:258");
    } else {
        lower_name_equal(&m->functions[selected],lowered?"apply$1@0:257":"apply$1@0:258");
        uint32_t defaults=0;
        for (uint32_t i=0;i<p->count;++i) {
            const XrXirOrigin *candidate=&p->origins[i];
            if (candidate->function!=1 || i==selected) continue;
            CHECK(!candidate->argument_count && candidate->effect_argument_count==1 &&
                candidate->effect_arguments[0].parameter==0 &&
                candidate->effect_arguments[0].type==(XrXirType)(lowered?256:257));
            lower_name_equal(&m->functions[i],"apply");
            CHECK(m->declarations->functions[i].exported==1);++defaults;
        }
        CHECK(defaults==1);
    }
}

static XrXirStatus lower_name_pipeline(const XrXirCompileContext *c, bool ordinary,
    XrXirArtifact **output, bool qualify) {
    LowerNameFixture f;lower_name_fixture(&f,c,ordinary);
    XrXirArtifact *checked=NULL,*instance=NULL,*read=NULL,*lowered=NULL;
    XrXirCheckedPacket packet={0};XrXirDiagnostic d={0};
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    XrXirStatus status=xir_fixture_check(c, &f.base.module, &checked, &d);
    memset(&f,0x95,sizeof(f));
    if (status==XR_XIR_OK) status=xr_xir_compile_specialize(checked,&instance,&d);
    xr_xir_compile_artifact_free(checked);
    if (status==XR_XIR_OK && qualify) lower_name_shape(instance,ordinary,false);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_write(instance,&packet,&d);
    xr_xir_compile_artifact_free(instance);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_read(c,packet.bytes,packet.length,&read,&d);
    xr_xir_compile_checked_packet_free(&packet);
    if (status==XR_XIR_OK) status=xr_xir_compile_lower(read,&target,&lowered,&d);
    xr_xir_compile_artifact_free(read);
    if (status==XR_XIR_OK && qualify) lower_name_shape(lowered,ordinary,true);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(lowered,&d);
    if (status==XR_XIR_OK) { *output=lowered;lowered=NULL; }
    xr_xir_compile_artifact_free(lowered);
    if (qualify && status!=XR_XIR_OK) fprintf(stderr,"lower names ordinary%u status%u f%u b%u i%u\n",
        (unsigned)ordinary,status,d.function,d.block,d.instruction);
    return status;
}

static void lower_name_literals(void) {
    for (unsigned ordinary=0;ordinary<2;++ordinary) {
        RootParameterMark physical=rp_mark();XrXirCompileContext c=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&c).live_bytes;XrXirArtifact *artifact=NULL;
        CHECK(lower_name_pipeline(&c,ordinary!=0,&artifact,true)==XR_XIR_OK && artifact);
        xr_xir_compile_artifact_free(artifact);rp_owner_free(&c,baseline);rp_balanced(physical);
    }
}

static void lower_name_resources(void) {
    for (unsigned ordinary=0;ordinary<2;++ordinary) {
        RootParameterMark physical=rp_mark();XrXirCompileContext normal=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&normal).live_bytes;XrXirArtifact *artifact=NULL;rp_attempts=0;
        CHECK(lower_name_pipeline(&normal,ordinary!=0,&artifact,false)==XR_XIR_OK && artifact);
        size_t sites=rp_attempts;CHECK(sites);XrCompileResourceStats required=rp_stats(&normal);
        xr_xir_compile_artifact_free(artifact);rp_owner_free(&normal,baseline);rp_balanced(physical);
        for (size_t fault=0;fault<sites;++fault) {
            XrXirCompileContext c=rp_owner(rp_caps());baseline=rp_stats(&c).live_bytes;
            RootParameterMark mark=rp_mark();rp_attempts=0;rp_fail_at=fault;rp_injected=false;artifact=NULL;
            CHECK(lower_name_pipeline(&c,ordinary!=0,&artifact,false)==XR_XIR_OUT_OF_MEMORY && rp_injected && !artifact);
            rp_balanced(mark);XrCompileResourceStats failed=rp_stats(&c);rp_fail_at=SIZE_MAX;
            CHECK(lower_name_pipeline(&c,ordinary!=0,&artifact,false)==XR_XIR_OK && artifact);
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
            CHECK(lower_name_pipeline(&c,ordinary!=0,&artifact,false)==(shortfall?XR_XIR_BUDGET:XR_XIR_OK));
            CHECK(shortfall?!artifact:artifact!=NULL);xr_xir_compile_artifact_free(artifact);
            rp_owner_free(&c,baseline);rp_balanced(physical);
        }
        printf("lower names ordinary%u full pipeline OOM sites=%zu allocated=%llu peak=%llu work=%llu\n",ordinary,sites,
            (unsigned long long)required.allocated_bytes,(unsigned long long)required.peak_bytes,
            (unsigned long long)required.work);
    }
}

int main(void) {
    lower_name_literals();lower_name_resources();CHECK(!rp_live && !rp_live_bytes);return 0;
}
