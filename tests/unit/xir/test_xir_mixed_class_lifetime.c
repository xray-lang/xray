/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_class_lifetime.c - Class data survives execution code retirement
 */
#include "xir/xxir_vm.h"
#include "xir/xxir_class.h"
#include "xir/xxir_checked.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "class lifetime %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
XR_DATA const XrXirProgramSpec class_lifetime_fixture_program;
XR_DATA const uint32_t class_lifetime_entry;
typedef struct ClassLifetimeCode { XrXirProgram *vm; XrXirCallEntry *entries; } ClassLifetimeCode;
static unsigned class_code_releases;
static const char *class_backends[] = {"0", "1", "vm", "native"};
static void class_code_free(void *pointer) {
    ClassLifetimeCode *code = pointer;
    xr_xir_compile_program_drop(code->vm); xr_compile_resources_free(code->entries);
    xr_compile_resources_free(code); ++class_code_releases;
}
static XrXirProgram *class_lifetime_program(unsigned backend) {
    const XrXirCompileContext context = *effects_source_owner(UINT64_C(64)*1024*1024, UINT64_C(128000000));
    XrXirArtifact *checked = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(&context, class_lifetime_fixture_program.proof.bytes,
        class_lifetime_fixture_program.proof.length, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_lower(checked, &class_lifetime_fixture_program.target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    CHECK(module->function_count == class_lifetime_fixture_program.entry_count);
    ClassLifetimeCode *code = NULL;
    CHECK(xr_compile_resources_calloc(context.resources, 1, sizeof(*code), (void **)&code) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_calloc(context.resources, module->function_count,
        sizeof(*code->entries), (void **)&code->entries) == XR_COMPILE_RESOURCE_OK);
    XrXirProgramProof proof = xr_xir_compile_program_proof(lowered);
    CHECK(xr_xir_compile_vm_program_take(&lowered, &code->vm) == XR_XIR_OK && !lowered);
    unsigned vm = 0, native = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        bool root = module->declarations->functions[f].module == module->declarations->root_module;
        bool use_native = backend == 3 || (backend < 2 && root == (backend == 1));
        CHECK(class_lifetime_fixture_program.entries[f].resume != code->vm->entries[f].resume);
        code->entries[f] = use_native ? class_lifetime_fixture_program.entries[f] : code->vm->entries[f];
        if (use_native) ++native; else ++vm;
    }
    if (backend == 2) CHECK(vm == module->function_count && !native);
    else if (backend == 3) CHECK(native == module->function_count && !vm);
    else CHECK(vm && native);
    XrXirProgramSpec spec = class_lifetime_fixture_program;
    spec.entries = code->entries; spec.types = module->types; spec.declarations = module->declarations;
    spec.proof = proof; spec.code = (XrXirCodeLease){code, class_code_free};
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(&context, &spec, &program) == XR_XIR_OK && program);
    return program;
}
static void class_lifetime_fields(const XrXirValue *value, XrXirValue *text) {
    CHECK(xr_xir_value_valid(value));
    XrXirValueAdmission admission = {.arena=xr_xir_value_arena(value), .work=10000, .scratch_bytes=65536};
    XrXirValue number = {0};
    CHECK(xr_xir_class_get(value, 0, &admission, &number) == XR_XIR_VALUE_OK);
    CHECK(number.type == XR_XIR_I64 && number.payload == 40); xr_xir_value_drop(&number);
    CHECK(xr_xir_class_get(value, 1, &admission, text) == XR_XIR_VALUE_OK);
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(text, &bytes, &length) && length == 10 && !memcmp(bytes, "held-class", 10));
}
int main(int argc, char **argv) {
    CHECK(argc == 2); unsigned backend = 0;
    while (backend < 4 && strcmp(argv[1], class_backends[backend])) ++backend;
    CHECK(backend < 4);
    XrXirProgram *program = class_lifetime_program(backend);
    XrXirInstance *instances[2] = {0};
    for (unsigned i = 0; i < 2; ++i) {
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
    XrXirValue retained[4] = {{0}}, alias = {0}, texts[4] = {{0}};
    for (unsigned round = 0; round < 2; ++round) {
        for (unsigned i = 0; i < 2; ++i) CHECK(xr_xir_instance_start(instances[i], class_lifetime_entry, NULL, 0) == XR_XIR_CALL_READY);
        bool done[2] = {false, false}; unsigned polls = 0;
        while (!done[0] || !done[1]) for (unsigned i = 0; i < 2; ++i) {
            if (done[i]) continue;
            XrXirCallStatus status = xr_xir_instance_poll_bounded(instances[i], 1).outcome.status;
            CHECK(++polls < 4096 && (status == XR_XIR_CALL_READY || status == XR_XIR_CALL_RETURNED));
            done[i] = status == XR_XIR_CALL_RETURNED;
        }
        for (unsigned i = 0; i < 2; ++i)
            CHECK(xr_xir_instance_take_result(instances[i], &retained[2*round+i]) == XR_XIR_CALL_RETURNED);
    }
    CHECK(xr_xir_instance_free(instances[0]) == XR_XIR_CALL_READY && !class_code_releases);
    CHECK(xr_xir_instance_free(instances[1]) == XR_XIR_CALL_READY && class_code_releases == 1);
    CHECK(runtime_live && runtime_bytes && xr_xir_value_copy(&retained[0], &alias) == XR_XIR_VALUE_OK);
    for (unsigned i = 0; i < 4; ++i) class_lifetime_fields(&retained[i], &texts[i]);
    for (unsigned i = 0; i < 4; ++i) xr_xir_value_drop(&retained[i]);
    XrXirValue last = {0}; class_lifetime_fields(&alias, &last); xr_xir_value_drop(&alias);
    for (unsigned i = 0; i < 4; ++i) {
        const char *bytes = NULL; size_t length = 0;
        CHECK(xr_xir_string_view(&texts[i], &bytes, &length) && length == 10 && !memcmp(bytes, "held-class", 10));
        xr_xir_value_drop(&texts[i]);
    }
    xr_xir_value_drop(&last); CHECK(class_code_releases == 1 && !runtime_live && !runtime_bytes);
    effects_source_owners_free();
    printf("Class lifetime backend=%s two Instances two rounds, class40/held-class survives all executable owners, late alias and strings readable, code lease1 before data drop, physical0/0\n", class_backends[backend]);
    return 0;
}
