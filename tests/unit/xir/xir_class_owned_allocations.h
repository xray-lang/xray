/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_owned_allocations.h - Every class metadata allocation has one owner
 *
 * KEY CONCEPT:
 *   Faults preserve inputs, publish no output, and restore physical ownership baselines.
 */
#ifndef XIR_CLASS_OWNED_ALLOCATIONS_H
#define XIR_CLASS_OWNED_ALLOCATIONS_H
#include "xir_class_owned_fixture.h"
typedef struct ClassOwnedStages {
    ClassOwnedFixture fixture;
    XrXirArtifact *checked,*specialized,*lowered;
    XrXirCheckedPacket packet;
    XrXirDomain *domain;
    XrXirVmBinding bindings[5];
    XrXirCallEntry entries[5];
    XrXirProgramSpec spec;
} ClassOwnedStages;
typedef struct ClassOwnedOutput {
    XrXirNominalTable *nominals;
    XrXirTypes *types;
    XrXirArtifact *artifact;
    XrXirCheckedPacket packet;
    XrXirTypeArena *arena;
    XrXirProgram *program;
    XrXirBudget budget;
    XrXirStorageLayout storage;
    uint32_t offsets[2];
} ClassOwnedOutput;
static XrXirStatus class_owned_phase(ClassOwnedStages *s,uint32_t phase,ClassOwnedOutput *o) {
    switch(phase) {
    case 0:return xr_xir_nominal_clone(&s->fixture.nominals,&s->fixture.types,&o->budget,&o->nominals);
    case 1:return xr_xir_nominal_project(&s->fixture.nominals,&o->budget,&o->nominals);
    case 2:return xr_xir_types_clone(&s->fixture.types,&o->types);
    case 3:return xr_xir_check(&s->fixture.module,&o->budget,&o->artifact,NULL);
    case 4:return xr_xir_checked_write(s->checked,&o->budget,&o->packet,NULL);
    case 5:return xr_xir_checked_read(s->packet.bytes,s->packet.length,&o->budget,&o->artifact,NULL);
    case 6:return xr_xir_specialize(s->checked,&o->budget,&o->artifact,NULL);
    case 7:return xr_xir_lower(s->specialized,&fixture_target,&o->budget,&o->artifact,NULL);
    case 8:
        o->storage.field_offsets=o->offsets;o->storage.field_count=2;
        return xr_xir_storage_layouts(xr_xir_artifact_module(s->lowered)->types,&fixture_target,&o->budget,&o->storage,1);
    case 9:{
        XrXirValueStatus status=xr_xir_type_arena_new(s->domain,xr_xir_artifact_module(s->lowered)->types,&o->budget,&o->arena);
        return status==XR_XIR_VALUE_OK ? XR_XIR_OK : status==XR_XIR_VALUE_OOM ? XR_XIR_OUT_OF_MEMORY :
            status==XR_XIR_VALUE_LIMIT ? XR_XIR_BUDGET : XR_XIR_BAD_TYPE;
    }
    case 10:return xr_xir_program_seal(&s->spec,(XrXirProgramBudget){33554432,64000000},&o->program);
    default:return xr_xir_verify_remaining(&s->fixture.module,&o->budget,NULL);
    }
}
static void class_owned_output_drop(ClassOwnedOutput *o) {
    xr_xir_program_drop(o->program);xr_xir_type_arena_drop(o->arena);
    xr_xir_checked_packet_free(&o->packet);xr_xir_artifact_free(o->artifact);
    xr_xir_types_free(o->types);xr_xir_nominal_free(o->nominals);
}
static void class_owned_stages_init(ClassOwnedStages *s) {
    memset(s,0,sizeof(*s));class_owned_fixture(&s->fixture);
    CHECK(xr_xir_check(&s->fixture.module,NULL,&s->checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_checked_write(s->checked,NULL,&s->packet,NULL)==XR_XIR_OK);
    CHECK(xr_xir_specialize(s->checked,NULL,&s->specialized,NULL)==XR_XIR_OK);
    CHECK(xr_xir_lower(s->specialized,&fixture_target,NULL,&s->lowered,NULL)==XR_XIR_OK);
    CHECK(xr_xir_domain_new(1048576,&s->domain)==XR_XIR_VALUE_OK);
    for(uint32_t f=0;f<5;++f) CHECK(xr_xir_vm_bind(s->lowered,f,&s->bindings[f],&s->entries[f])==XR_XIR_OK);
    const XrXirModule *m=xr_xir_artifact_module(s->lowered);
    s->spec=(XrXirProgramSpec){XR_XIR_PROGRAM_ABI_VERSION,fixture_target,s->entries,5,m->declarations,
        {NULL,NULL},m->types,xr_xir_program_proof(s->lowered)};
}
static void class_owned_stages_drop(ClassOwnedStages *s) {
    xr_xir_domain_drop(s->domain);xr_xir_artifact_free(s->lowered);xr_xir_artifact_free(s->specialized);
    xr_xir_checked_packet_free(&s->packet);xr_xir_artifact_free(s->checked);
}
static void class_owned_poisoning(ClassOwnedStages *s) {
    const XrXirModule *m=xr_xir_artifact_module(s->lowered);
    XrXirNominalIdentity *identity=(XrXirNominalIdentity *)m->types->nominals->identities;
    const XrXirNominalType original=m->types->nodes[0].nominal;
    identity[0].flags=0;
    CHECK(m->types->nodes[0].nominal.field_count==original.field_count && m->types->nodes[0].nominal.fields==original.fields);
    CHECK(xr_xir_artifact_verify(s->lowered,NULL,NULL)==XR_XIR_BAD_STRUCTURE);
    XrXirProgram *program=(XrXirProgram *)(uintptr_t)1;
    CHECK(xr_xir_program_seal(&s->spec,(XrXirProgramBudget){33554432,64000000},&program)==XR_XIR_BAD_STRUCTURE && !program);
    identity[0].flags=XR_XIR_NOMINAL_FINAL;
    CHECK(xr_xir_artifact_verify(s->lowered,NULL,NULL)==XR_XIR_OK);
    XrXirNominalDeclaration *origin=(XrXirNominalDeclaration *)s->specialized->module.provenance->source->module.types->nominals->declarations;
    origin[0].flags=0;
    XrXirArtifact *output=(XrXirArtifact *)(uintptr_t)1;
    CHECK(xr_xir_lower(s->specialized,&fixture_target,NULL,&output,NULL)==XR_XIR_BAD_STRUCTURE && !output);
    origin[0].flags=XR_XIR_NOMINAL_FINAL;
    CHECK(xr_xir_artifact_verify(s->specialized,NULL,NULL)==XR_XIR_OK);
}
static void class_owned_budget_cases(ClassOwnedStages *s) {
    const uint32_t phases[]={0,1,3,4,5,6,7,8,9,11};
    for(uint32_t p=0;p<sizeof(phases)/sizeof(phases[0]);++p) {
        for(uint32_t limit=0;limit<3;++limit) {
            uint32_t phase=phases[p];if((phase==8 && limit==0) || ((phase==0 || phase==1) && limit==2)) continue;
            ClassOwnedOutput o={0};o.budget=xr_xir_default_budget();
            if(limit==0)o.budget.metadata_bytes=0;
            if(limit==1)o.budget.work=0;
            if(limit==2)o.budget.scratch_bytes=0;
            XrXirBudget before=o.budget;size_t baseline=live,bytes=class_live_bytes;
            XrXirStatus status=class_owned_phase(s,phase,&o);
            if(status!=XR_XIR_BUDGET)fprintf(stderr,"class phase %u limit %u status %u\n",phase,limit,status);
            CHECK(status==XR_XIR_BUDGET);
            CHECK(!o.nominals && !o.artifact && !o.packet.bytes && !o.arena);
            CHECK(o.budget.scratch_bytes==before.scratch_bytes && o.budget.frame_bytes==before.frame_bytes);
            if(phase!=11)CHECK(!memcmp(&o.budget,&before,sizeof(before)));
            class_owned_output_drop(&o);CHECK(live==baseline && class_live_bytes==bytes);
        }
    }
    /* Clone/project own only durable metadata; storage layouts own only scratch. */
    const uint32_t scratchless[]={0,1,8};
    for(uint32_t i=0;i<3;++i) {
        ClassOwnedOutput o={0};o.budget=xr_xir_default_budget();
        if(i<2)o.budget.scratch_bytes=0;else o.budget.metadata_bytes=0;
        XrXirBudget before=o.budget;size_t baseline=live,bytes=class_live_bytes;
        CHECK(class_owned_phase(s,scratchless[i],&o)==XR_XIR_OK);
        CHECK(o.budget.scratch_bytes==before.scratch_bytes&&o.budget.frame_bytes==before.frame_bytes);
        class_owned_output_drop(&o);CHECK(live==baseline&&class_live_bytes==bytes);
    }
    for(uint32_t i=0;i<2;++i){
        XrXirProgram *output=(XrXirProgram *)(uintptr_t)1;
        XrXirProgramBudget budget={i?33554432:0,i?0:64000000};
        size_t baseline=live,bytes=class_live_bytes;
        CHECK(xr_xir_program_seal(&s->spec,budget,&output)==XR_XIR_BUDGET && !output);
        CHECK(live==baseline && class_live_bytes==bytes);
    }
}
static void class_owned_allocations(void) {
    CHECK(!live && !class_live_bytes);fail_at=SIZE_MAX;
    ClassOwnedStages s;class_owned_stages_init(&s);
    for(uint32_t phase=0;phase<12;++phase) {
        size_t sites=0,baseline=live,bytes=class_live_bytes;
        uint64_t domain_bytes=xr_xir_domain_stats(s.domain).live_bytes;
        for(size_t attempt=0;attempt<=sites;++attempt) {
            ClassOwnedOutput o={0};o.budget=xr_xir_default_budget();XrXirBudget before=o.budget;
            calls=0;fail_at=attempt?attempt-1:SIZE_MAX;
            XrXirStatus status=class_owned_phase(&s,phase,&o);
            if(!attempt){CHECK(status==XR_XIR_OK);sites=calls;CHECK(sites);
                if(o.nominals)CHECK((o.nominals->declarations?o.nominals->declarations[0].flags:o.nominals->identities[0].flags)==XR_XIR_NOMINAL_FINAL);
                if(o.arena){const XrXirStorageLayout *layout=xr_xir_type_arena_storage(o.arena,(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE);CHECK(layout&&layout->value.size==8&&layout->body.size==16&&layout->field_offsets[1]==8);}
                if(phase==8)CHECK(o.storage.value.size==8&&o.storage.body.size==16&&o.offsets[1]==8);
            } else {
                if(status!=XR_XIR_OUT_OF_MEMORY)fprintf(stderr,"class phase %u site %zu status %u\n",phase,attempt-1,status);
                CHECK(status==XR_XIR_OUT_OF_MEMORY);
                CHECK(!o.nominals&&!o.types&&!o.artifact&&!o.packet.bytes&&!o.packet.length&&!o.arena&&!o.program);
                if(phase!=11)CHECK(!memcmp(&o.budget,&before,sizeof(before)));
            }
            CHECK(o.budget.scratch_bytes==before.scratch_bytes&&o.budget.frame_bytes==before.frame_bytes);
            class_owned_output_drop(&o);CHECK(live==baseline&&class_live_bytes==bytes);
            CHECK(xr_xir_domain_stats(s.domain).live_bytes==domain_bytes);
        }
        fail_at=SIZE_MAX;printf("Class owning phase %u: %zu allocation faults; baseline bytes restored\n",phase,sites);
    }
    class_owned_budget_cases(&s);class_owned_poisoning(&s);
    class_owned_stages_drop(&s);CHECK(!live&&!class_live_bytes);calls=0;fail_at=SIZE_MAX;
}
#endif // XIR_CLASS_OWNED_ALLOCATIONS_H
