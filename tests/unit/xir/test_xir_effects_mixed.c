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
XR_DATA const XrXirProgramSpec effect_source_program;
XR_DATA const uint32_t effect_source_entry;
XR_DATA const uint32_t effect_source_captured;
XR_DATA const XrXirProgramSpec method_source_program;
XR_DATA const uint32_t method_source_selected_entries[3];
typedef struct EffectMixed {
    XrXirArtifact *artifact;
    XrXirCallEntry *entries;
    XrXirVmBinding *bindings;
} EffectMixed;
static unsigned releases;
static void effect_mixed_free(void *pointer) {
    EffectMixed *owner = pointer;
    xr_xir_artifact_free(owner->artifact); xr_free(owner->entries); xr_free(owner->bindings);
    xr_free(owner); ++releases;
}
static void effect_mixed(bool callback_native, bool methods) {
    const XrXirProgramSpec *native = methods ? &method_source_program : &effect_source_program;
    XrXirArtifact *checked = NULL;
    CHECK(xr_xir_checked_read(native->proof.bytes, native->proof.length,
        NULL, &checked, NULL) == XR_XIR_OK);
    EffectMixed *owner = xr_calloc(1, sizeof(*owner)); CHECK(owner);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(checked, &target, NULL, &owner->artifact, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    const XrXirModule *module = xr_xir_artifact_module(owner->artifact);
    CHECK(module->function_count == native->entry_count);
    owner->entries = xr_calloc(module->function_count, sizeof(*owner->entries));
    owner->bindings = xr_calloc(module->function_count, sizeof(*owner->bindings));
    CHECK(owner->entries && owner->bindings);
    unsigned callbacks = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        CHECK(xr_xir_vm_bind(owner->artifact, f, &owner->bindings[f], &owner->entries[f]) == XR_XIR_OK);
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
        module->declarations, {owner, effect_mixed_free}, module->types, xr_xir_program_proof(owner->artifact)};
    XrXirProgram *program = NULL;
    CHECK(xr_xir_program_seal(&spec, (XrXirProgramBudget){33554432, 64000000}, &program) == XR_XIR_OK);
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY); XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    const int64_t expected[] = {11, 13, 36};
    for (unsigned i = 0; i < (methods ? 3u : 2u); ++i) {
        CHECK(xr_xir_instance_start(instance, methods ? method_source_selected_entries[i] : i ? effect_source_captured : effect_source_entry, NULL, 0) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll(instance).outcome.status == XR_XIR_CALL_RETURNED);
        XrXirValue value = {0};
        CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
        CHECK(value.type == XR_XIR_I64 && value.payload == (methods ? expected[i] : 7)); xr_xir_value_drop(&value);
    }
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    xr_xir_program_drop(program);
}
int main(void) {
    effect_mixed(false, false); CHECK(releases == 1);
    effect_mixed(true, false); CHECK(releases == 2);
    effect_mixed(false, true); CHECK(releases == 3);
    effect_mixed(true, true); CHECK(releases == 4);
    puts("Both method callback backend directions returned independent values 11, 13 and 36");
    puts("Both qualified callback backend directions returned the independent expected value 7");
    return 0;
}
