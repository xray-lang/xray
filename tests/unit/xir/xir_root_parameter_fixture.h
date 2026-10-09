/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_root_parameter_fixture.h - Independent owned parameter evidence probes
 *
 * KEY CONCEPT:
 *   Copying raw evidence tests lifetime only; complete checking proves effects.
 */

#ifndef XIR_ROOT_PARAMETER_FIXTURE_H
#define XIR_ROOT_PARAMETER_FIXTURE_H

#include "xir/xxir_internal.h"
#include "xir/xxir_declarations.h"
#include "base/xmalloc.h"

typedef struct RootParameterAllocation { void *pointer; size_t bytes; } RootParameterAllocation;
typedef struct RootParameterMark { size_t blocks; size_t bytes; } RootParameterMark;
static RootParameterAllocation rp_blocks[8192];
static size_t rp_live, rp_live_bytes, rp_attempts, rp_fail_at = SIZE_MAX;
static bool rp_injected;

static inline RootParameterMark rp_mark(void) { return (RootParameterMark){rp_live,rp_live_bytes}; }
static inline void rp_balanced(RootParameterMark mark) {
    CHECK(rp_live == mark.blocks && rp_live_bytes == mark.bytes);
}
static void *rp_malloc(size_t bytes) {
    if (rp_attempts++ == rp_fail_at) { rp_injected = true; return NULL; }
    void *memory = xr_malloc(bytes);
    if (memory) {
        CHECK(rp_live < 8192 && bytes <= SIZE_MAX - rp_live_bytes);
        rp_blocks[rp_live++] = (RootParameterAllocation){memory,bytes};
        rp_live_bytes += bytes;
    }
    return memory;
}
static void rp_free(void *memory) {
    if (!memory) return;
    size_t at = 0;
    while (at < rp_live && rp_blocks[at].pointer != memory) ++at;
    CHECK(at < rp_live && rp_blocks[at].bytes <= rp_live_bytes);
    rp_live_bytes -= rp_blocks[at].bytes;
    rp_blocks[at] = rp_blocks[--rp_live];
    xr_free(memory);
}

#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) rp_malloc(bytes)
#define xr_free(memory) rp_free(memory)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")

static inline XrCompileResourceLimits rp_caps(void) {
    return (XrCompileResourceLimits){UINT64_C(8388608),UINT64_C(4194304),UINT64_C(4000000)};
}
static inline XrXirCompileContext rp_owner(XrCompileResourceLimits caps) {
    XrCompileResourceLimits maximum = rp_caps();
    CHECK(caps.allocated_bytes <= maximum.allocated_bytes && caps.live_bytes <= maximum.live_bytes &&
        caps.work <= maximum.work);
    XrXirCompileContext context = {0};
    CHECK(xr_compile_resources_new(&caps,&context.resources) == XR_COMPILE_RESOURCE_OK);
    context.limits = xr_xir_compile_default_limits();
    context.limits.functions = 32;
    context.limits.parameters = 16;
    context.limits.instructions = 128;
    return context;
}
static inline XrCompileResourceStats rp_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats result = {0};
    CHECK(xr_compile_resources_stats(context->resources,&result) == XR_COMPILE_RESOURCE_OK);
    return result;
}
static inline void rp_owner_free(XrXirCompileContext *context, uint64_t baseline) {
    CHECK(rp_stats(context).live_bytes == baseline);
    xr_compile_resources_release(context->resources);
    *context = (XrXirCompileContext){0};
}

typedef struct RootParameterFixture {
    XrXirTypeNode callable;
    XrXirTypes types;
    XrXirType parameter;
    XrXirInstruction instructions[2];
    XrXirBlock block;
    XrXirFunction function;
    XrXirEffectParameter effect_parameter;
    XrXirRootTerm term;
    XrXirFunctionEffectContract contract;
    XrXirProvenance evidence;
    XrXirModule module;
    XrXirArtifact source;
    XrXirEffectArgument effect_argument;
    XrXirOrigin origin;
    XrXirProvenance instance;
} RootParameterFixture;

static inline void rp_fixture(RootParameterFixture *fixture, const XrXirCompileContext *context) {
    memset(fixture,0,sizeof(*fixture));
    fixture->callable = (XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,
        .flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    fixture->types = (XrXirTypes){.nodes=&fixture->callable,.count=1};
    fixture->parameter = (XrXirType)256;
    fixture->instructions[0] = (XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=0};
    fixture->instructions[1] = (XrXirInstruction){.op=XR_XIR_RETURN,.args={1,0}};
    fixture->block = (XrXirBlock){.count=2};
    fixture->function = (XrXirFunction){.name="apply",.name_length=5,.parameters=&fixture->parameter,
        .parameter_count=1,.result=XR_XIR_I64,.blocks=&fixture->block,.block_count=1,
        .instructions=fixture->instructions,.instruction_count=2};
    fixture->effect_parameter = (XrXirEffectParameter){.kind=XR_XIR_EFFECT_PARAMETER_VARIABLE,
        .uses=XR_XIR_EFFECT_USE_INVOKE};
    fixture->term = (XrXirRootTerm){.kind=XR_XIR_ROOT_TERM_PARAMETER,.index=0};
    fixture->contract = (XrXirFunctionEffectContract){.parameter_count=1,.parameters=&fixture->effect_parameter,
        .formula={.constant_mask=0,.term_count=1,.terms=&fixture->term}};
    fixture->evidence = (XrXirProvenance){.kind=XR_XIR_EVIDENCE_TEMPLATE,
        .contracts=&fixture->contract,.contract_count=1};
    fixture->module = (XrXirModule){.stage=XR_XIR_CHECKED,.functions=&fixture->function,
        .function_count=1,.types=&fixture->types,.provenance=&fixture->evidence,.linkage_kind=XR_XIR_PROGRAM};
    fixture->source.module = fixture->module;
    fixture->source.context = *context;
    fixture->effect_argument = (XrXirEffectArgument){.parameter=0,.type=(XrXirType)256};
    fixture->origin = (XrXirOrigin){.function=0,.effect_arguments=&fixture->effect_argument,.effect_argument_count=1};
    fixture->instance = (XrXirProvenance){.kind=XR_XIR_EVIDENCE_INSTANCE,.source=&fixture->source,
        .origins=&fixture->origin,.count=1};
}

/* Literal actual Fn2 binding to an Fn8 declaration. The contract records a
 * real COPY at its actual call/capture operand, never an inferred target. */
typedef struct RootParameterBindingFixture {
    RootParameterFixture apply;
    XrXirTypeNode nodes[2];
    XrXirTypes types;
    XrXirInstruction caller_ops[4], pure_ops[2];
    XrXirInstruction initializer;
    XrXirBlock caller_block, pure_block, initializer_block;
    uint32_t operand;
    XrXirFunction functions[4];
    XrXirSourceModule source_module;
    XrXirFunctionIdentity identities[4];
    XrXirDeclarations declarations;
    XrXirRootValueIdentity values[2];
    XrXirEffectCallBinding binding;
    XrXirFunctionEffectContract contracts[4];
    XrXirProvenance evidence;
    XrXirModule module;
} RootParameterBindingFixture;

static inline void rp_binding_fixture(RootParameterBindingFixture *f,
    const XrXirCompileContext *context, bool capture) {
    memset(f,0,sizeof(*f)); rp_fixture(&f->apply,context);
    f->nodes[0]=f->apply.callable;f->nodes[1]=f->nodes[0];f->nodes[1].flags=2;
    f->types=(XrXirTypes){.nodes=f->nodes,.count=2};
    f->pure_ops[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7};
    f->pure_ops[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0,0}};
    f->pure_block=(XrXirBlock){.count=2};
    f->functions[2]=(XrXirFunction){.name="pure",.name_length=4,.result=XR_XIR_I64,
        .blocks=&f->pure_block,.block_count=1,.instructions=f->pure_ops,.instruction_count=2};
    f->functions[1]=f->apply.function;
    f->caller_ops[0]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)257,.immediate=2};
    f->caller_ops[1]=(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)257,.args={0,0}};
    f->caller_ops[2]=(XrXirInstruction){.op=capture?XR_XIR_FUNCTION_REF:XR_XIR_CALL,
        .type=capture?(XrXirType)256:XR_XIR_I64,.args={0,1},.immediate=1};
    f->caller_ops[3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2,0}};
    f->caller_block=(XrXirBlock){.count=4};f->operand=1;
    f->functions[0]=(XrXirFunction){.name="run",.name_length=3,
        .result=capture?(XrXirType)256:XR_XIR_I64,.blocks=&f->caller_block,.block_count=1,
        .instructions=f->caller_ops,.instruction_count=4,.operands=&f->operand,.operand_count=1};
    f->values[0]=(XrXirRootValueIdentity){0,4,(XrXirType)257};
    f->values[1]=(XrXirRootValueIdentity){1,3,(XrXirType)256};
    f->binding=(XrXirEffectCallBinding){capture?2u:1u,2,0,1};
    f->contracts[0]=(XrXirFunctionEffectContract){.values=f->values,.value_count=2,
        .bindings=&f->binding,.binding_count=1};
    f->contracts[1]=f->apply.contract;
    f->initializer=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->initializer_block=(XrXirBlock){.count=1};
    f->functions[3]=(XrXirFunction){.name="init",.name_length=4,.result=XR_XIR_UNIT,
        .blocks=&f->initializer_block,.block_count=1,.instructions=&f->initializer,.instruction_count=1};
    f->source_module=(XrXirSourceModule){"probe",5,NULL,0,3};
    f->declarations=(XrXirDeclarations){.modules=&f->source_module,.module_count=1,
        .functions=f->identities,.entry_function=capture?2u:0u};
    f->contracts[3].formula.constant_mask=4;
    f->evidence=(XrXirProvenance){.kind=1,.contracts=f->contracts,.contract_count=4};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=4,
        .declarations=&f->declarations,.types=&f->types,.linkage_kind=XR_XIR_PROGRAM,.provenance=&f->evidence};
}

#endif // XIR_ROOT_PARAMETER_FIXTURE_H
