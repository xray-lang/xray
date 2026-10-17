/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_string_vm_program.h - Full VM binding tables retained by an immutable Program
 *
 * KEY CONCEPT:
 *   The code lease owns the artifact and binding environments until final drop.
 */
#ifndef XIR_STRING_VM_PROGRAM_H
#define XIR_STRING_VM_PROGRAM_H
#include "xir/xxir_program_packet_internal.h"
#include "xir/xxir_program.h"
#include "xir/xxir_compile_memory.h"
typedef struct StringVmCode {
    XrXirArtifact *artifact;
    XrXirVmBinding *bindings;
} StringVmCode;
static void string_vm_code_release(void *pointer) {
    StringVmCode *code=pointer;
    if (!code) return;
    xr_xir_compile_artifact_free(code->artifact);
    xr_compile_resources_free(code->bindings);
    xr_compile_resources_free(code);
}
static XrXirStatus string_vm_program_take(XrXirArtifact **artifact,
    const XrXirCallEntry *native_child, XrXirProgram **output) {
    CHECK(artifact && *artifact && output && !*output);
    const XrXirCompileContext *context=xr_xir_compile_artifact_context(*artifact);
    const XrXirModule *module=xr_xir_compile_artifact_module(*artifact);
    CHECK(module && module->function_count==5 && module->declarations &&
        module->declarations->functions[0].exported && module->declarations->functions[2].exported);
    XrXirStatus status=XR_XIR_OK;
    StringVmCode *code=xir_compile_calloc(context,1,sizeof(*code),&status);
    if (!code) return status;
    XrXirCallEntry *entries=NULL;
    status=xr_xir_compile_vm_bind_table(*artifact,&code->bindings,&entries);
    if (status==XR_XIR_OK) {
        for (uint32_t f=0;f<5;++f)
            CHECK(code->bindings[f].artifact==*artifact && code->bindings[f].function==f &&
                entries[f].environment==&code->bindings[f]);
        if (native_child) entries[1]=*native_child;
        XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,*xr_xir_compile_artifact_target(*artifact),
            entries,5,module->declarations,{code,string_vm_code_release},module->types,
            xir_program_packet_borrow(*artifact)};
        status=xr_xir_compile_program_seal(context,&spec,output);
        if (status==XR_XIR_OK) { code->artifact=*artifact;*artifact=NULL; }
    }
    xr_compile_resources_free(entries);
    if (status!=XR_XIR_OK) { CHECK(!*output);string_vm_code_release(code); }
    return status;
}
#endif // XIR_STRING_VM_PROGRAM_H
