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
    XrXirCheckedPacket packet;XrXirDomain *domain;
    XrXirVmBinding bindings[5];XrXirCallEntry entries[5];XrXirProgramSpec spec;
} ClassOwnedStages;
typedef struct ClassOwnedOutput {
    XrXirNominalTable *nominals;XrXirTypes *types;XrXirArtifact *artifact;
    XrXirCheckedPacket packet;XrXirTypeArena *arena;XrXirProgram *program;
    XrXirStorageLayout storage;uint32_t offsets[2];
} ClassOwnedOutput;
static void class_owned_output_drop(ClassOwnedOutput *o) {
    xr_xir_compile_program_drop(o->program);xr_xir_compile_type_arena_drop(o->arena);
    xr_xir_compile_checked_packet_free(&o->packet);xr_xir_compile_artifact_free(o->artifact);
    xr_xir_compile_types_free(o->types);xr_xir_compile_nominal_free(o->nominals);
}
static void class_owned_stages_drop(ClassOwnedStages *s) {
    xr_xir_domain_drop(s->domain);xr_xir_compile_artifact_free(s->lowered);
    xr_xir_compile_artifact_free(s->specialized);xr_xir_compile_checked_packet_free(&s->packet);
    xr_xir_compile_artifact_free(s->checked);
}
static XrXirStatus class_owned_stages_init(const XrXirCompileContext *context,ClassOwnedStages *s) {
    memset(s,0,sizeof(*s));class_owned_fixture(&s->fixture);
    XrXirStatus status=xr_xir_compile_check(context,&s->fixture.module,&s->checked,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_checked_write(s->checked,&s->packet,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_specialize(s->checked,&s->specialized,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_lower(s->specialized,&fixture_target,&s->lowered,NULL);
    if(status==XR_XIR_OK) {
        XrXirValueStatus ds=xr_xir_domain_new(1048576,&s->domain);
        status=ds==XR_XIR_VALUE_OK?XR_XIR_OK:ds==XR_XIR_VALUE_OOM?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_TYPE;
    }
    for(uint32_t f=0;status==XR_XIR_OK && f<5;++f)
        status=xr_xir_compile_vm_bind(s->lowered,f,&s->bindings[f],&s->entries[f]);
    if(status==XR_XIR_OK) {
        const XrXirModule *m=xr_xir_compile_artifact_module(s->lowered);
        s->spec=(XrXirProgramSpec){XR_XIR_PROGRAM_ABI_VERSION,fixture_target,s->entries,5,m->declarations,
            {NULL,NULL},m->types,xr_xir_compile_program_proof(s->lowered)};
    }
    return status;
}
static XrXirStatus class_owned_phase(const XrXirCompileContext *context,ClassOwnedStages *s,uint32_t phase,ClassOwnedOutput *o) {
    switch(phase) {
    case 0:return xr_xir_compile_nominal_clone(context,&s->fixture.nominals,&s->fixture.types,&o->nominals);
    case 1:return xr_xir_compile_nominal_project(context,&s->fixture.nominals,&o->nominals);
    case 2:return xr_xir_compile_types_clone(context,&s->fixture.types,&o->types);
    case 3:return xr_xir_compile_check(context,&s->fixture.module,&o->artifact,NULL);
    case 4:return xr_xir_compile_checked_write(s->checked,&o->packet,NULL);
    case 5:return xr_xir_compile_checked_read(context,s->packet.bytes,s->packet.length,&o->artifact,NULL);
    case 6:return xr_xir_compile_specialize(s->checked,&o->artifact,NULL);
    case 7:return xr_xir_compile_lower(s->specialized,&fixture_target,&o->artifact,NULL);
    case 8:
        o->storage.field_offsets=o->offsets;o->storage.field_count=2;
        return xr_xir_compile_storage_layouts(context,xr_xir_compile_artifact_module(s->lowered)->types,&fixture_target,&o->storage,1);
    case 9:{
        XrXirValueStatus status=xr_xir_compile_type_arena_new(context,xr_xir_compile_artifact_module(s->lowered)->types,&o->arena);
        return status==XR_XIR_VALUE_OK?XR_XIR_OK:status==XR_XIR_VALUE_OOM?XR_XIR_OUT_OF_MEMORY:
            status==XR_XIR_VALUE_LIMIT?XR_XIR_BUDGET:XR_XIR_BAD_TYPE;
    }
    case 10:return xr_xir_compile_program_seal(context,&s->spec,&o->program);
    default:return xr_xir_compile_verify(context,&s->fixture.module,NULL);
    }
}
static XrXirStatus class_owned_operation(const XrXirCompileContext *context,void *opaque) {
    const uint32_t phase=*(uint32_t *)opaque;
    ClassOwnedStages stages;ClassOwnedOutput output={0};
    XrXirStatus status=class_owned_stages_init(context,&stages);
    if(status==XR_XIR_OK)status=class_owned_phase(context,&stages,phase,&output);
    if(status==XR_XIR_OK) {
        if(output.nominals)CHECK((output.nominals->declarations?output.nominals->declarations[0].flags:
            output.nominals->identities[0].flags)==XR_XIR_NOMINAL_FINAL);
        if(output.arena) {
            const XrXirStorageLayout *layout=xr_xir_compile_type_arena_storage(output.arena,(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE);
            CHECK(layout && layout->value.size==8 && layout->body.size==16 && layout->field_offsets[1]==8);
        }
        if(phase==8)CHECK(output.storage.value.size==8 && output.storage.body.size==16 && output.offsets[1]==8);
    } else CHECK(!output.nominals && !output.types && !output.artifact && !output.packet.bytes &&
        !output.packet.length && !output.arena && !output.program);
    class_owned_output_drop(&output);class_owned_stages_drop(&stages);return status;
}
static void class_owned_poisoning(void) {
    AllocationCompileOwner owner={0};allocation_compile_owner_new(&owner,&allocation_compile_limits);
    ClassOwnedStages s;CHECK(class_owned_stages_init(&owner.context,&s)==XR_XIR_OK);
    const XrXirModule *m=xr_xir_compile_artifact_module(s.lowered);
    XrXirNominalIdentity *identity=(XrXirNominalIdentity *)m->types->nominals->identities;
    const XrXirNominalType original=m->types->nodes[0].nominal;identity[0].flags=0;
    CHECK(m->types->nodes[0].nominal.field_count==original.field_count && m->types->nodes[0].nominal.fields==original.fields);
    XrXirStatus poison_status=xr_xir_compile_artifact_verify(s.lowered,NULL);
    if(poison_status!=XR_XIR_BAD_TYPE)fprintf(stderr,"class projected identity poison status=%u\n",poison_status);
    CHECK(poison_status==XR_XIR_BAD_TYPE);
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&owner.context,&s.spec,&program)==XR_XIR_BAD_STRUCTURE && !program);
    program=(XrXirProgram *)(uintptr_t)1;
    CHECK(xr_xir_compile_program_seal(&owner.context,&s.spec,&program)==XR_XIR_BAD_STRUCTURE && program==(XrXirProgram *)(uintptr_t)1);
    identity[0].flags=XR_XIR_NOMINAL_FINAL;
    CHECK(xr_xir_compile_artifact_verify(s.lowered,NULL)==XR_XIR_OK);
    XrXirNominalDeclaration *origin=(XrXirNominalDeclaration *)s.specialized->module.provenance->source->module.types->nominals->declarations;
    origin[0].flags=0;XrXirArtifact *output=NULL;
    CHECK(xr_xir_compile_lower(s.specialized,&fixture_target,&output,NULL)==XR_XIR_BAD_TYPE && !output);
    output=(XrXirArtifact *)(uintptr_t)1;
    CHECK(xr_xir_compile_lower(s.specialized,&fixture_target,&output,NULL)==XR_XIR_BAD_TYPE && output==(XrXirArtifact *)(uintptr_t)1);
    origin[0].flags=XR_XIR_NOMINAL_FINAL;
    CHECK(xr_xir_compile_artifact_verify(s.specialized,NULL)==XR_XIR_OK);
    class_owned_stages_drop(&s);allocation_compile_owner_drop(&owner);
}
static void class_owned_allocations(void) {
    CHECK(!live && !class_live_bytes);fail_at=SIZE_MAX;
    for(uint32_t phase=0;phase<12;++phase) {
        char name[64];CHECK(snprintf(name,sizeof(name),"class owning phase %u full prefix",phase)>0);
        allocation_compile_operation_cases(name,class_owned_operation,&phase);
    }
    class_owned_poisoning();CHECK(!live && !class_live_bytes);calls=0;
}
#endif // XIR_CLASS_OWNED_ALLOCATIONS_H
