/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_program_correspondence.c - Current input and final callback correspondence
 *
 * KEY CONCEPT:
 *   Valid programs with equal descriptors must not exchange canonical bodies.
 */
#include "base/xmalloc.h"
#include "xir/xxir_internal.h"
static void final_callback(unsigned attack);
static void final_restore(void);
static bool probe_armed, probe_injected, probe_allocations;
static size_t probe_calls, probe_at=SIZE_MAX;
static unsigned probe_attack;
static bool probe_seedless_cycle;
static XrXirArtifact *probe_artifact;
static void correspondence_callback(bool allocation) {
    if (!probe_armed || allocation!=probe_allocations) return;
    size_t ordinal=probe_calls++;
    if (ordinal!=probe_at) return;
    XrXirModule *module=(XrXirModule *)xr_xir_compile_artifact_module(probe_artifact);
    if (probe_attack==0 || probe_attack==3)
        ((XrXirInstruction *)module->functions[1].instructions)[0].immediate=43;
    if (probe_attack==1 || probe_attack==3)
        ++probe_artifact->layouts[1].frame_bytes;
    if (probe_attack==2 || probe_attack==3)
        ((XrXirFunctionIdentity *)module->declarations->functions)[1].test_timeout_seconds=7;
    if (probe_attack==4)
        ((XrXirFunctionIdentity *)module->declarations->functions)[1].exported=1;
    if (probe_attack==5)
        ((XrXirDeclarations *)module->declarations)->modules=NULL;
    if (probe_attack==6)
        ((XrXirDeclarations *)module->declarations)->functions=NULL;
    if (probe_attack==7)
        ((XrXirFunction *)module->functions)[1].name=(const char *)(uintptr_t)1;
    if (probe_attack==8)
        ((XrXirFunction *)module->functions)[1].blocks=(const XrXirBlock *)(uintptr_t)1;
    if (probe_attack==9)
        ((XrXirFunction *)module->functions)[1].instructions=(const XrXirInstruction *)(uintptr_t)1;
    if (probe_attack>=10) final_callback(probe_attack);
    probe_injected=true;
}
static void correspondence_free(void *memory) {
    xr_free(memory);
    correspondence_callback(false);
}
static void *correspondence_allocate(size_t bytes) {
    void *memory=xr_malloc(bytes);
    correspondence_callback(true);
    return memory;
}
#ifdef xr_free
#undef xr_free
#endif
#ifdef xr_malloc
#undef xr_malloc
#endif
#define xr_free(memory) correspondence_free(memory)
#define xr_malloc(bytes) correspondence_allocate(bytes)
#include "base/xcompile_resources.c"
#undef xr_free
#undef xr_malloc

#include "xir/xxir_effects.c"
#include "xir/xxir_program_packet_internal.h"
#include "xir/xxir_program_internal.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_generic.h"
#include "xir_construction_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)

static XrXirRootEffects saved_final_root,saved_final_facts;
static XrXirFunctionEffects saved_final_control;
static XrXirStatus saved_final_task_errors;
static uint32_t saved_summary_mask,saved_public_mask;
static uint64_t saved_error_row;
static void final_callback(unsigned attack) {
    XrXirEffects *effects=probe_artifact->program_final;
    EffectFinalOutcomes *final=(EffectFinalOutcomes *)effect_final_outcomes(effects);
    EffectFinalFunctions *summary=&effects->invocations->final->summary;
    CHECK(final && attack>=10 && attack<=16);
    saved_final_root=final->root[1];saved_final_facts=summary->facts[1];
    saved_final_control=final->functions[1];saved_final_task_errors=final->task_errors[1];
    saved_summary_mask=summary->functions[1].formula.constant_mask;
    saved_public_mask=final->contracts[1].formula.constant_mask;
    CHECK(final->atom_count==0 && final->words==1);
    saved_error_row=final->errors[1];
    if (attack==10) final->root[1].requires_root=!final->root[1].requires_root;
    if (attack==11) {
        final->root[1].requires_root=true;summary->facts[1].requires_root=true;
        final->contracts[1].formula.constant_mask|=XR_XIR_CALLABLE_ROOT_REQUIRED;
        summary->functions[1].formula.constant_mask|=XR_XIR_CALLABLE_ROOT_REQUIRED;
    }
    if (attack==12) final->functions[1].suspend=XR_XIR_EFFECT_MAY;
    if (attack==13) final->task_errors[1]=XR_XIR_BAD_TYPE;
    if (attack==14) final->functions[1].throws=XR_XIR_EFFECT_MAY;
    if (attack==15) {
        final->errors[1]|=UINT64_C(2);
        final->functions[1].throws=XR_XIR_EFFECT_MAY;
        final->task_errors[1]=XR_XIR_BAD_TYPE;
    }
    if (attack==16) {
        final->errors[1]|=UINT64_C(1);
        final->functions[1].throws=XR_XIR_EFFECT_UNKNOWN;
        final->task_errors[1]=XR_XIR_BAD_TYPE;
    }
}
static void final_restore(void) {
    if (probe_attack<10) return;
    XrXirEffects *effects=probe_artifact->program_final;
    EffectFinalOutcomes *final=(EffectFinalOutcomes *)effect_final_outcomes(effects);
    EffectFinalFunctions *summary=&effects->invocations->final->summary;
    final->root[1]=saved_final_root;summary->facts[1]=saved_final_facts;
    final->functions[1]=saved_final_control;final->task_errors[1]=saved_final_task_errors;
    summary->functions[1].formula.constant_mask=saved_summary_mask;
    final->contracts[1].formula.constant_mask=saved_public_mask;
    final->errors[1]=saved_error_row;
}

static XrXirArtifact *program_variant(const XrXirCompileContext *context, int64_t value, bool varied) {
    const XrXirInstruction init = {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}};
    XrXirInstruction body[] = {
        {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, value, {0}},
        {XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, 19, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {0}, {0}, 0, {0}}
    };
    uint32_t instructions = varied ? 3 : 2;
    if (!varied) body[1] = body[2];
    const XrXirInstruction cycle_a[]={{.op=XR_XIR_CALL,.immediate=3},{.op=XR_XIR_RETURN}};
    const XrXirInstruction cycle_b[]={{.op=XR_XIR_CALL,.immediate=2},{.op=XR_XIR_RETURN}};
    const XrXirBlock blocks[] = {{0, 1, 0, 0}, {0, instructions, 0, 0},{0,2,0,0}};
    const XrXirFunction functions[] = {
        {"init", 4, NULL, 0, XR_XIR_UNIT, blocks, 1, &init, 1, NULL, 0},
        {"main", 4, NULL, 0, XR_XIR_I64, blocks + 1, 1, body, instructions, NULL, 0},
        {"left",4,NULL,0,XR_XIR_UNIT,blocks+2,1,cycle_a,2,NULL,0},
        {"right",5,NULL,0,XR_XIR_UNIT,blocks+2,1,cycle_b,2,NULL,0}
    };
    const XrXirSourceModule source = {"root", 4, NULL, 0, 0};
    const XrXirFunctionIdentity identities[] = {
        {0, 0, 0, 0, 0, 0, XR_XIR_NON_MEMBER, 0, 0},
        {0, varied ? 1 : 0, 0, 0, 0, varied ? XR_XIR_FUNCTION_NO_SUSPEND : 0, XR_XIR_NON_MEMBER, 0, 0},
        {0,0,0,0,0,0,XR_XIR_NON_MEMBER,0,0},
        {0,0,0,0,0,0,XR_XIR_NON_MEMBER,0,0}
    };
    const XrXirDeclarations declarations = {&source, 1, identities, NULL, 0, NULL, 0, 0, 1, NULL};
    const XrXirModule module = {XR_XIR_BUILT, functions, probe_seedless_cycle?4:2,
        &declarations, NULL, NULL, NULL, XR_XIR_PROGRAM, NULL};
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    XrXirArtifact *checked = NULL, *closed = NULL, *lowered = NULL;
    CHECK(xir_fixture_check(context, &module, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    xr_xir_compile_artifact_free(closed);
    return lowered;
}

static XrXirArtifact *constant_program(const XrXirCompileContext *context, int64_t value) {
    return program_variant(context, value, false);
}

static uint64_t execute(XrXirProgram *program) {
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance, 1, NULL, 0) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
    XrXirValue result = {0};
    CHECK(xr_xir_instance_take_result(instance, &result) == XR_XIR_CALL_RETURNED);
    CHECK(result.type == XR_XIR_I64);
    uint64_t value = result.payload;
    xr_xir_value_drop(&result);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    return value;
}

static void normal(const XrXirCompileContext *context, int64_t expected, bool varied) {
    XrXirArtifact *artifact = program_variant(context, expected, varied);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&artifact, &program) == XR_XIR_OK && !artifact);
    CHECK(execute(program) == (uint64_t)expected);
    xr_xir_compile_program_drop(program);
    XrCompileResourceStats observed = {0};
    CHECK(xr_compile_resources_stats(context->resources, &observed) == XR_COMPILE_RESOURCE_OK);
    printf("normal=%lld live=%llu work=%llu\n", (long long)expected,
        (unsigned long long)observed.live_bytes, (unsigned long long)observed.work);
}


static void body_refusal(const XrXirCompileContext *context) {
    XrXirArtifact *artifact=constant_program(context,42),*original=artifact;
    XrXirModule *module=(XrXirModule *)xr_xir_compile_artifact_module(artifact);
    ((XrXirInstruction *)module->functions[1].instructions)[0].immediate=43;
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&artifact,&program)==XR_XIR_BAD_STRUCTURE);
    CHECK(artifact==original && !program);
    ((XrXirInstruction *)module->functions[1].instructions)[0].immediate=42;
    CHECK(xr_xir_compile_vm_program_take(&artifact,&program)==XR_XIR_OK && !artifact);
    CHECK(execute(program)==42);
    xr_xir_compile_program_drop(program);
}

static void joint_refusal(const XrXirCompileContext *context) {
    XrXirArtifact *left = program_variant(context, 42, false);
    XrXirArtifact *right = program_variant(context, 43, true);
    CHECK(xr_xir_compile_artifact_verify(left, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_verify(right, NULL) == XR_XIR_OK);
    CHECK(left->layouts[1].frame_bytes != right->layouts[1].frame_bytes);
    const XrXirFunction *functions = left->module.functions;
    const XrXirDeclarations *declarations = left->module.declarations;
    XrXirFunctionLayout *layouts = left->layouts;
    XrXirArtifact *original = left;
    left->module.functions = right->module.functions;
    left->module.declarations = right->module.declarations;
    left->layouts = right->layouts;
    XrXirProgram *program = NULL;
    XrXirStatus status = xr_xir_compile_vm_program_take(&left, &program);
    CHECK(status == XR_XIR_BAD_STRUCTURE && !program && left == original);
    left->module.functions = functions;
    left->module.declarations = declarations;
    left->layouts = layouts;
    CHECK(xr_xir_compile_vm_program_take(&left, &program) == XR_XIR_OK && !left);
    CHECK(execute(program) == 42);
    xr_xir_compile_program_drop(program);
    program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&right, &program) == XR_XIR_OK && !right);
    CHECK(execute(program) == 43);
    xr_xir_compile_program_drop(program);
    puts("Two valid bodies/layouts/declarations jointly spliced into unchanged proof rejected");
}

static void callback_refusal(const XrXirCompileContext *context,unsigned attack,bool allocation) {
    probe_allocations=allocation;
    XrXirArtifact *artifact=constant_program(context,42);
    probe_artifact=artifact;probe_attack=attack;probe_at=SIZE_MAX;
    probe_calls=0;probe_armed=true;probe_injected=false;
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&artifact,&program)==XR_XIR_OK && !artifact);
    probe_armed=false;
    size_t frees=probe_calls;
    CHECK(frees && !probe_injected && execute(program)==42);
    xr_xir_compile_program_drop(program);

    artifact=constant_program(context,42);
    XrXirArtifact *original=artifact;
    XrXirModule *module=(XrXirModule *)xr_xir_compile_artifact_module(artifact);
    XrXirFunction saved_function=module->functions[1];
    uint32_t frame=artifact->layouts[1].frame_bytes;
    probe_artifact=artifact;probe_at=frees-1;probe_calls=0;probe_injected=false;probe_armed=true;
    program=NULL;
    XrXirStatus status=xr_xir_compile_vm_program_take(&artifact,&program);
    probe_armed=false;
    fprintf(stderr,"Final callback attack%u allocation%u ordinal%zu status%u artifact%p program%p\n",
        attack,allocation,frees-1,status,(void *)artifact,(void *)program);
    CHECK(probe_injected && status==XR_XIR_BAD_STRUCTURE && !program && artifact==original);
    final_restore();
    ((XrXirFunction *)module->functions)[1]=saved_function;
    ((XrXirInstruction *)module->functions[1].instructions)[0].immediate=42;
    artifact->layouts[1].frame_bytes=frame;
    ((XrXirFunctionIdentity *)module->declarations->functions)[1].test_timeout_seconds=0;
    CHECK(xr_xir_compile_vm_program_take(&artifact,&program)==XR_XIR_OK && !artifact);
    CHECK(execute(program)==42);
    xr_xir_compile_program_drop(program);
    printf("last %s mutation attack=%u ordinal=%zu rejected; same-owner recovery returns42\n",
        allocation ? "allocation" : "free",attack,frees-1);
}

static void seal_callback_refusal(const XrXirCompileContext *context,bool allocation,unsigned attack) {
    probe_allocations=allocation;
    XrXirArtifact *artifact = constant_program(context, 42);
    XrXirCallEntry entries[2];
    XrXirVmBinding bindings[2];
    for (uint32_t f = 0; f < 2; ++f)
        CHECK(xr_xir_compile_vm_bind(artifact, f, &bindings[f], &entries[f]) == XR_XIR_OK);
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, artifact->target,
        entries, 2, artifact->module.declarations, {0}, artifact->module.types,
        xir_program_packet_borrow(artifact)};
    XrXirProgram *program = NULL;
    probe_artifact = artifact;
    const XrXirSourceModule *saved_modules=spec.declarations->modules;
    const XrXirFunctionIdentity *saved_functions=spec.declarations->functions;
    probe_attack = attack;
    probe_at = SIZE_MAX;
    probe_calls = 0;
    probe_injected = false;
    probe_armed = true;
    CHECK(xr_xir_compile_program_seal(context, &spec, &program) == XR_XIR_OK);
    probe_armed = false;
    size_t frees = probe_calls;
    CHECK(frees && !probe_injected && execute(program) == 42);
    xr_xir_compile_program_drop(program);
    program = NULL;
    probe_at = frees - 1;
    probe_calls = 0;
    probe_injected = false;
    probe_armed = true;
    XrXirStatus status = xr_xir_compile_program_seal(context, &spec, &program);
    probe_armed = false;
    CHECK(probe_injected && status == XR_XIR_BAD_STRUCTURE && !program);
    ((XrXirDeclarations *)spec.declarations)->modules=saved_modules;
    ((XrXirDeclarations *)spec.declarations)->functions=saved_functions;
    ((XrXirFunctionIdentity *)spec.declarations->functions)[1].exported = 0;
    CHECK(xr_xir_compile_program_seal(context, &spec, &program) == XR_XIR_OK);
    CHECK(execute(program) == 42);
    xr_xir_compile_program_drop(program);
    xr_xir_compile_artifact_free(artifact);
    printf("Public seal last %s attack=%u rejected; original storage recovery returns42\n",
        allocation ? "allocation" : "free",attack);
}

static void metadata_storage_refusal(const XrXirCompileContext *context) {
    const XrXirTypes *types = NULL;
    const XrXirDeclarations *declarations = NULL;
    CHECK(xir_effects_program_metadata(context, (const XrXirEffects *)(uintptr_t)1,
        &types, &declarations) == XR_XIR_BAD_STRUCTURE && !types && !declarations);
    XrXirArtifact *artifact = constant_program(context, 42);
    XrXirEffects *effects = NULL;
    CHECK(xr_xir_compile_effects_analyze(artifact, &effects) == XR_XIR_OK);
    CHECK(xir_effects_program_metadata(context, effects, &types, &declarations) == XR_XIR_OK);
    XrXirTypes *mutable_types = (XrXirTypes *)types;
    XrXirDeclarations *mutable_declarations = (XrXirDeclarations *)declarations;
    XrXirTypes saved_types = *types;
    XrXirDeclarations saved_declarations = *declarations;
    XrXirSourceModule *module = (XrXirSourceModule *)declarations->modules;
    XrXirSourceModule saved_module = *module;
    for (unsigned attack = 0; attack < 7; ++attack) {
        if (attack == 0) mutable_declarations->modules = (const XrXirSourceModule *)(uintptr_t)1;
        if (attack == 1) mutable_declarations->functions = (const XrXirFunctionIdentity *)(uintptr_t)1;
        if (attack == 2) module->name = (const char *)(uintptr_t)1;
        if (attack == 3) {
            mutable_declarations->slots = (const XrXirSlot *)(uintptr_t)1;
            mutable_declarations->slot_count = 1;
        }
        if (attack == 4) {
            mutable_declarations->literals = (const XrXirLiteral *)(uintptr_t)1;
            mutable_declarations->literal_count = 1;
        }
        if (attack == 5) {
            mutable_types->nodes = (const XrXirTypeNode *)(uintptr_t)1;
            mutable_types->count = 1;
        }
        if (attack == 6) mutable_types->nominals = (const XrXirNominalTable *)(uintptr_t)1;
        const XrXirTypes *rejected_types = NULL;
        const XrXirDeclarations *rejected_declarations = NULL;
        CHECK(xir_effects_program_metadata(context, effects, &rejected_types, &rejected_declarations)
            == XR_XIR_BAD_STRUCTURE && !rejected_types && !rejected_declarations);
        *mutable_types = saved_types;
        *mutable_declarations = saved_declarations;
        *module = saved_module;
        CHECK(xir_effects_program_metadata(context, effects, &rejected_types, &rejected_declarations) == XR_XIR_OK);
        CHECK(rejected_types == types && rejected_declarations == declarations);
    }
    xr_xir_compile_effects_free(effects);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&artifact, &program) == XR_XIR_OK && !artifact);
    CHECK(execute(program) == 42);
    xr_xir_compile_program_drop(program);
    puts("Owned metadata storage rejects forged addresses without reading them; recovery returns42");
}

static void input_header_refusal(const XrXirCompileContext *context,unsigned attack) {
    XrXirArtifact *artifact=constant_program(context,42);
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    XrXirVmBinding bindings[2];XrXirCallEntry entries[2];
    for (uint32_t f=0;f<2;++f)
        CHECK(xr_xir_compile_vm_bind(artifact,f,&bindings[f],&entries[f])==XR_XIR_OK);
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,*xr_xir_compile_artifact_target(artifact),
        entries,2,module->declarations,{0},module->types,{0}};
    XrXirProgramInput *real=NULL;
    CHECK(xir_program_input_take(context,&artifact,&real)==XR_XIR_OK && !artifact);
    XrXirProgramInput *forged=(XrXirProgramInput *)(uintptr_t)1;
    void *storage=NULL;
    XrXirCompileContext foreign={0};
    if (attack==1 || attack==2) {
        size_t bytes=attack==1?sizeof(*real)-1:sizeof(*real)+1;
        CHECK(xr_compile_resources_alloc(context->resources,bytes,&storage)==XR_COMPILE_RESOURCE_OK);
        forged=attack==1?(XrXirProgramInput *)storage:(XrXirProgramInput *)((uint8_t *)storage+1);
    }
    if (attack==3) {
        const XrCompileResourceLimits caps={67108864,8388608,128000000};
        foreign.limits=context->limits;
        CHECK(xr_compile_resources_new(&caps,&foreign.resources)==XR_COMPILE_RESOURCE_OK);
        CHECK(xr_compile_resources_alloc(foreign.resources,sizeof(*real),&storage)==XR_COMPILE_RESOURCE_OK);
        memcpy(storage,real,sizeof(*real));forged=storage;
    }
    XrXirProgramInput *saved=forged;
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirProgram *program=NULL;
    XrXirStatus status=xir_program_receive(context,&spec,&forged,NULL,&program);
    CHECK(status==XR_XIR_BAD_STRUCTURE && forged==saved && !program);
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(before.allocation_count==after.allocation_count && before.allocated_bytes==after.allocated_bytes &&
        before.live_bytes==after.live_bytes && before.peak_bytes==after.peak_bytes && after.work>=before.work);
    xr_compile_resources_free(storage);
    xr_compile_resources_release(foreign.resources);
    CHECK(xir_program_receive(context,&spec,&real,NULL,&program)==XR_XIR_OK && !real);
    CHECK(execute(program)==42);xr_xir_compile_program_drop(program);
    printf("Input header attack%u refused before allocation; same-owner recovery returns42\n",attack);
}

int main(int argc,char **argv) {
    const XrCompileResourceLimits caps={67108864,8388608,128000000};
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    if (argc==2 && !strcmp(argv[1],"seedless-cycle")) {
        probe_seedless_cycle=true;
        normal(&context,42,false);
        callback_refusal(&context,15,false);callback_refusal(&context,15,true);
        callback_refusal(&context,16,false);callback_refusal(&context,16,true);
        CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK &&
            after.live_bytes==before.live_bytes);
        xr_compile_resources_release(context.resources);return 0;
    }
    if (argc==2 && !strncmp(argv[1],"input-header-",13)) {
        unsigned attack=(unsigned)strtoul(argv[1]+13,NULL,10);CHECK(attack<4);
        input_header_refusal(&context,attack);
        CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK &&
            after.live_bytes==before.live_bytes);
        xr_compile_resources_release(context.resources);return 0;
    }
    if (argc==2) {
        const char *cases[]={"final","joint","suspend","task-errors","throws","joint-error","unknown-error"};
        unsigned selected=0;
        for (;selected<7 && strcmp(argv[1],cases[selected]);++selected) {}
        CHECK(selected<7);
        callback_refusal(&context,selected+10,false);
        callback_refusal(&context,selected+10,true);
        CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK);
        CHECK(after.live_bytes==before.live_bytes);
        xr_compile_resources_release(context.resources);
        return 0;
    }
    normal(&context,42,false);normal(&context,43,true);body_refusal(&context);joint_refusal(&context);
    metadata_storage_refusal(&context);
    for(unsigned attack=0;attack<4;++attack) {
        callback_refusal(&context,attack,false);
        callback_refusal(&context,attack,true);
    }
    for(unsigned attack=7;attack<10;++attack) {
        callback_refusal(&context,attack,false);
        callback_refusal(&context,attack,true);
    }
    for(unsigned attack=4;attack<7;++attack) {
        seal_callback_refusal(&context,false,attack);
        seal_callback_refusal(&context,true,attack);
    }
    callback_refusal(&context,10,false);
    callback_refusal(&context,10,true);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==before.live_bytes);
    xr_compile_resources_release(context.resources);
    puts("Program current body/layout/metadata correspondence and final-free recovery PASS");
    return 0;
}
