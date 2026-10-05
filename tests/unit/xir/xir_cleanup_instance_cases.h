/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cleanup_instance_cases.h - Revoked external gates and scoped draining access
 */
#ifndef XIR_CLEANUP_INSTANCE_CASES_H
#define XIR_CLEANUP_INSTANCE_CASES_H
#include "xir/xxir_program.h"
#include "xir/xxir_instance_value.h"
typedef struct DrainState { uint32_t pc; XrXirValue values[2]; int64_t panic; } DrainState;
typedef struct DrainWitness {
    XrXirInstance *instance; XrXirCall *activation; XrXirValue escaped, generated;
    uint32_t bodies, outputs, leases; bool late_gate, malformed;
} DrainWitness;
static XrXirAction drain_initializer(XrXirCallView *view) {
    XrXirValue value = {XR_XIR_I64,0,1};
    CHECK(xr_xir_instance_slot_write(view,0,&value,true) == XR_XIR_CALL_READY);
    return exit_action(XR_XIR_ACTION_RETURN);
}
static XrXirAction drain_owner(XrXirCallView *view) {
    DrainWitness *w = (DrainWitness *)view->environment; DrainState *s = view->state;
    if (view->phase == XR_XIR_CALL_EXIT) {
        CHECK(view->exit.status == XR_XIR_CALL_CANCELLED);
        if (s->pc == 1) { ++s->pc; return (XrXirAction){XR_XIR_ACTION_CALL,2,s->values,w->late_gate ? 1u : 2u,{0},{0},XR_XIR_ACTION_CLEANUP}; }
        CHECK(s->pc == 2 && view->inbox.status == XR_XIR_CALL_RETURNED);
        return exit_action(XR_XIR_ACTION_EXIT_DONE);
    }
    CHECK(!s->pc++);
    XrXirValue value = {XR_XIR_I64,0,16};
    CHECK(xr_xir_instance_cell(view,(XrXirType)257,&value,&s->values[0]) == XR_XIR_CALL_READY);
    if (!w->late_gate) {
        CHECK(xr_xir_instance_function(view,(XrXirType)256,3,s->values,1,&s->values[1]) == XR_XIR_CALL_READY);
        CHECK(xr_xir_value_copy(&s->values[1],&w->escaped) == XR_XIR_VALUE_OK);
    }
    return exit_action(XR_XIR_ACTION_SUSPEND);
}
static XrXirAction drain_body(XrXirCallView *view) {
    DrainWitness *w = (DrainWitness *)view->environment; uint32_t *pc = view->state;
    w->activation = view->activation;
    CHECK(xr_xir_instance_state(w->instance) == XR_XIR_INSTANCE_DRAINING);
    CHECK(xr_xir_instance_free(w->instance) == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_stop(w->instance) == XR_XIR_CALL_READY);
    if (!(*pc)++) {
        ++w->bodies;
        XrXirValue value = {0}; XrXirCallView copied = *view;
        CHECK(xr_xir_instance_cell_read(&copied,&view->arguments[0],&value) == XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_instance_cell_read(view,&view->arguments[0],&value) == XR_XIR_CALL_READY && value.payload == 16);
        value.payload = 17;
        CHECK(xr_xir_instance_cell_write(view,&view->arguments[0],&value) == XR_XIR_CALL_READY);
        XrXirValue cell = {0};
        CHECK(xr_xir_instance_cell(view,(XrXirType)257,&value,&cell) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_function(view,(XrXirType)256,3,&cell,1,&w->generated) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start_function(w->instance,&w->generated,NULL,0) == XR_XIR_CALL_BAD_STATE);
        xr_xir_value_drop(&cell);
        const XrXirValue *function = w->late_gate ? &w->generated : &view->arguments[1];
        uint32_t entry = UINT32_MAX;
        CHECK(xr_xir_instance_resolve_function(view,function,&entry) == XR_XIR_CALL_READY && entry == 3);
        return (XrXirAction){XR_XIR_ACTION_CALL,entry,NULL,0,*function,{0},0};
    }
    if (*pc == 2) {
        CHECK(view->inbox.status == XR_XIR_CALL_RETURNED && view->inbox.value.payload == 21);
        XrXirValue value = {XR_XIR_I64,0,42};
        CHECK(xr_xir_instance_slot_write(view,0,&value,false) == XR_XIR_CALL_READY);
        value = (XrXirValue){0};
        CHECK(xr_xir_instance_slot_read(view,0,&value) == XR_XIR_CALL_READY && value.payload == 42);
        return (XrXirAction){XR_XIR_ACTION_OUTPUT,XR_XIR_STDOUT,&view->inbox.value,1,{0},{0},0};
    }
    if (w->malformed) return (XrXirAction){XR_XIR_ACTION_CONTINUE,0,NULL,0,{XR_XIR_I64,0,1},{0},0};
    return exit_action(XR_XIR_ACTION_RETURN);
}
static XrXirAction drain_callee(XrXirCallView *view) {
    DrainState *s = view->state;
    CHECK(xr_xir_call_cleanup_active(view->activation));
    if (!s->pc) {
        XrXirValue value = {0};
        CHECK(xr_xir_instance_cell_read(view,&view->arguments[0],&value) == XR_XIR_CALL_READY && value.payload == 17);
        value.payload = 21;
        CHECK(xr_xir_instance_cell_write(view,&view->arguments[0],&value) == XR_XIR_CALL_READY);
        return xr_xir_instance_panic_land(view,s,xr_xir_call_numeric_fault(XR_XIR_RUN_DIVIDE_BY_ZERO,false),
            (uint32_t)offsetof(DrainState,panic),1,&s->pc);
    }
    XrXirValue info = {XR_XIR_PANIC_INFO,0,s->panic};
    CHECK(xr_xir_value_argument(&info,NULL,XR_XIR_PANIC_INFO));
    return (XrXirAction){XR_XIR_ACTION_RETURN,0,NULL,0,{XR_XIR_I64,0,21},{0},0};
}
static void drain_release(XrXirCallView *view, XrXirCallStatus reason) {
    (void)reason; DrainState *s = view->state;
    CHECK(!xr_xir_call_cleanup_active(view->activation));
    xr_xir_value_drop(&s->values[0]); xr_xir_value_drop(&s->values[1]);
    xr_xir_owned_slot_clear(s,(uint32_t)offsetof(DrainState,panic));
}
static XrXirOutputStatus drain_output(void *context, const XrXirOutputGroup *group) {
    DrainWitness *w = context;
    CHECK(!xr_xir_call_cleanup_active(w->activation));
    CHECK(xr_xir_instance_start_function(w->instance,&w->generated,NULL,0) == XR_XIR_CALL_BAD_STATE);
    CHECK(group->count == 1 && group->values[0].type == XR_XIR_I64 && group->values[0].payload == 21);
    CHECK(xr_xir_instance_stop(w->instance) == XR_XIR_CALL_READY);
    ++w->outputs; return XR_XIR_OUTPUT_OK;
}
static void drain_lease(void *context) { ++((DrainWitness *)context)->leases; }
static XrXirStatus drain_proof(const XrXirCompileContext *context, const XrXirDeclarations *declarations,
    const XrXirTypes *types, const XrXirType *parameters, bool late_gate, XrXirArtifact **output) {
    XrXirInstruction unit = {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    XrXirInstruction value[] = {{XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirBlock unit_block = {0,1,0, 0}, value_block = {0,2,0, 0};
    XrXirInstruction callee_value[] = {value[0],value[1]}; callee_value[1].args[0] = 1;
    XrXirFunction functions[] = {
        {"init",4,NULL,0,XR_XIR_UNIT,&unit_block,1,&unit,1,NULL,0},
        {"main",4,NULL,0,XR_XIR_I64,&value_block,1,value,2,NULL,0},
        {"cleanup",7,parameters,late_gate ? 1u : 2u,XR_XIR_UNIT,&unit_block,1,&unit,1,NULL,0},
        {"callee",6,parameters,1,XR_XIR_I64,&value_block,1,callee_value,2,NULL,0}};
    XrXirModule built = {XR_XIR_BUILT,functions,4,declarations,NULL,types,NULL, XR_XIR_PROGRAM, NULL};
    XrXirArtifact *checked = NULL;
    XrXirStatus status = xr_xir_compile_check(context,&built,&checked,NULL);
    XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    if (status == XR_XIR_OK) status = xr_xir_compile_lower(checked,&target,output,NULL);
    xr_xir_compile_artifact_free(checked); return status;
}
static void cleanup_instance_case(bool late_gate, bool malformed) {
    DrainWitness w = {0}; w.late_gate = late_gate; w.malformed = malformed;
    XrXirTypeNode nodes[2] = {0}; nodes[0].kind = XR_XIR_TYPE_CALLABLE; nodes[0].result = XR_XIR_I64;
    nodes[1].kind = XR_XIR_TYPE_CELL; nodes[1].element = XR_XIR_I64;
    XrXirTypes types = {nodes,2,NULL, NULL};
    XrXirType parameters[] = {(XrXirType)257,(XrXirType)256};
    XrXirCallEntry entries[] = {
        {XR_XIR_CALL_ABI_VERSION,NULL,0,XR_XIR_UNIT,0,drain_initializer,NULL,&w,0,0},
        {XR_XIR_CALL_ABI_VERSION,NULL,0,XR_XIR_I64,sizeof(DrainState),drain_owner,drain_release,&w,XR_XIR_ENTRY_EXIT,0},
        {XR_XIR_CALL_ABI_VERSION,parameters,late_gate ? 1u : 2u,XR_XIR_UNIT,sizeof(uint32_t),drain_body,NULL,&w,0,2},
        {XR_XIR_CALL_ABI_VERSION,parameters,1,XR_XIR_I64,sizeof(DrainState),drain_callee,drain_release,&w,0,0}};
    XrXirFunctionIdentity ids[] = {{0},{0,1,0,0,0, 0, XR_XIR_NON_MEMBER, 0, 0},{0,0,0,0,2, 0, XR_XIR_NON_MEMBER, 0, 0},{0}};
    XrXirSourceModule module = {"main",4,NULL,0,0}; XrXirSlot slot = {0,XR_XIR_I64,1};
    XrXirDeclarations declarations = {&module,1,ids,&slot,1,NULL,0,0,1, NULL};
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        entries,4,&declarations,{&w,drain_lease},&types,{0}};
    SourceFixtureOwner compiler={0}; cleanup_fixture_owner_new(&compiler);
    XrXirArtifact *proof=NULL;
    CHECK(drain_proof(&compiler.context,&declarations,&types,parameters,late_gate,&proof) == XR_XIR_OK);
    spec.proof = xr_xir_compile_program_proof(proof);
    XrXirProgram *program = NULL;
    spec.abi_version = 18;
    CHECK(xr_xir_compile_program_seal(&compiler.context,&spec,&program) == XR_XIR_BAD_LAYOUT && !program);
    spec.abi_version = XR_XIR_PROGRAM_ABI_VERSION;
    entries[1].abi_version = 18;
    CHECK(xr_xir_compile_program_seal(&compiler.context,&spec,&program) == XR_XIR_BAD_LAYOUT && !program);
    entries[1].abi_version = XR_XIR_CALL_ABI_VERSION;
    entries[3].flags = XR_XIR_ENTRY_EXIT;
    CHECK(xr_xir_compile_program_seal(&compiler.context,&spec,&program) == XR_XIR_BAD_STRUCTURE && !program);
    entries[3].flags = 0; ids[2].cleanup_owner = entries[2].cleanup_owner = 1; entries[0].flags = XR_XIR_ENTRY_EXIT;
    CHECK(xr_xir_compile_program_seal(&compiler.context,&spec,&program) == XR_XIR_BAD_STRUCTURE && !program);
    ids[2].cleanup_owner = entries[2].cleanup_owner = 2; entries[0].flags = 0;
    CHECK(!w.leases);
    CHECK(xr_xir_compile_program_seal(&compiler.context,&spec,&program) == XR_XIR_OK);
    xr_xir_compile_artifact_free(proof);
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY); config.output = (XrXirOutputProvider) {XR_XIR_CALL_ABI_VERSION, 0, drain_output, &w};
    CHECK(xr_xir_instance_new(program,&config,&w.instance) == XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    CHECK(xr_xir_instance_start(w.instance,1,NULL,0) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(w.instance, UINT64_MAX).outcome.status == XR_XIR_CALL_SUSPENDED);
    CHECK(xr_xir_instance_stop(w.instance) == (malformed ? XR_XIR_CALL_BAD_STATE : XR_XIR_CALL_READY));
    CHECK(w.bodies == 1 && w.outputs == 1);
    CHECK(xr_xir_instance_poll_bounded(w.instance, UINT64_MAX).outcome.status == (malformed ? XR_XIR_CALL_BAD_STATE : XR_XIR_CALL_CANCELLED));
    CHECK(xr_xir_instance_start_function(w.instance,late_gate ? &w.generated : &w.escaped,NULL,0) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_stop(w.instance) == XR_XIR_CALL_READY && w.bodies == 1);
    CHECK(xr_xir_instance_free(w.instance) == XR_XIR_CALL_READY && !w.leases);
    xr_xir_value_drop(&w.escaped); CHECK(!w.leases); xr_xir_value_drop(&w.generated); CHECK(w.leases == 1 && !live);
    CHECK(!cleanup_runtime_live && !cleanup_runtime_bytes);
    source_fixture_owner_free(&compiler); instance_compile_zero();
    puts("Draining cell, function admission, PanicInfo, output and revoked gate passed");
}
typedef struct CleanupCompilerFixture {
    DrainWitness witness;
    XrXirTypeNode nodes[2]; XrXirTypes types; XrXirType parameters[2];
    XrXirCallEntry entries[4]; XrXirFunctionIdentity identities[4];
    XrXirSourceModule module; XrXirSlot slot; XrXirDeclarations declarations;
    XrXirProgramSpec spec;
} CleanupCompilerFixture;
static void cleanup_compiler_fixture(CleanupCompilerFixture *f, bool late) {
    memset(f,0,sizeof(*f)); f->witness.late_gate=late;
    f->nodes[0].kind=XR_XIR_TYPE_CALLABLE; f->nodes[0].result=XR_XIR_I64;
    f->nodes[1].kind=XR_XIR_TYPE_CELL; f->nodes[1].element=XR_XIR_I64;
    f->types=(XrXirTypes){f->nodes,2,NULL,NULL};
    f->parameters[0]=(XrXirType)257; f->parameters[1]=(XrXirType)256;
    f->entries[0]=(XrXirCallEntry){XR_XIR_CALL_ABI_VERSION,NULL,0,XR_XIR_UNIT,0,drain_initializer,NULL,&f->witness,0,0};
    f->entries[1]=(XrXirCallEntry){XR_XIR_CALL_ABI_VERSION,NULL,0,XR_XIR_I64,sizeof(DrainState),drain_owner,drain_release,&f->witness,XR_XIR_ENTRY_EXIT,0};
    f->entries[2]=(XrXirCallEntry){XR_XIR_CALL_ABI_VERSION,f->parameters,late?1u:2u,XR_XIR_UNIT,sizeof(uint32_t),drain_body,NULL,&f->witness,0,2};
    f->entries[3]=(XrXirCallEntry){XR_XIR_CALL_ABI_VERSION,f->parameters,1,XR_XIR_I64,sizeof(DrainState),drain_callee,drain_release,&f->witness,0,0};
    f->identities[1].exported=1; f->identities[2].cleanup_owner=2;
    f->module=(XrXirSourceModule){"main",4,NULL,0,0}; f->slot=(XrXirSlot){0,XR_XIR_I64,1};
    f->declarations=(XrXirDeclarations){&f->module,1,f->identities,&f->slot,1,NULL,0,0,1,NULL};
    f->spec=(XrXirProgramSpec){XR_XIR_PROGRAM_ABI_VERSION,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        f->entries,4,&f->declarations,{&f->witness,drain_lease},&f->types,{0}};
}
static XrXirStatus cleanup_compiler_pipeline(const XrXirCompileContext *context,
    CleanupCompilerFixture *f, XrXirProgram **output) {
    XrXirArtifact *proof=NULL;
    XrXirStatus status=drain_proof(context,&f->declarations,&f->types,f->parameters,f->witness.late_gate,&proof);
    if (status==XR_XIR_OK) {
        f->spec.proof=xr_xir_compile_program_proof(proof);
        status=xr_xir_compile_program_seal(context,&f->spec,output);
        f->spec.proof=(XrXirProgramProof){0};
    }
    xr_xir_compile_artifact_free(proof);
    return status;
}
static XrCompileResourceStats cleanup_compiler_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK); return stats;
}
static XrXirCompileContext cleanup_compiler_context(XrCompileResourceLimits limits) {
    XrXirCompileContext context={0};
    CHECK(limits.allocated_bytes<=40960 && limits.live_bytes<=8192 && limits.work<=65536);
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits(); return context;
}
static void cleanup_compiler_free(XrXirCompileContext *context, XrCompileResourceStats baseline) {
    CHECK(cleanup_compiler_stats(context).live_bytes==baseline.live_bytes);
    xr_compile_resources_release(context->resources); *context=(XrXirCompileContext){0};
    instance_compile_zero(); CHECK(!cleanup_runtime_live && !cleanup_runtime_bytes);
}
static void cleanup_compiler_faults(bool late) {
    size_t sites=0;
    for(size_t pass=0;pass<=sites;++pass) {
        XrXirCompileContext context=cleanup_compiler_context((XrCompileResourceLimits){40960,8192,65536});
        XrCompileResourceStats baseline=cleanup_compiler_stats(&context);
        CleanupCompilerFixture f; cleanup_compiler_fixture(&f,late);
        instance_compile_attempts=0; instance_compile_injected=false;
        instance_compile_fail_at=pass?pass-1:SIZE_MAX;
        XrXirProgram *program=NULL;
        XrXirStatus status=cleanup_compiler_pipeline(&context,&f,&program);
        size_t seen=instance_compile_attempts; instance_compile_fail_at=SIZE_MAX;
        if(!pass) {CHECK(status==XR_XIR_OK && program);sites=seen;CHECK(sites && sites<10000);}
        else CHECK(status==XR_XIR_OUT_OF_MEMORY && !program && instance_compile_injected && seen>=pass);
        CHECK(!f.witness.leases);
        xr_xir_compile_program_drop(program);
        CHECK(f.witness.leases==(pass?0u:1u));
        cleanup_compiler_free(&context,baseline);
    }
    printf("Cleanup same-graph compiler OOM sites=%zu late=%u; no partial lease; physical0\n",sites,(unsigned)late);
}
static void cleanup_compiler_boundaries(bool late) {
    XrCompileResourceStats exact={0};
    for(unsigned pass=0;pass<5;++pass) {
        XrCompileResourceLimits limits={40960,8192,65536};
        if(pass) {
            limits=(XrCompileResourceLimits){exact.allocated_bytes,exact.peak_bytes,exact.work};
            if(pass==2)--limits.allocated_bytes;
            if(pass==3)--limits.live_bytes;
            if(pass==4)--limits.work;
        }
        XrXirCompileContext context=cleanup_compiler_context(limits);
        XrCompileResourceStats baseline=cleanup_compiler_stats(&context);
        CleanupCompilerFixture f; cleanup_compiler_fixture(&f,late);
        XrXirProgram *program=NULL;
        XrXirStatus status=cleanup_compiler_pipeline(&context,&f,&program);
        XrCompileResourceStats actual=cleanup_compiler_stats(&context);
        if(pass<2) {CHECK(status==XR_XIR_OK && program); if(!pass)exact=actual;}
        else CHECK(status==XR_XIR_BUDGET && !program && !f.witness.leases);
        xr_xir_compile_program_drop(program); CHECK(f.witness.leases==(pass<2?1u:0u));
        cleanup_compiler_free(&context,baseline);
    }
    printf("Cleanup compiler exact allocated/live/work=%llu/%llu/%llu late=%u; all minus1 rejected\n",
        (unsigned long long)exact.allocated_bytes,(unsigned long long)exact.peak_bytes,
        (unsigned long long)exact.work,(unsigned)late);
}
static void cleanup_compiler_occupied(void) {
    XrXirCompileContext context=cleanup_compiler_context((XrCompileResourceLimits){40960,8192,65536});
    XrCompileResourceStats baseline=cleanup_compiler_stats(&context);
    XrXirProgram *program=(XrXirProgram *)(uintptr_t)1;
    CHECK(xr_xir_compile_program_seal(&context,(const XrXirProgramSpec *)(uintptr_t)1,&program)==XR_XIR_BAD_STRUCTURE);
    CHECK(program==(XrXirProgram *)(uintptr_t)1);
    XrCompileResourceStats after=cleanup_compiler_stats(&context);
    CHECK(!memcmp(&baseline,&after,sizeof(after)));
    cleanup_compiler_free(&context,baseline);
    XrCompileResourceLimits limits={40960,8192,65536}; XrCompileResources *resources=NULL;
    instance_compile_injected=false; instance_compile_fail_at=instance_compile_attempts;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OUT_OF_MEMORY && !resources);
    instance_compile_fail_at=SIZE_MAX; CHECK(instance_compile_injected); instance_compile_zero();
}
static void cleanup_compiler_cases(void) {
    cleanup_compiler_faults(false); cleanup_compiler_faults(true);
    cleanup_compiler_boundaries(false); cleanup_compiler_boundaries(true); cleanup_compiler_occupied();
}
static void cleanup_instance_cases(void) {
    cleanup_instance_case(false,false); cleanup_instance_case(true,false); cleanup_instance_case(false,true);
}
#endif // XIR_CLEANUP_INSTANCE_CASES_H
