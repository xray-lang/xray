/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_set_algebra_mixed_code.h - Two real code leases retain one owned artifact
 */
#ifndef P09_SET_ALGEBRA_MIXED_CODE_H
#define P09_SET_ALGEBRA_MIXED_CODE_H
typedef struct AlgebraMixedCode { XrXirArtifact *artifact; uint32_t owners; } AlgebraMixedCode;
typedef struct AlgebraMixedLease {
    AlgebraMixedCode *code;
    XrXirVmBinding *bindings;
    XrXirCallEntry *entries;
    uint32_t releases;
} AlgebraMixedLease;
static inline void algebra_code_drop(AlgebraMixedCode *code) {
    CHECK(code && code->artifact && code->owners);
    if (!--code->owners) {
        xr_xir_compile_artifact_free(code->artifact); code->artifact = NULL;
    }
}
static inline void algebra_code_release(void *opaque) {
    AlgebraMixedLease *lease = opaque; CHECK(lease && lease->code && !lease->releases);
    ++lease->releases;
    xr_compile_resources_free(lease->entries); xr_compile_resources_free(lease->bindings);
    lease->entries = NULL; lease->bindings = NULL;
    algebra_code_drop(lease->code); lease->code = NULL;
}
static inline uint32_t algebra_generic_callee(const XrXirModule *module, const uint32_t *entries) {
    uint32_t callee = UINT32_MAX; const XrXirFunction *root = &module->functions[entries[3]];
    for (uint32_t i = 0; i < root->instruction_count; ++i)
        if (root->instructions[i].op == XR_XIR_CALL) {
            CHECK(callee == UINT32_MAX); callee = (uint32_t)root->instructions[i].immediate;
        }
    CHECK(callee < module->function_count && callee != entries[3]); return callee;
}
static inline XrXirProgram *algebra_mixed_program(const XrXirCompileContext *context,
    AlgebraMixedCode *code, AlgebraMixedLease *lease, const XrXirProgramSpec *native, uint32_t entry) {
    CHECK(code && code->artifact && code->owners && code->owners < UINT32_MAX && !lease->code);
    CHECK(xr_xir_compile_vm_bind_table(code->artifact, &lease->bindings, &lease->entries) == XR_XIR_OK);
    lease->entries[entry] = native->entries[entry]; lease->code = code; ++code->owners;
    XrXirProgramSpec spec = *native; spec.entries = lease->entries;
    spec.code = (XrXirCodeLease){lease, algebra_code_release};
    XrXirProgram *program = NULL;
    XrXirStatus status = xr_xir_compile_program_seal(context, &spec, &program);
    if (status != XR_XIR_OK) algebra_code_release(lease);
    CHECK(status == XR_XIR_OK && program); return program;
}
#endif // P09_SET_ALGEBRA_MIXED_CODE_H
