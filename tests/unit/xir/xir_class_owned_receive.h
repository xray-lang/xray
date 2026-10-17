/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_owned_receive.h - Owned class code and failure-atomic reception
 */
#ifndef XIR_CLASS_OWNED_RECEIVE_H
#define XIR_CLASS_OWNED_RECEIVE_H
#include "xir_mixed_binding_projection.h"
typedef struct ClassMixedCode {
    XrXirCallEntry *entries;
    XrXirVmBinding *bindings;
} ClassMixedCode;
typedef struct ClassOwnedFixture {
    const XrXirProgramSpec *definition;
} ClassOwnedFixture;
static unsigned class_code_releases;

static void class_code_free(ClassMixedCode *code) {
    if (!code) return;
    xr_compile_resources_free(code->entries);
    xr_compile_resources_free(code->bindings);
    xr_compile_resources_free(code);
}
static void class_code_release(void *pointer) {
    class_code_free(pointer);
    ++class_code_releases;
}

static XrXirStatus class_owned_retry(const XrXirCompileContext *context,
    XrXirProgramSpec *spec,ClassMixedCode *code,XrXirProgramInput **input,
    XrXirProgram **output) {
    unsigned char saved_code[sizeof(*code)];
    unsigned char saved_entries[32*sizeof(*code->entries)];
    unsigned char saved_bindings[32*sizeof(*code->bindings)];
    size_t entry_bytes=spec->entry_count*sizeof(*code->entries);
    size_t binding_bytes=spec->entry_count*sizeof(*code->bindings);
    XrXirProgramInput *retained=*input;
    XrXirArtifact *artifact=retained->artifact;
    unsigned releases=class_code_releases;
    C(spec->entry_count<=32 && artifact && !*output);
    memcpy(saved_code,code,sizeof(*code));
    memcpy(saved_entries,code->entries,entry_bytes);
    memcpy(saved_bindings,code->bindings,binding_bytes);
    XrCompileResourceStats before=class_pipeline_stats(context);
    spec->abi_version=0;
    C(xir_program_receive(context,spec,input,NULL,output)==XR_XIR_BAD_LAYOUT);
    C(*input==retained && retained->artifact==artifact && !*output && class_code_releases==releases);
    C(!memcmp(saved_code,code,sizeof(*code)) && !memcmp(saved_entries,code->entries,entry_bytes));
    C(!memcmp(saved_bindings,code->bindings,binding_bytes));
    XrCompileResourceStats after=class_pipeline_stats(context);
    C(after.live_bytes==before.live_bytes && after.work>=before.work);
    spec->abi_version=XR_XIR_PROGRAM_ABI_VERSION;
    return xir_program_receive(context,spec,input,NULL,output);
}

static XrXirStatus class_owned_program(const XrXirCompileContext *context,
    const XrXirProgramSpec *definition,bool retry,XrXirProgram **output) {
    if (!output || *output) return XR_XIR_BAD_STRUCTURE;
    C(definition->entry_count<=32 && !*output);
    XrXirArtifact *checked=NULL,*lowered=NULL;
    XrXirProgramInput *input=NULL;
    XrXirProgram *received=NULL;
    XirMixedProjection projection;
    ClassMixedCode *code=NULL;
    XrCompileResourceStatus made=xr_compile_resources_calloc(context->resources,1,sizeof(*code),(void **)&code);
    XrXirStatus status=made==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:
        made==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET;
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_read(context,definition->packet.bytes,
        definition->packet.length,&checked,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_lower(checked,&definition->target,&lowered,NULL);
    xr_xir_compile_artifact_free(checked);
    const XrXirModule *module=status==XR_XIR_OK?xr_xir_compile_artifact_module(lowered):NULL;
    if (status==XR_XIR_OK && module->function_count!=definition->entry_count) status=XR_XIR_BAD_STRUCTURE;
    if (status==XR_XIR_OK) status=xr_xir_compile_vm_bind_table(lowered,&code->bindings,&code->entries);
    if (status==XR_XIR_OK) status=xir_mixed_projection_prepare(context,lowered,code->bindings,
        code->entries,definition->entry_count,&projection);
    if (status==XR_XIR_OK) {
        unsigned native=0,vm=0;
        for (uint32_t f=0;status==XR_XIR_OK && f<definition->entry_count;++f) {
            C(code->bindings[f].artifact==lowered && code->bindings[f].function==f);
            C(code->entries[f].environment==&code->bindings[f]);
            if (f%2) { status=xir_mixed_projection_native(context,&projection,f,&definition->entries[f]);
                if (status==XR_XIR_OK) { code->entries[f]=definition->entries[f];++native; } }
            else { C(code->entries[f].resume!=definition->entries[f].resume);++vm; }
        }
        if (status!=XR_XIR_OK) goto finish;
        C(native && vm);
        XrXirProgramSpec spec=*definition;
        spec.entries=code->entries;spec.declarations=module->declarations;spec.types=module->types;
        spec.packet=(XrXirProgramPacket){0};spec.code=(XrXirCodeLease){0};
        status=xir_program_input_take(context,&lowered,&input);
        if (status==XR_XIR_OK) status=retry?class_owned_retry(context,&spec,code,&input,&received):
            xir_program_receive(context,&spec,&input,NULL,&received);
        if (status==XR_XIR_OK) status=xir_mixed_projection_match(context,received,&projection);
        if (status==XR_XIR_OK) { C(!input && !lowered && received);
            received->code=(XrXirCodeLease){code,class_code_release};*output=received;received=NULL;code=NULL; }
    }
finish:
    xr_xir_compile_program_drop(received);
    xir_program_input_free(input);
    xr_xir_compile_artifact_free(lowered);
    class_code_free(code);
    if (status!=XR_XIR_OK) C(!*output);
    return status;
}

static XrXirStatus class_owned_operation(const XrXirCompileContext *context,void *pointer) {
    const ClassOwnedFixture *fixture=pointer;
    XrXirProgram *program=NULL;
    unsigned releases=class_code_releases;
    XrXirStatus status=class_owned_program(context,fixture->definition,false,&program);
    xr_xir_compile_program_drop(program);
    C(class_code_releases==releases+(status==XR_XIR_OK));
    C(!runtime_live && !runtime_bytes);
    return status;
}
#endif // XIR_CLASS_OWNED_RECEIVE_H
