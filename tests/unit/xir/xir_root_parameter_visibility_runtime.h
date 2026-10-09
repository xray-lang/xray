/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_root_parameter_visibility_runtime.h - Sealed private reference edges
 *
 * KEY CONCEPT:
 *   Exact edge authority survives producer death without exporting the target.
 */
#ifndef XIR_ROOT_PARAMETER_VISIBILITY_RUNTIME_H
#define XIR_ROOT_PARAMETER_VISIBILITY_RUNTIME_H
#include "xir/xxir_vm.h"
#include "xir/xxir_program_internal.h"
/* Match the existing program_root_permissions test: exercise the private key
 * matcher independently from public signature and capture validation. */
#include "xir/xxir_instance.c"

static uint32_t visibility_runtime_target, visibility_runtime_init;
static XrXirType visibility_runtime_type;
static unsigned visibility_runtime_no_edge, visibility_runtime_allowed;

static void visibility_key_case(XrXirInstance *instance, const char *name,
    uint32_t caller, uint32_t target, XrXirType type, uint32_t captures,
    XrXirCallStatus expected) {
    XrXirDomainStats before = xr_xir_domain_stats(instance->domain);
    XrXirCallStatus status = instance_private_reference(instance, caller, target, type, captures);
    if (status != expected) fprintf(stderr, "private reference key %s status%u expected%u\n",
        name, status, expected);
    CHECK(status == expected);
    XrXirDomainStats after = xr_xir_domain_stats(instance->domain);
    CHECK(!memcmp(&before, &after, sizeof(before)));
}

static XrXirAction visibility_static_resume(XrXirCallView *view) {
    XrXirInstance *instance = view_instance(view);
    CHECK(instance);
    uint32_t caller = xr_xir_call_current_entry(view->activation);
    XrXirValue output = {0};
    if (caller == visibility_runtime_init) {
        /* This caller has the import and a compatible signature, but no edge. */
        XrXirDomainStats before = xr_xir_domain_stats(instance->domain);
        CHECK(xr_xir_instance_function(view, visibility_runtime_type, visibility_runtime_target,
            NULL, 0, &output) == XR_XIR_CALL_BAD_ARGUMENT);
        XrXirDomainStats after = xr_xir_domain_stats(instance->domain);
        CHECK(!output.type && !output.reserved && !output.payload);
        CHECK(!memcmp(&before, &after, sizeof(before)));
        ++visibility_runtime_no_edge;
    } else if (caller == 0) {
        visibility_key_case(instance, "exact", 0, visibility_runtime_target,
            visibility_runtime_type, 0, XR_XIR_CALL_READY);
        visibility_key_case(instance, "caller-no-edge", visibility_runtime_init, visibility_runtime_target,
            visibility_runtime_type, 0, XR_XIR_CALL_BAD_ARGUMENT);
        visibility_key_case(instance, "caller-boundary", instance->program->entry_count, visibility_runtime_target,
            visibility_runtime_type, 0, XR_XIR_CALL_BAD_ARGUMENT);
        visibility_key_case(instance, "different-target", 0, 0,
            visibility_runtime_type, 0, XR_XIR_CALL_BAD_ARGUMENT);
        visibility_key_case(instance, "target-boundary", 0, instance->program->entry_count,
            visibility_runtime_type, 0, XR_XIR_CALL_BAD_ARGUMENT);
        visibility_key_case(instance, "different-type", 0, visibility_runtime_target,
            XR_XIR_I64, 0, XR_XIR_CALL_BAD_ARGUMENT);
        visibility_key_case(instance, "different-capture-count", 0, visibility_runtime_target,
            visibility_runtime_type, 1, XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(xr_xir_instance_function(view, visibility_runtime_type, visibility_runtime_target,
            NULL, 0, &output) == XR_XIR_CALL_READY);
        const XrXirFunctionBinding *binding = xr_xir_function_binding(&output);
        CHECK(binding && binding->entry == visibility_runtime_target && !binding->capture_count);
        xr_xir_value_drop(&output); ++visibility_runtime_allowed;
        return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value={XR_XIR_I64,0,37}};
    }
    return (XrXirAction){.kind=XR_XIR_ACTION_RETURN};
}

static void visibility_runtime_shape(const XrXirProgram *program) {
    const XrXirProgramPermissions *p = program->permissions;
    CHECK(p && p->function_count == program->entry_count && !p->slot_count);
    CHECK(p->entries[0].reference_begin == 0 && p->entries[0].reference_count == 1);
    const XrXirProgramFunctionRef *edge = &p->references[0];
    CHECK(edge->entry == visibility_runtime_target && edge->type == visibility_runtime_type && !edge->captures);
    for (uint32_t f = 1; f < program->entry_count; ++f) CHECK(!p->entries[f].reference_count);
    CHECK(!program->declarations->functions[visibility_runtime_target].exported);
    CHECK(program->declarations->functions[visibility_runtime_target].module == 1);
    CHECK(!p->entries[visibility_runtime_target].requires_root && !p->entries[visibility_runtime_target].unresolved);
    CHECK(p->entries[visibility_runtime_init].requires_root);
}

static void visibility_runtime_drive(XrXirProgram *program) {
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance, 0, NULL, 0) == XR_XIR_CALL_READY);
    XrXirCallStatus status = xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status;
    if (status != XR_XIR_CALL_RETURNED) fprintf(stderr,"private reference runtime status%u\n",status);
    CHECK(status == XR_XIR_CALL_RETURNED);
    XrXirValue result = {0};
    CHECK(xr_xir_instance_take_result(instance, &result) == XR_XIR_CALL_RETURNED);
    CHECK(result.type == XR_XIR_I64 && !result.reserved && result.payload == 37);
    xr_xir_value_drop(&result);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
}

static void visibility_runtime(void) {
    /* The real VM executes the same private specialization used by the key
     * probes. The second program uses static callbacks and retains no producer. */
    for (unsigned native_probe = 0; native_probe < 2; ++native_probe) {
        RootParameterMark physical = rp_mark(); XrXirCompileContext c = rp_owner(rp_caps());
        uint64_t baseline = rp_stats(&c).live_bytes;
        XrXirArtifact *checked = NULL, *lowered = NULL;
        CHECK(visibility_pipeline(&c, 1, &checked, true) == XR_XIR_OK);
        visibility_runtime_target = visibility_shape(checked, 1);
        const XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
        CHECK(xr_xir_compile_lower(checked, &target, &lowered, NULL) == XR_XIR_OK);
        xr_xir_compile_artifact_free(checked);
        const XrXirModule *m = xr_xir_compile_artifact_module(lowered);
        CHECK(m && m->function_count <= 32);
        visibility_runtime_type = m->functions[0].instructions[1].type;
        visibility_runtime_init = m->declarations->modules[0].initializer;
        XrXirProgram *program = NULL;
        if (!native_probe) {
            CHECK(xr_xir_compile_vm_program_take(&lowered, &program) == XR_XIR_OK && !lowered);
        } else {
            XrXirVmBinding bindings[32]; XrXirCallEntry entries[32];
            for (uint32_t f = 0; f < m->function_count; ++f) {
                CHECK(xr_xir_compile_vm_bind(lowered, f, &bindings[f], &entries[f]) == XR_XIR_OK);
                entries[f].resume = visibility_static_resume; entries[f].release = NULL;
                entries[f].environment = NULL; entries[f].state_bytes = 0;
            }
            XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION,target,entries,m->function_count,
                m->declarations,{0},m->types,xr_xir_compile_program_proof(lowered)};
            CHECK(xr_xir_compile_program_seal(&c, &spec, &program) == XR_XIR_OK);
            xr_xir_compile_artifact_free(lowered); lowered = NULL;
            memset(bindings, 0xa5, sizeof(bindings)); memset(entries, 0xa5, sizeof(entries));
            memset(&spec, 0xa5, sizeof(spec));
        }
        visibility_runtime_shape(program);
        visibility_runtime_drive(program);
        xr_xir_compile_program_drop(program);
        rp_owner_free(&c, baseline); rp_balanced(physical);
    }
    CHECK(visibility_runtime_no_edge == 1 && visibility_runtime_allowed == 1);
    puts("private reference VM result37; producer-dead public factory; seven named exact-key cases; compiler physical0");
}
#endif // XIR_ROOT_PARAMETER_VISIBILITY_RUNTIME_H
