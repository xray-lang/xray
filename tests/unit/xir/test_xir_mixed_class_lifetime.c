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
#include "xir/xxir_array.h"
#include "xir/xxir_types.h"
#include "xir/xxir_checked.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "class lifetime %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
XR_DATA const XrXirProgramSpec class_lifetime_fixture_program;
XR_DATA const uint32_t class_lifetime_entry;
XR_DATA const uint32_t class_lifetime_array_entry;
typedef struct ClassLifetimeCode { XrXirProgram *vm; XrXirCallEntry *entries; } ClassLifetimeCode;
static unsigned class_code_releases;
static const char *class_backends[] = {"vm-native", "native-vm", "vm", "native"};
static const XrXirCallEntry *class_selected[64];
static bool class_native[64], class_compile_owner_released;
static unsigned class_callbacks[2];
static uintptr_t class_metadata[3];
static XrXirAction class_lifetime_resume(XrXirCallView *view) {
    uint32_t entry=xr_xir_call_current_entry(view->activation);
    CHECK(entry<64 && class_selected[entry]);
    ++class_callbacks[class_native[entry]?1u:0u];
    return class_selected[entry]->resume(view);
}
static bool class_metadata_live(uintptr_t address) {
    for(size_t i=0;i<32768;++i) if(effects_compile_allocations[i].pointer) {
        uintptr_t base=(uintptr_t)effects_compile_allocations[i].pointer;
        if(address>=base && address-base<effects_compile_allocations[i].bytes) return true;
    }
    return false;
}
static void class_code_free(void *pointer) {
    ClassLifetimeCode *code = pointer;
    CHECK(!class_code_releases && code->vm && code->entries);
    xr_xir_compile_program_drop(code->vm); xr_compile_resources_free(code->entries);
    xr_compile_resources_free(code); ++class_code_releases;
}
static XrXirProgram *class_lifetime_program(unsigned backend) {
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    const XrCompileResourceLimits limits={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrXirArtifact *checked = NULL, *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(&context, class_lifetime_fixture_program.packet.bytes,
        class_lifetime_fixture_program.packet.length, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL, NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_compile_lower(closed, &class_lifetime_fixture_program.target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    CHECK(module->function_count == class_lifetime_fixture_program.entry_count && module->function_count<64);
    ClassLifetimeCode *code = NULL;
    CHECK(xr_compile_resources_calloc(context.resources, 1, sizeof(*code), (void **)&code) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_calloc(context.resources, module->function_count,
        sizeof(*code->entries), (void **)&code->entries) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_xir_compile_vm_program_take(&lowered, &code->vm) == XR_XIR_OK && !lowered);
    unsigned vm = 0, native = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        bool root = module->declarations->functions[f].module == module->declarations->root_module;
        bool use_native = backend == 3 || (backend < 2 && root == (backend == 1));
        CHECK(class_lifetime_fixture_program.entries[f].resume != code->vm->entries[f].resume);
        class_native[f]=use_native;
        class_selected[f]=use_native ? &class_lifetime_fixture_program.entries[f] : &code->vm->entries[f];
        code->entries[f]=*class_selected[f];code->entries[f].resume=class_lifetime_resume;
        if (use_native) ++native; else ++vm;
    }
    if (backend == 2) CHECK(vm == module->function_count && !native);
    else if (backend == 3) CHECK(native == module->function_count && !vm);
    else CHECK(vm && native);
    XrXirProgramSpec spec = class_lifetime_fixture_program;
    spec.entries = code->entries; spec.types = module->types; spec.declarations = module->declarations;
    spec.packet = class_lifetime_fixture_program.packet; spec.code = (XrXirCodeLease){code, class_code_free};
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(&context, &spec, &program) == XR_XIR_OK && program);
    class_metadata[0]=(uintptr_t)module;class_metadata[1]=(uintptr_t)code->vm;class_metadata[2]=(uintptr_t)code->entries;
    CHECK(class_metadata_live(class_metadata[0]) && class_metadata_live(class_metadata[1]) && class_metadata_live(class_metadata[2]));
    xr_compile_resources_release(context.resources);class_compile_owner_released=true;
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
static void class_lifetime_array_fields(const XrXirValue *value,XrXirValue *text) {
    CHECK(xr_xir_value_valid(value));
    CHECK(xr_xir_type_is_array(xr_xir_compile_type_arena_types(xr_xir_value_arena(value)),(XrXirType)value->type));
    /* Inline Class elements materialize a snapshot in the retained Array owner Domain. */
    const XirObject *owner=(const XirObject *)(uintptr_t)value->payload;
    XrXirValueAdmission admission={.arena=xr_xir_value_arena(value),.domain=owner->domain,
        .work=10000,.scratch_bytes=65536};
    int64_t length=-1;
    CHECK(xr_xir_array_len(value,&admission,&length)==XR_XIR_VALUE_OK && length==1);
    XrXirFaultDetail fault={0};XrXirValue object={0};
    XrXirValueStatus status=xr_xir_array_get(value,0,&admission,&object,&fault);
    if(status!=XR_XIR_VALUE_OK) fprintf(stderr,"Late Source Array<Box> get status%u type%u length%lld workLeft%llu fault%u\n",
        status,value->type,(long long)length,(unsigned long long)admission.work,fault.code);
    CHECK(status==XR_XIR_VALUE_OK);
    class_lifetime_fields(&object,text);xr_xir_value_drop(&object);
}
static void class_lifetime_round(XrXirInstance *instances[2],uint32_t entry,XrXirValue output[2]) {
    for(unsigned i=0;i<2;++i) CHECK(xr_xir_instance_start(instances[i],entry,NULL,0)==XR_XIR_CALL_READY);
    bool done[2]={false,false};unsigned polls=0;
    while(!done[0] || !done[1]) for(unsigned i=0;i<2;++i) {
        if(done[i]) continue;
        XrXirCallStatus status=xr_xir_instance_poll_bounded(instances[i],1).outcome.status;
        CHECK(++polls<4096 && (status==XR_XIR_CALL_READY || status==XR_XIR_CALL_RETURNED));
        done[i]=status==XR_XIR_CALL_RETURNED;
    }
    for(unsigned i=0;i<2;++i) CHECK(xr_xir_instance_take_result(instances[i],&output[i])==XR_XIR_CALL_RETURNED);
}
static int class_lifetime_normal_main(int argc, char **argv) {
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
    XrXirValue retained[4]={{0}},alias={0},texts[4]={{0}};
    XrXirValue arrays[4]={{0}},array_alias={0},array_texts[4]={{0}};
    for(unsigned round=0;round<2;++round) {
        class_lifetime_round(instances,class_lifetime_entry,&retained[2*round]);
        class_lifetime_round(instances,class_lifetime_array_entry,&arrays[2*round]);
    }
    CHECK(xr_xir_instance_free(instances[0]) == XR_XIR_CALL_READY && !class_code_releases);
    CHECK(xr_xir_instance_free(instances[1]) == XR_XIR_CALL_READY && class_code_releases == 1);
    CHECK(class_compile_owner_released);
    for(unsigned i=0;i<3;++i) CHECK(!class_metadata_live(class_metadata[i]));
    CHECK(runtime_live && runtime_bytes && xr_xir_value_copy(&retained[0], &alias) == XR_XIR_VALUE_OK);
    for (unsigned i = 0; i < 4; ++i) class_lifetime_fields(&retained[i], &texts[i]);
    CHECK(xr_xir_value_copy(&arrays[0],&array_alias)==XR_XIR_VALUE_OK);
    for(unsigned i=0;i<4;++i) class_lifetime_array_fields(&arrays[i],&array_texts[i]);
    for (unsigned i = 0; i < 4; ++i) {xr_xir_value_drop(&retained[i]);xr_xir_value_drop(&arrays[i]);}
    XrXirValue last = {0}; class_lifetime_fields(&alias, &last); xr_xir_value_drop(&alias);
    for (unsigned i = 0; i < 4; ++i) {
        const char *bytes = NULL; size_t length = 0;
        CHECK(xr_xir_string_view(&texts[i], &bytes, &length) && length == 10 && !memcmp(bytes, "held-class", 10));
        xr_xir_value_drop(&texts[i]);
    }
    xr_xir_value_drop(&last);
    XrXirValue array_last={0};class_lifetime_array_fields(&array_alias,&array_last);xr_xir_value_drop(&array_alias);
    for(unsigned i=0;i<4;++i) {
        const char *bytes=NULL;size_t length=0;
        CHECK(xr_xir_string_view(&array_texts[i],&bytes,&length) && length==10 && !memcmp(bytes,"held-class",10));
        xr_xir_value_drop(&array_texts[i]);
    }
    const char *array_bytes=NULL;size_t array_length=0;
    CHECK(xr_xir_string_view(&array_last,&array_bytes,&array_length) && array_length==10 && !memcmp(array_bytes,"held-class",10));
    xr_xir_value_drop(&array_last);
    CHECK(class_code_releases == 1 && !runtime_live && !runtime_bytes);
    CHECK(!effects_compile_live && !effects_compile_bytes);
    CHECK(backend<2 ? class_callbacks[0] && class_callbacks[1] :
        backend==2 ? class_callbacks[0] && !class_callbacks[1] : class_callbacks[1] && !class_callbacks[0]);
    effects_source_owners_free();
    printf("Class lifetime backend=%s two Instances two rounds, Source Class/Array/String40/held-class, VM%u native%u callbacks, compilerOwnerGone metadataGone3 codeLease1 before late alias/String reads, physical0/0\n",
        class_backends[backend],class_callbacks[0],class_callbacks[1]);
    return 0;
}

#ifndef XR_CLASS_LIFETIME_RESOURCE_MAIN
int main(int argc, char **argv) { return class_lifetime_normal_main(argc, argv); }
#endif
