/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_compile_program_owner_cases.h - Program transfer and typed resource failures
 */
#ifndef XIR_COMPILE_PROGRAM_OWNER_CASES_H
#define XIR_COMPILE_PROGRAM_OWNER_CASES_H
#include "xir_compile_program_fixture.h"
#include "xir_class_owned_fixture.h"
static void program_lifetime(void) {
    reset_observer();
    XrXirCompileContext context=context_new(UINT64_MAX);
    XrXirArtifact *lowered=owner_lowered(&context);
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    CHECK(program->context.resources==context.resources);
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    uint64_t compile_work=stats(&context).work;
    xr_compile_resources_release(context.resources);
    xr_xir_compile_program_drop(program);
    CHECK(xr_xir_instance_start(instance,1,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue result={0};
    CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(result.type==XR_XIR_I64 && result.payload==42);
    xr_xir_value_drop(&result);
    CHECK(xr_xir_instance_start(instance,2,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(stats(&context).work==compile_work);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    CHECK(!live && !physical);
    const char *bytes=NULL; size_t length=0;
    CHECK(xr_xir_string_view(&result,&bytes,&length));
    CHECK(length==5 && !memcmp(bytes,"A\0\xe4\xb8\xad",5));
    xr_xir_value_drop(&result);
}
static void program_take_failures(void) {
    size_t take_allocations=0;
    for (size_t failure=SIZE_MAX;;) {
        reset_observer();
        XrXirCompileContext context=context_new(UINT64_MAX);
        XrXirArtifact *lowered=owner_lowered(&context),*original=lowered;
        XrCompileResourceStats before=stats(&context);
        size_t start=attempts;
        if (failure!=SIZE_MAX) fail_at=start+failure;
        XrXirProgram *program=NULL;
        XrXirStatus status=xr_xir_compile_vm_program_take(&lowered,&program);
        if (failure==SIZE_MAX) {
            CHECK(status==XR_XIR_OK && !lowered && program);
            take_allocations=attempts-start;
        } else {
            CHECK(status==XR_XIR_OUT_OF_MEMORY && lowered==original && !program);
            CHECK(stats(&context).live_bytes==before.live_bytes);
            fail_at=SIZE_MAX;
            CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);
        }
        xr_xir_compile_program_drop(program); xr_xir_compile_artifact_free(lowered);
        xr_compile_resources_release(context.resources);
        CHECK(!live && !physical);
        if (failure==SIZE_MAX) failure=0;
        else if (++failure==take_allocations) break;
    }
    reset_observer();
    XrXirCompileContext context=context_new(UINT64_MAX);
    XrXirArtifact *lowered=owner_lowered(&context),*original=lowered;
    XrCompileResourceStats before=stats(&context);
    CHECK(xr_compile_resources_work(context.resources,UINT64_MAX-before.work)==XR_COMPILE_RESOURCE_OK);
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_BUDGET);
    CHECK(lowered==original && !program && stats(&context).live_bytes==before.live_bytes);
    xr_xir_compile_artifact_free(lowered); xr_compile_resources_release(context.resources);
    CHECK(!live && !physical);
    printf("VM take: %zu actual OOM points, exhausted work preserves input, physical zero\n",take_allocations);
}
static void program_abi_and_owner(void) {
    reset_observer();
    XrXirCompileContext context=context_new(UINT64_MAX);
    XrXirProgram *out=NULL;
    XrXirProgramSpec poison={0};
    poison.abi_version=26;
    poison.entries=(const XrXirCallEntry *)(uintptr_t)1;
    poison.entry_count=1;
    poison.declarations=(const XrXirDeclarations *)(uintptr_t)1;
    CHECK(xr_xir_compile_program_seal(&context,&poison,&out)==XR_XIR_BAD_LAYOUT && !out);
    CHECK(attempts==1);
    XrXirArtifact *lowered=owner_lowered(&context);
    XrXirVmBinding bindings[3]; XrXirCallEntry entries[3];
    for (unsigned i=0;i<3;++i)
        CHECK(xr_xir_compile_vm_bind(lowered,i,bindings+i,entries+i)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,*xr_xir_compile_artifact_target(lowered),
        entries,3,module->declarations,{0},module->types,xr_xir_compile_program_proof(lowered)};
    entries[0].abi_version=21;
    spec.declarations=(const XrXirDeclarations *)(uintptr_t)1;
    CHECK(xr_xir_compile_program_seal(&context,&spec,&out)==XR_XIR_BAD_LAYOUT && !out);
    entries[0].abi_version=XR_XIR_CALL_ABI_VERSION; spec.declarations=module->declarations;
    XrXirCompileContext foreign=context_new(UINT64_MAX);
    CHECK(xr_xir_compile_program_match(&foreign,&spec,spec.proof.layouts,lowered)==XR_XIR_BAD_STRUCTURE);
    xr_compile_resources_release(foreign.resources);
    XrXirVmBinding binding={lowered,99},saved_binding=binding;
    XrXirCallEntry entry=entries[0],saved_entry=entry;
    CHECK(xr_xir_compile_vm_bind(lowered,99,&binding,&entry)==XR_XIR_BAD_STRUCTURE);
    CHECK(!memcmp(&binding,&saved_binding,sizeof(binding)) && !memcmp(&entry,&saved_entry,sizeof(entry)));
    XrXirProgram *untouched=(XrXirProgram *)(uintptr_t)1;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&untouched)==XR_XIR_BAD_STRUCTURE && lowered);
    CHECK(untouched==(XrXirProgram *)(uintptr_t)1);
    xr_xir_compile_artifact_free(lowered); xr_compile_resources_release(context.resources);
    CHECK(!live && !physical);
}

static XrXirArtifact *program_method_lowered(const XrXirCompileContext *context,
    uint32_t method_kind) {
    ClassOwnedFixture fixture;
    class_owned_fixture(&fixture);
    fixture.identities[2].method_kind = method_kind;
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    XrXirArtifact *checked = NULL, *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_check(context, &fixture.module, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    xr_xir_compile_artifact_free(closed);
    return lowered;
}
static void program_method_result(XrXirProgram *program, uint32_t entry) {
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance, entry, NULL, 0) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
    XrXirValue value = {0};
    CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
    CHECK(value.type == XR_XIR_I64 && value.payload == 41);
    xr_xir_value_drop(&value);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
}
static void program_method_kind_match(void) {
    reset_observer();
    XrXirCompileContext context = context_new(UINT64_MAX);
    const uint32_t roles[] = {XR_XIR_READ_METHOD, XR_XIR_MEMBER_HELPER};
    XrXirArtifact *lowered[2] = {NULL, NULL};
    XrXirVmBinding bindings[2][5];
    XrXirCallEntry entries[2][5];
    XrXirProgramSpec specs[2];
    for (uint32_t side = 0; side < 2; ++side) {
        lowered[side] = program_method_lowered(&context, roles[side]);
        const XrXirModule *module = xr_xir_compile_artifact_module(lowered[side]);
        CHECK(module->function_count == 5 && module->provenance);
        CHECK(module->declarations->functions[2].method_kind == roles[side]);
        for (uint32_t f = 0; f < 5; ++f)
            CHECK(xr_xir_compile_vm_bind(lowered[side], f,
                &bindings[side][f], &entries[side][f]) == XR_XIR_OK);
        specs[side] = (XrXirProgramSpec) {XR_XIR_PROGRAM_ABI_VERSION,
            *xr_xir_compile_artifact_target(lowered[side]), entries[side], 5,
            module->declarations, {0}, module->types,
            xr_xir_compile_program_proof(lowered[side])};
        CHECK(xr_xir_compile_program_match(&context, &specs[side],
            specs[side].proof.layouts, lowered[side]) == XR_XIR_OK);
        XrXirProgram *program = NULL;
        CHECK(xr_xir_compile_program_seal(&context, &specs[side], &program) == XR_XIR_OK);
        program_method_result(program, module->declarations->entry_function);
        xr_xir_compile_program_drop(program);
    }
    for (uint32_t side = 0; side < 2; ++side) {
        XrXirFunctionIdentity identities[5];
        memcpy(identities, specs[side].declarations->functions, sizeof(identities));
        identities[2].method_kind = roles[1 - side];
        XrXirDeclarations declarations = *specs[side].declarations;
        declarations.functions = identities;
        XrXirProgramSpec altered = specs[side];
        altered.declarations = &declarations;
        /* The other independently verified proof admits this exact descriptor.
         * A legal member role must still match the selected proof's authority. */
        altered.proof = specs[1 - side].proof;
        XrXirProgram *program = NULL;
        CHECK(xr_xir_compile_program_seal(&context, &altered, &program) == XR_XIR_OK);
        program_method_result(program, declarations.entry_function);
        xr_xir_compile_program_drop(program);
        altered.proof = specs[side].proof;
        XrCompileResourceStats before = stats(&context);
        XrXirStatus matched = xr_xir_compile_program_match(&context, &altered,
            altered.proof.layouts, lowered[side]);
        XrXirStatus proof = xr_xir_compile_program_proof_verify(&context, &altered,
            &altered.proof);
        program = NULL;
        XrXirStatus sealed = xr_xir_compile_program_seal(&context, &altered, &program);
        bool empty = !program;
        if (matched != XR_XIR_BAD_STRUCTURE || proof != XR_XIR_BAD_STRUCTURE ||
            sealed != XR_XIR_BAD_STRUCTURE || !empty)
            fprintf(stderr, "method role %u -> %u: match=%u proof=%u seal=%u output=%u\n",
                roles[side], roles[1 - side], (unsigned)matched, (unsigned)proof,
                (unsigned)sealed, empty ? 0u : 1u);
        xr_xir_compile_program_drop(program);
        CHECK(matched == XR_XIR_BAD_STRUCTURE && proof == XR_XIR_BAD_STRUCTURE &&
            sealed == XR_XIR_BAD_STRUCTURE && empty);
        CHECK(stats(&context).live_bytes == before.live_bytes);
        CHECK(xr_xir_compile_artifact_verify(lowered[side], NULL) == XR_XIR_OK);
    }
    xr_xir_compile_artifact_free(lowered[0]);
    xr_xir_compile_artifact_free(lowered[1]);
    xr_compile_resources_release(context.resources);
    CHECK(!live && !physical);
    puts("Program method roles: two valid proofs return 41; mismatched descriptors rejected both ways");
}
#endif
