/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_effects_mixed.c - Qualified callbacks across both backend directions
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
XR_DATA const XrXirProgramSpec effect_source_program;
XR_DATA const uint32_t effect_source_entry;
XR_DATA const uint32_t effect_source_captured;
XR_DATA const XrXirProgramSpec method_source_program;
XR_DATA const uint32_t method_source_selected_entries[3];
typedef struct EffectMixed {
    XrXirArtifact *artifact;
    XrXirCallEntry *entries;
    XrXirProgram *vm_program;
} EffectMixed;
static unsigned releases;
static void effect_mixed_free(void *pointer) {
    EffectMixed *owner = pointer;
    xr_xir_compile_artifact_free(owner->artifact); xr_compile_resources_free(owner->entries); xr_xir_compile_program_drop(owner->vm_program);
    xr_compile_resources_free(owner); ++releases;
}
static void effect_mixed(bool callback_native, bool methods) {
    const XrXirCompileContext context=*effects_source_owner(UINT64_C(64)*1024*1024,UINT64_C(128000000));
    const XrXirProgramSpec *native = methods ? &method_source_program : &effect_source_program;
    XrXirArtifact *checked = NULL;
    CHECK(xr_xir_compile_checked_read(&context, native->proof.bytes, native->proof.length, &checked, NULL) == XR_XIR_OK);
    EffectMixed *owner=NULL;
    CHECK(xr_compile_resources_calloc(context.resources,1,sizeof(*owner),(void **)&owner)==XR_COMPILE_RESOURCE_OK);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(checked, &target, &owner->artifact, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    const XrXirModule *module = xr_xir_compile_artifact_module(owner->artifact);
    CHECK(module->function_count == native->entry_count);
    CHECK(xr_compile_resources_calloc(context.resources,module->function_count,sizeof(*owner->entries),(void **)&owner->entries)==XR_COMPILE_RESOURCE_OK);
    XrXirProgramProof proof=xr_xir_compile_program_proof(owner->artifact);
    CHECK(xr_xir_compile_vm_program_take(&owner->artifact,&owner->vm_program)==XR_XIR_OK && !owner->artifact);
    CHECK(owner->vm_program && owner->vm_program->entry_count==module->function_count);
    CHECK(owner->entries);
    unsigned callbacks = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        /* The existing full-program pipeline binds and verifies the table once.
         * Copy actual immutable entries; their VM code lease stays in owner. */
        owner->entries[f]=owner->vm_program->entries[f];
        const XrXirFunction *function = &module->functions[f];
        bool callback = function->name_length == 4 && !memcmp(function->name, "pure", 4);
        callback |= function->name_length >= 8 && !memcmp(function->name, "$closure", 8) &&
            module->declarations->functions[f].promises == XR_XIR_FUNCTION_NO_SUSPEND;
        if (methods && module->declarations->functions[f].promises == XR_XIR_FUNCTION_NO_SUSPEND)
            callback |= (function->name_length > 4 && !memcmp(function->name, "get$", 4)) ||
                (function->name_length > 9 && !memcmp(function->name, "identity$", 9));
        if (callback) ++callbacks;
        if (callback == callback_native) owner->entries[f] = native->entries[f];
        CHECK((owner->entries[f].resume == native->entries[f].resume) == (callback == callback_native));
    }
    if (callbacks != (methods ? 4u : 5u)) {
        fprintf(stderr, "Qualified callbacks: %u\n", callbacks);
        for (uint32_t f = 0; f < module->function_count; ++f)
            fprintf(stderr, "%.*s promise=%u owner=%u\n", (int)module->functions[f].name_length,
                module->functions[f].name, module->declarations->functions[f].promises,
                module->declarations->functions[f].nominal_owner);
    }
    CHECK(callbacks == (methods ? 4u : 5u));
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, target, owner->entries, module->function_count,
        module->declarations, {owner, effect_mixed_free}, module->types, proof};
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(&context,&spec, &program) == XR_XIR_OK);
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY); XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    const int64_t expected[] = {11, 13, 36};
    for (unsigned i = 0; i < (methods ? 3u : 2u); ++i) {
        CHECK(xr_xir_instance_start(instance, methods ? method_source_selected_entries[i] : i ? effect_source_captured : effect_source_entry, NULL, 0) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
        XrXirValue value = {0};
        CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
        CHECK(value.type == XR_XIR_I64 && value.payload == (methods ? expected[i] : 7)); xr_xir_value_drop(&value);
    }
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
}
int main(void) {
    effect_mixed(false, false); CHECK(releases == 1);
    effect_mixed(true, false); CHECK(releases == 2);
    effect_mixed(false, true); CHECK(releases == 3);
    effect_mixed(true, true); CHECK(releases == 4);
    puts("Both method callback backend directions returned independent values 11, 13 and 36");
    puts("Both qualified callback backend directions returned the independent expected value 7");
    CHECK(!runtime_live && !runtime_bytes); effects_source_owners_free();
    return 0;
}
