/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_hash_foreach_mixed_code.h - Two real code leases retain one owned artifact
 */
#ifndef P09_HASH_FOREACH_MIXED_CODE_H
#define P09_HASH_FOREACH_MIXED_CODE_H
typedef struct ForeachMixedCode { XrXirArtifact *artifact; uint32_t owners; } ForeachMixedCode;
typedef struct ForeachMixedLease {
    ForeachMixedCode *code;
    XrXirVmBinding *bindings;
    XrXirCallEntry *entries;
    uint32_t releases;
} ForeachMixedLease;
static inline void foreach_code_drop(ForeachMixedCode *code) {
    CHECK(code && code->artifact && code->owners);
    if (!--code->owners) {
        xr_xir_compile_artifact_free(code->artifact); code->artifact = NULL;
    }
}
static inline void foreach_code_release(void *opaque) {
    ForeachMixedLease *lease = opaque; CHECK(lease && lease->code && !lease->releases);
    ++lease->releases;
    xr_compile_resources_free(lease->entries); xr_compile_resources_free(lease->bindings);
    lease->entries = NULL; lease->bindings = NULL;
    foreach_code_drop(lease->code); lease->code = NULL;
}
static inline uint32_t foreach_generic_callee(const XrXirModule *module, const uint32_t *entries) {
    uint32_t callee = UINT32_MAX; const XrXirFunction *root = &module->functions[entries[3]];
    for (uint32_t i = 0; i < root->instruction_count; ++i)
        if (root->instructions[i].op == XR_XIR_CALL) {
            CHECK(callee == UINT32_MAX); callee = (uint32_t)root->instructions[i].immediate;
        }
    CHECK(callee < module->function_count && callee != entries[3]); return callee;
}
static inline XrXirProgram *foreach_mixed_program(const XrXirCompileContext *context,
    ForeachMixedCode *code, ForeachMixedLease *lease, const XrXirProgramSpec *native, uint32_t entry) {
    CHECK(code && code->artifact && code->owners && code->owners < UINT32_MAX && !lease->code);
    CHECK(xr_xir_compile_vm_bind_table(code->artifact, &lease->bindings, &lease->entries) == XR_XIR_OK);
    lease->entries[entry] = native->entries[entry]; lease->code = code; ++code->owners;
    XrXirProgramSpec spec = *native; spec.entries = lease->entries;
    spec.code = (XrXirCodeLease){lease, foreach_code_release};
    XrXirProgram *program = NULL;
    XrXirStatus status = xr_xir_compile_program_seal(context, &spec, &program);
    if (status != XR_XIR_OK) foreach_code_release(lease);
    fprintf(stderr,"mixed seal status=%u\n",(unsigned)status);
    CHECK(status == XR_XIR_OK && program); return program;
}
#endif // P09_HASH_FOREACH_MIXED_CODE_H
