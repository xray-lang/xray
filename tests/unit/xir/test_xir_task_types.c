/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_task_types.c - Authentic Task obligations and an independent Checked packet
 */
#include "xir/xxir.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_types.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_constraint_proof.h"
#include "xir/xxir_compile_memory.h"
#include "xir/xxir_interface_members.h"
#include "xir/xxir_type_inference.h"
#include "xir/xxir_internal.h"
#include "tuple_owner_observer.h"
#include "xir_task_types66_golden.h"
#include "xir_task_go66_golden.h"
#include "xir_atomic_types65_golden.h"
#include "xir/xxir_effect_terms.inc.c"
#include "task_outcome_sendable_cases.h"
#include "task_go_checked_cases.h"
typedef struct TaskTypesFixture {
    XrXirTypeNode nodes[2]; XrXirTypes types; XrXirType parameters[2];
    XrXirInstruction instruction; XrXirBlock block; XrXirFunction function; XrXirModule module;
} TaskTypesFixture;
static void task_types_fixture(TaskTypesFixture *f) {
    *f = (TaskTypesFixture){0};
    f->nodes[0] = (XrXirTypeNode){.kind = XR_XIR_TYPE_TASK, .element = XR_XIR_I64};
    f->nodes[1] = (XrXirTypeNode){.kind = XR_XIR_TYPE_TASK, .element = XR_XIR_STRING};
    f->types = (XrXirTypes){f->nodes, 2, NULL, NULL};
    f->parameters[0] = (XrXirType)256; f->parameters[1] = (XrXirType)257;
    f->instruction = (XrXirInstruction){.op = XR_XIR_RETURN};
    f->block = (XrXirBlock){.count = 1};
    f->function = (XrXirFunction){.name = "taskTypes", .name_length = 9, .parameters = f->parameters,
        .parameter_count = 2, .blocks = &f->block, .block_count = 1,
        .instructions = &f->instruction, .instruction_count = 1};
    f->module = (XrXirModule){.stage = XR_XIR_BUILT, .functions = &f->function, .function_count = 1, .types = &f->types};
}
static XrXirStatus task_types_pipeline(const XrXirCompileContext *c) {
    TaskTypesFixture f; task_types_fixture(&f);
    XrXirArtifact *checked = NULL, *decoded = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0}; XrXirTypes *clone = NULL;
    XrXirStatus status = xr_xir_compile_check(c, &f.module, &checked, NULL);
    if (status == XR_XIR_OK) status = xr_xir_compile_checked_write(checked, &packet, NULL);
    if (status == XR_XIR_OK) {
        if (packet.length != sizeof(task_types66_golden)) fprintf(stderr, "packet length actual%zu expected%zu\n", packet.length, sizeof(task_types66_golden));
        for (size_t i = 64; i < packet.length && i < sizeof(task_types66_golden); ++i)
            if (packet.bytes[i] != task_types66_golden[i]) {
                fprintf(stderr, "packet first body mismatch offset%zu actual%u expected%u\n", i, packet.bytes[i], task_types66_golden[i]); break;
            }
        CHECK(packet.length == sizeof(task_types66_golden) && !memcmp(packet.bytes, task_types66_golden, packet.length));
    }
    if (status == XR_XIR_OK) status = xr_xir_compile_checked_read(c, task_types66_golden, sizeof(task_types66_golden), &decoded, NULL);
    if (status == XR_XIR_OK) status = xr_xir_compile_types_clone(c, xr_xir_compile_artifact_module(decoded)->types, &clone);
    if (status == XR_XIR_OK) {
        CHECK(clone->nodes != f.nodes && clone->nodes[0].kind == XR_XIR_TYPE_TASK &&
            clone->nodes[0].element == XR_XIR_I64 && clone->nodes[1].element == XR_XIR_STRING);
        memset(f.nodes, 0xcc, sizeof(f.nodes)); memset(f.parameters, 0xcc, sizeof(f.parameters));
        status = xr_xir_compile_artifact_verify(checked, NULL);
        if (status == XR_XIR_OK) status = xr_xir_compile_artifact_verify(decoded, NULL);
        if (status == XR_XIR_OK) {
            status = xr_xir_compile_lower(decoded,
                &(XrXirTarget){XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}, &lowered, NULL);
            if (status == XR_XIR_OK) {
                CHECK(lowered && xr_xir_compile_artifact_module(lowered)->stage == XR_XIR_LOWERED);
                status = xr_xir_compile_artifact_verify(lowered, NULL);
            } else CHECK(!lowered && (status == XR_XIR_OUT_OF_MEMORY || status == XR_XIR_BUDGET));
        }
    }
    xr_xir_compile_types_free(clone); xr_xir_compile_artifact_free(lowered);
    xr_xir_compile_artifact_free(decoded); xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_artifact_free(checked); return status;
}
static void task_types_reject(void) {
    XrXirCompileContext c = owner_new(caps()); uint64_t baseline = stats(&c).live_bytes;
    XrXirArtifact *out = NULL; size_t before = attempts;
    CHECK(xr_xir_compile_checked_read(&c, atomic_types65_golden, sizeof(atomic_types65_golden), &out, NULL) == XR_XIR_BAD_STRUCTURE && !out);
    out = (XrXirArtifact *)(uintptr_t)1;
    CHECK(xr_xir_compile_checked_read(&c, atomic_types65_golden, sizeof(atomic_types65_golden), &out, NULL) == XR_XIR_BAD_STRUCTURE &&
        out == (XrXirArtifact *)(uintptr_t)1 && attempts == before);
    for (uint32_t mutation = 0; mutation < 12; ++mutation) {
        TaskTypesFixture f; task_types_fixture(&f); XrXirCallableParameter parameter = {XR_XIR_I64, 0};
        switch (mutation) {
        case 0: f.nodes[0].element = XR_XIR_UNIT; break;
        case 1: f.nodes[0].element = (XrXirType)256; break;
        case 2: f.nodes[0].element = (XrXirType)257; break;
        case 3: f.nodes[0].element = (XrXirType)255; break;
        case 4: f.nodes[0].parameter_span = 1; break;
        case 5: f.nodes[0].result = XR_XIR_I64; break;
        case 6: f.nodes[0].flags = 1; break;
        case 7: f.nodes[0].parameters = &parameter; f.nodes[0].parameter_count = 1; break;
        case 8: f.nodes[0].nominal.declaration = 1; break;
        case 9: f.nodes[0].nominal.arguments = &parameter.type; f.nodes[0].nominal.argument_count = 1; break;
        case 10: f.nodes[0].nominal.fields = &parameter.type; f.nodes[0].nominal.field_count = 1; break;
        default: f.nodes[0].kind = 9; break;
        }
        out = NULL;
        CHECK(xr_xir_compile_check(&c, &f.module, &out, NULL) ==
            (mutation >= 5 && mutation <= 10 ? XR_XIR_BAD_STRUCTURE : XR_XIR_BAD_TYPE) && !out);
        CHECK(stats(&c).live_bytes == baseline);
    }
    owner_free(&c, baseline);
}
static void task_definition(void) {
    XrXirCompileContext c = owner_new(caps()); uint64_t baseline = stats(&c).live_bytes;
    XrXirType formal = (XrXirType)256, actual = (XrXirType)257, argument = XR_XIR_I64;
    XrXirTypeNode nodes[2] = {{.kind = XR_XIR_TYPE_TASK, .element = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE, .parameter_span = 1},
        {.kind = XR_XIR_TYPE_TASK, .element = XR_XIR_I64}};
    XrXirTypes types = {nodes, 2, NULL, NULL};
    XrXirConstraint constraint = {.markers = XR_XIR_CONSTRAINT_SENDABLE};
    XrXirGeneric generics[2] = {{.arguments = &argument, .argument_count = 1}, {.constraints = &constraint, .parameter_count = 1}};
    uint32_t operand = 0;
    XrXirInstruction root[2] = {{.op = XR_XIR_CALL, .type = actual, .args = {0, 1}, .immediate = 1, .type_arguments = {0, 1}},
        {.op = XR_XIR_RETURN, .args = {1}}};
    XrXirInstruction body = {.op = XR_XIR_RETURN};
    XrXirBlock blocks[2] = {{.count = 2}, {.count = 1}};
    XrXirFunction functions[2] = {{.name = "root", .name_length = 4, .parameters = &actual, .parameter_count = 1,
        .result = actual, .blocks = blocks, .block_count = 1, .instructions = root, .instruction_count = 2,
        .operands = &operand, .operand_count = 1}, {.name = "id", .name_length = 2, .parameters = &formal,
        .parameter_count = 1, .result = formal, .blocks = blocks + 1, .block_count = 1, .instructions = &body, .instruction_count = 1}};
    XrXirModule module = {.stage = XR_XIR_BUILT, .functions = functions, .function_count = 2, .types = &types, .generics = generics};
    XrXirArtifact *checked = NULL, *closed = NULL;
    CHECK(xr_xir_compile_check(&c, &module, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_OK);
    const XrXirModule *m = xr_xir_compile_artifact_module(closed);
    CHECK(!m->generics && m->stage == XR_XIR_CHECKED && m->types->count == 1 &&
        xr_xir_task_element(m->types, m->functions[1].result) == XR_XIR_I64);
    CHECK(xr_xir_compile_artifact_verify(closed, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_type_substitution_matches(&c, &types, &argument, 1, formal, actual) == XR_XIR_OK);
    argument = XR_XIR_STRING;
    CHECK(xr_xir_compile_type_substitution_matches(&c, &types, &argument, 1, formal, actual) == XR_XIR_BAD_TYPE);
    argument = XR_XIR_I64; constraint.markers = 0;
    XrXirArtifact *missing = NULL;
    CHECK(xr_xir_compile_check(&c, &module, &missing, NULL) == XR_XIR_BAD_TYPE && !missing);
    xr_xir_compile_artifact_free(closed); xr_xir_compile_artifact_free(checked); owner_free(&c, baseline);
}
static void task_dag_constraints(void) {
    XrXirCompileContext c = owner_new(caps()); uint64_t baseline = stats(&c).live_bytes;
    XrXirCallableParameter fields[3] = {{XR_XIR_UNIT, 0}, {(XrXirType)XR_XIR_TYPE_PARAMETER_BASE, 0}, {XR_XIR_STRING, 0}};
    XrXirTypeNode nodes[3] = {{.kind = XR_XIR_TYPE_TUPLE, .parameters = fields, .parameter_count = 3, .parameter_span = 1},
        {.kind = XR_XIR_TYPE_ARRAY, .element = (XrXirType)256, .parameter_span = 1},
        {.kind = XR_XIR_TYPE_TASK, .element = (XrXirType)257, .parameter_span = 1}};
    XrXirTypes types = {nodes, 3, NULL, NULL};
    XrXirConstraint bound = {.markers = XR_XIR_CONSTRAINT_SENDABLE};
    XrXirGeneric generic = {.constraints = &bound, .parameter_count = 1};
    XrXirFunction function = {0}; XrXirModule module = {.functions = &function, .function_count = 1, .types = &types, .generics = &generic};
    XrXirProofContext proof = {&module, {XR_XIR_CONTEXT_FUNCTION, 0, 0}};
    CHECK(xr_xir_compile_types_structure_verify(&c, &types) == XR_XIR_OK);
    CHECK(xr_xir_compile_type_expression_shape(&c, &types, (XrXirType)258, 0) == XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_type_expression_shape(&c, &types, (XrXirType)258, 1) == XR_XIR_OK);
    CHECK(xr_xir_compile_type_markers_prove(&c, &proof, (XrXirType)258, XR_XIR_CONSTRAINT_SENDABLE) == XR_XIR_OK);
    CHECK(xr_xir_compile_type_markers_prove(&c, &proof, (XrXirType)258, XR_XIR_CONSTRAINT_EQUAL) == XR_XIR_BAD_TYPE);
    bound.markers = 0;
    CHECK(xr_xir_compile_type_markers_prove(&c, &proof, (XrXirType)258, XR_XIR_CONSTRAINT_SENDABLE) == XR_XIR_BAD_TYPE);
    bound.markers = XR_XIR_CONSTRAINT_SENDABLE; fields[2].type = XR_XIR_ERROR;
    CHECK(xr_xir_compile_type_markers_prove(&c, &proof, (XrXirType)258, XR_XIR_CONSTRAINT_SENDABLE) == XR_XIR_BAD_TYPE);
    owner_free(&c, baseline);
}
static XrXirStatus task_substitution_paths(const XrXirCompileContext *c) {
    XrXirTypeNode nodes[2] = {{.kind = XR_XIR_TYPE_TASK, .element = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE, .parameter_span = 1},
        {.kind = XR_XIR_TYPE_TASK, .element = XR_XIR_I64}};
    XrXirTypes types = {nodes, 2, NULL, NULL}; XrXirType argument = XR_XIR_I64, result = XR_XIR_UNIT;
    XrXirGeneric substitution = {.arguments = &argument, .argument_count = 1}; XrXirCompileContext state = *c;
    EffectTerms pool = {.types = types, .remaining = &state, .capacity = 2};
    XrXirStatus status = effect_terms_substitute(&pool, (XrXirType)256, &substitution, &result);
    if (status == XR_XIR_OK) {
        CHECK(xr_xir_task_element(&pool.types, result) == XR_XIR_I64 && !xr_xir_type_span(&pool.types, result));
        status = xr_xir_compile_type_substitution_matches(c, &pool.types, &argument, 1, (XrXirType)256, result);
    }
    effect_terms_free(&pool);
    XrXirInferenceState *inference = NULL; XrXirType inferred = XR_XIR_UNIT;
    XrXirInferenceRequest request = {.types = &types, .own_count = 1};
    if (status == XR_XIR_OK) status = xr_xir_compile_inference_begin(c, &request, &inference);
    if (status == XR_XIR_OK) status = xr_xir_compile_inference_observe(inference, &types, (XrXirInferencePair){(XrXirType)256, (XrXirType)257});
    if (status == XR_XIR_OK) status = xr_xir_compile_inference_finalize(inference, &types, &inferred, 1);
    if (status == XR_XIR_OK) CHECK(inferred == XR_XIR_I64);
    xr_xir_compile_inference_dispose(inference); return status;
}
static XrXirStatus task_interface_paths(const XrXirCompileContext *c) {
    XrXirCallableParameter parameter = {(XrXirType)256, 0};
    XrXirTypeNode nodes[2] = {{.kind = XR_XIR_TYPE_TASK, .element = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE, .parameter_span = 1},
        {.kind = XR_XIR_TYPE_CALLABLE, .parameters = &parameter, .parameter_count = 1, .result = (XrXirType)256, .parameter_span = 1}};
    XrXirConstraint bound = {.markers = XR_XIR_CONSTRAINT_SENDABLE};
    XrXirInterfaceMethod method = {.name = {"inspect", 7}, .signature = (XrXirType)257};
    XrXirInterfaceDeclaration declaration = {.module = {"m", 1}, .name = {"Inspect", 7}, .parameter_count = 1,
        .constraints = &bound, .methods = &method, .method_count = 1};
    XrXirInterfaceTable table = {&declaration, 1}; XrXirTypes types = {nodes, 2, NULL, &table};
    XrXirType formal = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE, actual = XR_XIR_I64;
    XrXirInterfaceApplication app = {0, &formal, 1};
    XrXirInterfaceClosureRequest request = {&types, &types, &app, 1, &actual, 1, 0};
    XrXirInterfaceClosure *closure = NULL; XrXirTypes *owned = NULL;
    XrXirStatus status = xr_xir_compile_types_structure_verify(c, &types);
    if (status == XR_XIR_OK) status = xr_xir_compile_interface_closure_substitute(c, &request, &closure);
    XrXirType result = XR_XIR_UNIT;
    if (status == XR_XIR_OK) {
        const XrXirInterfaceRequirement *requirement = xr_xir_interface_closure_requirement(closure, 0);
        const XrXirTypes *view = xr_xir_interface_closure_types(closure);
        const XrXirTypeNode *signature = xr_xir_callable_signature(view, requirement->signature);
        CHECK(signature && xr_xir_task_element(view, signature->result) == XR_XIR_I64);
        result = signature->result;
        status = xr_xir_compile_type_substitution_matches_between(c, &types, view, &actual, 1, (XrXirType)256, result);
        if (status == XR_XIR_OK) status = xr_xir_compile_types_clone(c, view, &owned);
    }
    xr_xir_compile_interface_closure_free(closure);
    if (status == XR_XIR_OK) {
        memset(nodes, 0xcc, sizeof(nodes)); memset(&parameter, 0xcc, sizeof(parameter));
        status = xr_xir_compile_types_structure_verify(c, owned);
        if (status == XR_XIR_OK) CHECK(xr_xir_task_element(owned, result) == XR_XIR_I64);
    }
    xr_xir_compile_types_free(owned); return status;
}
static void task_types_faults(void) {
    size_t sites = 0;
    for (size_t ordinal = 0; ordinal <= sites; ++ordinal) {
        attempts = 0; fail_at = ordinal ? ordinal - 1 : SIZE_MAX; injected = false;
        XrCompileResourceLimits limits = caps(); XrXirCompileContext c = {0};
        XrCompileResourceStatus acquired = xr_compile_resources_new(&limits, &c.resources);
        XrXirStatus status = acquired == XR_COMPILE_RESOURCE_OK ? XR_XIR_OK : XR_XIR_OUT_OF_MEMORY;
        uint64_t baseline = 0;
        if (status == XR_XIR_OK) {
            c.limits = xr_xir_compile_default_limits(); baseline = stats(&c).live_bytes; status = task_types_pipeline(&c);
            if (status == XR_XIR_OK) status = task_substitution_paths(&c);
            if (status == XR_XIR_OK) status = task_interface_paths(&c);
            if (status == XR_XIR_OK) status = task_error_proof(&c);
            if (status == XR_XIR_OK) status = task_error_effects(&c);
            if (status == XR_XIR_OK) status = task_go_pipeline(&c);
            if (status == XR_XIR_OK) status = task_go_cleanup_pipeline(&c);
        }
        if (c.resources) owner_free(&c, baseline);
        if (!ordinal) { CHECK(status == XR_XIR_OK); sites = attempts; CHECK(sites); }
        else CHECK(injected && status == XR_XIR_OUT_OF_MEMORY);
        CHECK(!live && !live_bytes);
    }
    fail_at = SIZE_MAX;
    printf("Task Checked66 independent current KAT, old65 zeroalloc reject, actual compiler OOM ordinals=%zu physical0\n", sites);
}
int main(void) {
    task_types_reject(); task_definition(); task_dag_constraints(); task_error_reject();
    task_error_axes(); task_go_reject(); task_go_slots(); task_go_definition();
    task_go_outcomes(); task_go_caught(); task_go_cleanup(); task_go_visibility();
    task_go_phi(); task_go_packets(); task_go_cleanup_permissions(); task_go_axes(); task_types_faults(); return 0;
}
