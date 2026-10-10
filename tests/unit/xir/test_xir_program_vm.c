/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_program_vm.c - Sealed Lowered programs in the VM
 *
 * KEY CONCEPT:
 *   Instance code owns its artifact and matches independent expected effects.
 */
#include "xir/xxir_vm.h"
#include "xir/xxir_program_internal.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_consumer_context_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_module_forest_fixture.h"
#include "xir_module_forest_cases.h"
#include "xir_program_fixture.h"
#include "xir_program_cases.h"
#include "xir_capture_fixture.h"
#include "xir_capture_cases.h"
#include "xir_array_program_fixture.h"
#include "xir_array_program_cases.h"
#include "xir_nominal_generic_fixture.h"
#include "xir_nominal_generic_cases.h"
#include "xir_nominal_expression_fixture.h"
#include "xir_nominal_expression_cases.h"
#include "xir_nominal_chain_fixture.h"
#include "xir_nominal_checked_fixture.h"
#include "xir_nominal_transport_fixture.h"
#include "xir_nominal_transport_cases.h"
#include "xir_struct_ops_fixture.h"
#include "xir_struct_ops_cases.h"
#include "xir_enum_ops_fixture.h"
#include "xir_enum_ops_cases.h"
#include "xir_struct_set_fixture.h"
#include "xir_struct_set_cases.h"
#include "xir_cleanup_program_fixture.h"
#include "xir_cleanup_program_cases.h"
#include "xir_path_program_fixture.h"
#include "xir_path_program_cases.h"
extern const XrXirProgramSpec path0_program, path1_program, path2_program;
static void path_program_mixed(void) {
    const XrXirProgramSpec *specs[] = {&path0_program,&path1_program,&path2_program};
    for (unsigned kind = 0; kind < 3; ++kind) for (unsigned mask = 1; mask < 15; ++mask) {
        XrXirArtifact *artifact = path_program_fixture(suite_context, kind);
        XrXirProgramSpec spec = *specs[kind];
        XrXirCallEntry entries[7]; XrXirVmBinding bindings[7];
        memcpy(entries,spec.entries,sizeof(entries));
        for (unsigned f = 0; f < 4; ++f) if (mask & (1u << f))
            CHECK(xr_xir_compile_vm_bind(artifact, f, &bindings[f], &entries[f]) == XR_XIR_OK);
        spec.entries = entries;
        XrXirProgram *program = NULL;
        CHECK(xr_xir_compile_program_seal(suite_context, &spec, &program) == XR_XIR_OK);
        path_program_cases(program,kind);
        xr_xir_compile_artifact_free(artifact);artifact=NULL;
        CHECK(!runtime_live && !runtime_bytes);
    }
}
extern const XrXirProgramSpec cleanup0_program, cleanup1_program, cleanup2_program, cleanup3_program, cleanup4_program, cleanup5_program;
static void cleanup_program_mixed(void) {
    const XrXirProgramSpec *specs[] = {&cleanup0_program, &cleanup1_program, &cleanup2_program,
        &cleanup3_program, &cleanup4_program, &cleanup5_program};
    for (unsigned mode = 0; mode < 6; ++mode) for (unsigned mask = 1; mask < 7; ++mask) {
        XrXirArtifact *artifact = cleanup_program_fixture(suite_context, mode);
        XrXirProgramSpec spec = *specs[mode];
        XrXirCallEntry entries[3]; XrXirVmBinding bindings[3];
        memcpy(entries, spec.entries, sizeof(entries));
        for (unsigned f = 0; f < 3; ++f) if (mask & (1u << f))
            CHECK(xr_xir_compile_vm_bind(artifact, f, &bindings[f], &entries[f]) == XR_XIR_OK);
        spec.entries = entries;
        XrXirProgram *program = NULL;
        CHECK(xr_xir_compile_program_seal(suite_context, &spec, &program) == XR_XIR_OK);
        cleanup_program_cases(program, mode);
        xr_xir_compile_artifact_free(artifact);artifact=NULL;
        CHECK(!runtime_live && !runtime_bytes);
    }
}
static void descriptor_correspondence(void) {
    XrXirCompileContext pair=consumer_context_default();
    XrXirArtifact *a = nominal_expression_lowered(&pair), *b = nominal_expression_lowered(&pair);
    const XrXirModule *module = xr_xir_compile_artifact_module(b);
    CHECK(module->function_count == 9);
    XrXirCallEntry entries[9]; XrXirVmBinding bindings[9];
    for (uint32_t i = 0; i < 9; ++i)
        CHECK(xr_xir_compile_vm_bind(b, i, &bindings[i], &entries[i]) == XR_XIR_OK);
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, *xr_xir_compile_artifact_target(b),
        entries, 9, module->declarations, {0}, module->types, xr_xir_compile_program_proof(b)};
    XrCompileResourceStats before=consumer_context_stats(&pair);
    CHECK(xr_xir_compile_program_match(&pair,&spec,b->layouts,a)==XR_XIR_OK);
    XrCompileResourceStats complete=consumer_context_stats(&pair);
    CHECK(complete.work>before.work);
    for(unsigned minus=0;minus<2;++minus){
        /* A new bounded ledger is fixed before either graph is Checked. */
        XrXirCompileContext tight=consumer_context_limits((XrCompileResourceLimits){67108864,8388608,complete.work-minus});
        XrXirArtifact *left=nominal_expression_lowered(&tight),*right=nominal_expression_lowered(&tight);
        XrXirCallEntry replay_entries[9];XrXirVmBinding replay_bindings[9];
        for(uint32_t i=0;i<9;++i)CHECK(xr_xir_compile_vm_bind(right,i,&replay_bindings[i],&replay_entries[i])==XR_XIR_OK);
        CHECK(xr_xir_compile_program_match(&tight,&spec,right->layouts,left)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        CHECK(consumer_context_stats(&tight).work<=complete.work-minus);
        xr_xir_compile_artifact_free(left);xr_xir_compile_artifact_free(right);
    }
    for (uint32_t attack = 0; attack < 7; ++attack) {
        XrXirTypeNode *node = (XrXirTypeNode *)&module->types->nodes[0];
        XrXirType saved = node->nominal.fields[0];
        XrXirFunctionIdentity *id = (XrXirFunctionIdentity *)&module->declarations->functions[0];
        uint32_t exported = id->exported;
        char *name = (char *)module->types->nominals->identities[0].name.bytes;
        switch (attack) {
        case 0: entries[0].result = XR_XIR_U8; break;
        case 1: ((XrXirType *)node->nominal.fields)[0] = XR_XIR_BOOL; break;
        case 2: id->exported ^= 1; break;
        case 3: name[0] ^= 1; break;
        case 4: --spec.entry_count; break;
        case 5: ++spec.target.abi_version; break;
        case 6: id->member_access = XR_XIR_MEMBER_PRIVATE; break;
        }
        CHECK(xr_xir_compile_program_match(&pair, &spec, b->layouts, a) == XR_XIR_BAD_STRUCTURE);
        entries[0].result = module->functions[0].result;
        ((XrXirType *)node->nominal.fields)[0] = saved; id->exported = exported;
        if (attack == 3) name[0] ^= 1;
        id->member_access = XR_XIR_MEMBER_PUBLIC;
        spec.entry_count = 9; spec.target = *xr_xir_compile_artifact_target(b);
    }
    CHECK(xr_xir_compile_program_match(&pair, &spec, NULL, a) == XR_XIR_BAD_STRUCTURE);
    uint32_t owned = UINT32_MAX, parameter = UINT32_MAX;
    for (uint32_t i = 0; i < 9; ++i) {
        if (b->layouts[i].owned_count) owned = i;
        if (module->functions[i].parameter_count) parameter = i;
    }
    CHECK(owned != UINT32_MAX && parameter != UINT32_MAX);
    for (uint32_t attack = 0; attack < 14; ++attack) {
        uint32_t index = attack >= 10 ? parameter : owned;
        XrXirFunctionLayout *layout = &b->layouts[index], saved = *layout;
        uint32_t offset = layout->offsets[0], owned_offset = layout->owned_offsets[0];
        XrXirLayout physical = {0};
        if (index == parameter) physical = layout->parameters[0];
        switch (attack) {
        case 0: ++layout->slot_count; break;
        case 1: ++layout->frame_bytes; break;
        case 2: ++layout->owned_count; break;
        case 3: ++layout->outgoing_count; break;
        case 4: ++layout->result.size; break;
        case 5: ++layout->result.alignment; break;
        case 6: ((uint32_t *)layout->offsets)[0] ^= 8; break;
        case 7: ((uint32_t *)layout->owned_offsets)[0] ^= 8; break;
        case 8: layout->offsets = NULL; break;
        case 9: layout->owned_offsets = NULL; break;
        case 10: ++((XrXirLayout *)layout->parameters)[0].size; break;
        case 11: ++((XrXirLayout *)layout->parameters)[0].alignment; break;
        case 12: layout->parameters = NULL; break;
        case 13: ++layout->path_count; break;
        }
        CHECK(xr_xir_compile_program_match(&pair, &spec, b->layouts, a) == XR_XIR_BAD_STRUCTURE);
        *layout = saved;
        ((uint32_t *)layout->offsets)[0] = offset;
        ((uint32_t *)layout->owned_offsets)[0] = owned_offset;
        if (index == parameter) ((XrXirLayout *)layout->parameters)[0] = physical;
    }
    CHECK(xr_xir_compile_program_match(&pair, &spec, b->layouts, a) == XR_XIR_OK);
    xr_xir_compile_artifact_free(a);a=NULL; xr_xir_compile_artifact_free(b);b=NULL;
}
static void admission(void) {
    for (uint32_t invalid = 0; invalid < 23; ++invalid) {
        XrXirArtifact *artifact = program_fixture(suite_context, 0), *saved = artifact;
        const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
        XrXirDeclarations *d = (XrXirDeclarations *) module->declarations;
        XrXirSourceModule *modules = (XrXirSourceModule *) d->modules;
        XrXirFunctionIdentity *ids = (XrXirFunctionIdentity *) d->functions;
        XrXirSlot *slots = (XrXirSlot *) d->slots;
        XrXirLiteral *literals = (XrXirLiteral *) d->literals;
        XrXirInstruction *root = (XrXirInstruction *) module->functions[3].instructions;
        XrXirInstruction *alpha = (XrXirInstruction *) module->functions[2].instructions;
        switch (invalid) {
        case 0: ((uint32_t *) modules[0].dependencies)[1] = 1; break;
        case 1: ((uint32_t *) modules[0].dependencies)[1] = 0; break;
        case 2: ((uint32_t *) modules[0].dependencies)[1] = 3; break;
        case 3: modules[0].dependency_count = 1; break;
        case 4: memcpy((char *) modules[1].name, "root", 4); break;
        case 5: ((unsigned char *) modules[1].name)[0] = 0xff; break;
        case 6: ids[4].exported = 0; break;
        case 7: ids[2].exported = 1; break;
        case 8: d->entry_function = 2; break;
        case 9: slots[0].mutable = 1; break;
        case 10: slots[4].type = slots[0].type; break;
        case 11: ((unsigned char *) literals[0].bytes)[0] = 0xff; break;
        case 12: alpha[3].immediate = 5; break;
        case 13: alpha[2].immediate = 1; break;
        case 14: alpha[2].args[0] = 0; break;
        case 15: root[10].op = XR_XIR_SLOT_INIT; root[10].immediate = 4; break;
        case 16: root[0].immediate = 2; break;
        case 17: root[6].type = XR_XIR_I64; break;
        case 18: alpha[1].args[0] = 3; break;
        case 19: alpha[5].args[0] = 1; break;
        case 20: ((XrXirInstruction *) module->functions[4].instructions)[2].args[1] = 0; break;
        case 21: root[6].immediate = 0; break;
        case 22: ids[4].exported = 2; break;
        }
        XrXirProgram *program = NULL;
        CHECK(xr_xir_compile_artifact_verify(artifact, NULL) != XR_XIR_OK);
        CHECK(xr_xir_compile_vm_program_take(&artifact, &program) != XR_XIR_OK);
        CHECK(artifact == saved && !program);
        xr_xir_compile_artifact_free(artifact);artifact=NULL;
    }
}
extern const XrXirProgramSpec captures_program, captures_error_program;
typedef struct CaptureMixedOwner {
    XrXirArtifact *artifact;
    XrXirVmBinding bindings[6];
    XrXirCallEntry entries[6];
} CaptureMixedOwner;
static unsigned mixed_releases;
static void capture_mixed_release(void *pointer) {
    CaptureMixedOwner *owner = pointer;
    xr_xir_compile_artifact_free(owner->artifact); xr_free(owner); ++mixed_releases;
}
static void capture_mixed(void) {
    for (unsigned throwing = 0; throwing < 2; ++throwing) for (unsigned parity = 0; parity < 2; ++parity) {
        const XrXirProgramSpec *native = throwing ? &captures_error_program : &captures_program;
        CaptureMixedOwner *owner = xr_calloc(1,sizeof(*owner)); CHECK(owner);
        owner->artifact = capture_fixture(suite_context, throwing != 0);
        const XrXirModule *module = xr_xir_compile_artifact_module(owner->artifact);
        for (uint32_t i = 0; i < 6; ++i) {
            CHECK(xr_xir_compile_vm_bind(owner->artifact, i, &owner->bindings[i], &owner->entries[i]) == XR_XIR_OK);
            if ((i == 3 ? 0u : 1u) == parity) owner->entries[i] = native->entries[i];
        }
        CHECK((owner->entries[3].resume == native->entries[3].resume) !=
            (owner->entries[5].resume == native->entries[5].resume));
        XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION,
            {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},owner->entries,6,module->declarations,
            {owner,capture_mixed_release},module->types,xr_xir_compile_program_proof(owner->artifact)};
        XrXirProgram *program = NULL;
        CHECK(xr_xir_compile_program_seal(suite_context, &spec, &program) == XR_XIR_OK);
        capture_cases(program,throwing != 0); CHECK(mixed_releases == throwing*2+parity+1);
    }
}
typedef struct ArrayMixedOwner {
    XrXirArtifact *artifact;
    XrXirVmBinding bindings[8];
    XrXirCallEntry entries[8];
} ArrayMixedOwner;
static uint32_t array_mixed_releases;
static void array_mixed_release(void *pointer) {
    ArrayMixedOwner *owner = pointer;
    xr_xir_compile_artifact_free(owner->artifact); xr_free(owner); ++array_mixed_releases;
}
extern const XrXirProgramSpec array_program0_program, array_program1_program;
static void array_mixed(void) {
    const XrXirProgramSpec *native[] = {&array_program0_program,&array_program1_program};
    for (uint32_t mode = 0; mode < 2; ++mode) {
        for (uint32_t parity = 0; parity < 2; ++parity) {
            ArrayMixedOwner *owner = xr_calloc(1,sizeof(*owner)); CHECK(owner);
            owner->artifact = array_program_fixture(suite_context, mode != 0, false);
            const XrXirModule *module = xr_xir_compile_artifact_module(owner->artifact);
            for (uint32_t i = 0; i < 8; ++i) {
                CHECK(xr_xir_compile_vm_bind(owner->artifact, i, &owner->bindings[i], &owner->entries[i]) == XR_XIR_OK);
                if (i % 2 == parity) owner->entries[i] = native[mode]->entries[i];
            }
            CHECK((owner->entries[2].resume == native[mode]->entries[2].resume) !=
                (owner->entries[3].resume == native[mode]->entries[3].resume));
            XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION,
                {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},owner->entries,8,module->declarations,
                {owner,array_mixed_release},module->types,xr_xir_compile_program_proof(owner->artifact)};
            XrXirProgram *program = NULL;
            CHECK(xr_xir_compile_program_seal(suite_context, &spec, &program) == XR_XIR_OK);
            array_program_cases(program,mode != 0);
            CHECK(array_mixed_releases == mode * 2 + parity + 1);
        }
    }
}
int main(void) {
    consumer_context=consumer_context_default();
    CHECK(!module_forest_fixture(suite_context, true));
    XrXirArtifact *forest = module_forest_fixture(suite_context, false); XrXirProgram *forest_program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&forest, &forest_program) == XR_XIR_OK);
    module_forest_cases(forest_program);
    path_program_mixed();
    for (unsigned kind = 0; kind < 3; ++kind) {
        XrXirArtifact *artifact = path_program_fixture(suite_context, kind); XrXirProgram *program = NULL;
        CHECK(xr_xir_compile_vm_program_take(&artifact, &program) == XR_XIR_OK);
        path_program_cases(program,kind);
        CHECK(!runtime_live && !runtime_bytes);
    }
    cleanup_program_mixed();
    for (unsigned mode = 0; mode < 6; ++mode) {
        XrXirArtifact *artifact = cleanup_program_fixture(suite_context, mode);
        XrXirProgram *program = NULL;
        CHECK(xr_xir_compile_vm_program_take(&artifact, &program) == XR_XIR_OK);
        cleanup_program_cases(program, mode);
        CHECK(!runtime_live && !runtime_bytes);
    }

    XrXirArtifact *struct_set = struct_set_lowered(suite_context); XrXirProgram *set_program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&struct_set, &set_program) == XR_XIR_OK && !struct_set);
    struct_set_cases(set_program);
    for (unsigned i = 1; i <= 13; ++i) enum_ops_checked(suite_context, i, false);
    size_t enum_baseline = runtime_live, enum_bytes = runtime_bytes;
    XrXirArtifact *enum_ops = enum_ops_lowered(suite_context, false); XrXirProgram *enum_program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&enum_ops, &enum_program) == XR_XIR_OK && !enum_ops);
    enum_ops_cases(enum_program);
    CHECK(runtime_live == enum_baseline && runtime_bytes == enum_bytes);
    enum_ops = enum_ops_lowered(suite_context, true); enum_program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&enum_ops, &enum_program) == XR_XIR_OK && !enum_ops);
    enum_wrong_variant_cases(enum_program);
    CHECK(runtime_live == enum_baseline && runtime_bytes == enum_bytes);
    XrXirArtifact *struct_ops = struct_ops_lowered(suite_context); XrXirProgram *struct_program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&struct_ops, &struct_program) == XR_XIR_OK && !struct_ops);
    struct_ops_cases(struct_program);
    for (unsigned mode = 0; mode < 3; ++mode) for (unsigned branch = 0; branch < 2; ++branch) {
        XrXirArtifact *transport = nominal_transport_fixture(suite_context); XrXirProgram *program = NULL;
        CHECK(xr_xir_compile_vm_program_take(&transport,&program) == XR_XIR_OK && !transport);
        XrXirInstance *instance = nominal_transport_instance(program);
        const XrXirTypes *types = program->types;
        xr_xir_compile_program_drop(program); program = NULL;
        XrXirValue escaped = nominal_transport_cases(instance,types,mode,branch != 0);
        nominal_transport_escaped(&escaped);
        CHECK(!runtime_live && !runtime_bytes);
    }
    XrXirArtifact *combined_artifact = nominal_generic_lowered(suite_context);
    XrXirProgram *combined = NULL;
    CHECK(xr_xir_compile_vm_program_take(&combined_artifact, &combined) == XR_XIR_OK && !combined_artifact);
    nominal_generic_cases(combined);
    XrXirArtifact *expression_artifact = nominal_expression_lowered(suite_context);
    XrXirProgram *expression_program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&expression_artifact, &expression_program) == XR_XIR_OK && !expression_artifact);
    nominal_expression_cases(expression_program);
    for (unsigned i = 0; i < 3; ++i) {
        XrXirArtifact *nominal = i == 2 ? nominal_chain_lowered(suite_context) : nominal_lowered_fixture(suite_context, i ? 3 : 0);
        XrXirProgram *program = NULL;
        CHECK(xr_xir_compile_vm_program_take(&nominal, &program) == XR_XIR_OK && !nominal);
        program_cases(program, 0);
    }

    array_mixed();
    for (uint32_t mode = 0; mode < 3; ++mode) {
        XrXirArtifact *array = array_program_fixture(suite_context, mode == 1, mode == 2);
        XrXirProgram *program = NULL;
        CHECK(xr_xir_compile_vm_program_take(&array, &program) == XR_XIR_OK);
        array_program_cases(program,mode == 1);
    }
    descriptor_correspondence(); admission(); capture_mixed();
    for(uint32_t mode=0;mode<3;++mode){
        XrXirCompileContext measure=consumer_context_default();
        XrXirArtifact *measured=program_fixture(&measure,mode);
        XrXirProgram *measured_program=NULL;
        CHECK(xr_xir_compile_vm_program_take(&measured,&measured_program)==XR_XIR_OK && !measured);
        XrCompileResourceStats complete=consumer_context_stats(&measure);
        xr_xir_compile_program_drop(measured_program);
        for(unsigned axis=0;axis<2;++axis){
            XrCompileResourceLimits caps={67108864,8388608,128000000};
            if(!axis)caps.allocated_bytes=complete.allocated_bytes-1;else caps.work=complete.work-1;
            XrXirCompileContext tight=consumer_context_limits(caps);
            XrXirArtifact *artifact=program_fixture(&tight,mode),*saved=artifact;XrXirProgram *program=NULL;
            CHECK(xr_xir_compile_vm_program_take(&artifact,&program)==XR_XIR_BUDGET && artifact==saved && !program);
            CHECK(xr_xir_compile_vm_program_take(&artifact,&program)==XR_XIR_BUDGET && artifact==saved && !program);
            xr_xir_compile_artifact_free(artifact);
        }
        /* An exhausted immutable ledger is never reset to make the retry pass. */
        XrXirCompileContext fresh=consumer_context_default();
        XrXirArtifact *artifact=program_fixture(&fresh,mode);XrXirProgram *program=NULL;
        CHECK(xr_xir_compile_vm_program_take(&artifact,&program)==XR_XIR_OK && !artifact && program);
        program_cases(program,mode);
    }
    for (unsigned throwing = 0; throwing < 2; ++throwing) {
    XrXirArtifact *artifact = capture_fixture(suite_context, throwing != 0);
    XrXirProgram *captures = NULL;
    CHECK(xr_xir_compile_vm_program_take(&artifact, &captures) == XR_XIR_OK);
    capture_cases(captures,throwing != 0);
    }
    puts("VM capture environments, two suspensions, cancellation and escaped ownership passed");
    puts("VM module programs match independent output, state and lifetime expectations");
    CHECK(!runtime_live && !runtime_bytes);
    consumer_contexts_free();
    return 0;
}
