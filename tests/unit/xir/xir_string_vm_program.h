/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_string_vm_program.h - Full VM binding tables retained by an immutable Program
 *
 * KEY CONCEPT:
 *   The Program owns its Input while the code lease retains binding environments.
 */
#ifndef XIR_STRING_VM_PROGRAM_H
#define XIR_STRING_VM_PROGRAM_H
#include "xir/xxir_program_internal.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_program.h"
#include "xir/xxir_compile_memory.h"
#include "xir_mixed_binding_projection.h"
typedef struct StringVmCode {
    XrXirVmBinding *bindings;
} StringVmCode;
static void string_vm_code_release(void *pointer) {
    StringVmCode *code=pointer;
    if (!code) return;
    xr_compile_resources_free(code->bindings);
    xr_compile_resources_free(code);
}
static XrXirStatus string_vm_program_take(XrXirArtifact **artifact,
    const XrXirCallEntry *native_child, XrXirProgram **output) {
    CHECK(artifact && *artifact && output && !*output);
    XrXirCompileContext current=*xr_xir_compile_artifact_context(*artifact);
    const XrXirCompileContext *context=&current;
    XrXirArtifact *original=*artifact;
    const XrXirModule *module=xr_xir_compile_artifact_module(*artifact);
    CHECK(module && module->function_count==5 && module->declarations &&
        module->declarations->functions[0].exported && module->declarations->functions[2].exported);
    XrXirStatus status=XR_XIR_OK;
    StringVmCode *code=xir_compile_calloc(context,1,sizeof(*code),&status);
    if (!code) return status;
    XrXirCallEntry *entries=NULL;
    XrXirProgramInput *input=NULL;
    XrXirProgram *program=NULL;
    XirMixedProjection projection;
    status=xr_xir_compile_vm_bind_table(*artifact,&code->bindings,&entries);
    if (status==XR_XIR_OK) status=xir_mixed_projection_prepare(context,*artifact,
        code->bindings,entries,5,&projection);
    if (status==XR_XIR_OK && native_child) {
        status=xir_mixed_projection_native(context,&projection,1,native_child);
        if (status==XR_XIR_OK) entries[1]=*native_child;
    }
    if (status==XR_XIR_OK) {
        XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,*xr_xir_compile_artifact_target(*artifact),
            entries,5,module->declarations,{0},module->types,
            {0}};
        XrXirArtifact *owned=*artifact;
        status=xir_program_input_take(context,&owned,&input);
        if (status==XR_XIR_OK) status=xir_program_receive(context,&spec,&input,NULL,&program);
    }
    xr_compile_resources_free(entries);
    if (status==XR_XIR_OK && (*output || *artifact!=original)) status=XR_XIR_BAD_STRUCTURE;
    if (status==XR_XIR_OK) status=xir_program_input_verify(context,program->permissions->input);
    if (status==XR_XIR_OK && (program->permissions->input->artifact!=original ||
        original->program_final!=program->permissions->effects ||
        original->layouts!=program->permissions->layouts)) status=XR_XIR_BAD_STRUCTURE;
    if (status==XR_XIR_OK) status=xr_xir_compile_program_body_match(context,program->permissions,original);
    if (status==XR_XIR_OK) {
        XrXirProgramSpec received={XR_XIR_PROGRAM_ABI_VERSION,program->permissions->target,
            program->entries,program->entry_count,program->declarations,{0},program->types,{0}};
        status=xr_xir_compile_program_descriptor_match(context,&received,program->permissions);
    }
    if (status==XR_XIR_OK) status=xir_mixed_projection_match(context,program,&projection);
    if (status!=XR_XIR_OK) {
        if (program) {
            program->permissions->input->artifact=NULL;
            xr_xir_compile_program_drop(program);
        } else if (input) {
            input->artifact=NULL;
            xir_program_input_free(input);
        }
        string_vm_code_release(code);
        return status;
    }
    /* All preparation callbacks precede the final wiring check. Failure keeps
     * the original Artifact; success transfers both owners without callbacks. */
    program->code=(XrXirCodeLease){code,string_vm_code_release};
    *artifact=NULL;*output=program;
    return XR_XIR_OK;
}
#endif // XIR_STRING_VM_PROGRAM_H
